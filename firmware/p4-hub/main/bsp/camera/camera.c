/**
 * @file camera.c
 * @brief Camera BSP 实现 — OV5647 MIPI-CSI + ISP
 *
 * 数据流:
 *   OV5647 → MIPI-CSI 2-lane → CSI Controller (RAW8)
 *   → ISP Demosaic/CCM/BF (RAW8→RGB565)
 *   → Frame Buffer (PSRAM)
 *
 * SCCB 复用触摸 I2C_NUM_1 (GPIO7/8), 频率 10kHz
 *
 * 注: AE/AWB 使用固定预设参数, 不动态调节
 */

#include "camera.h"
#include "touch.h"

/* ESP-IDF 驱动 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
/* esp_cache_get_alignment 在 IDF 6.1 里只声明于该私有头（与 IDF 官方
 * esp_driver_cam/test_apps/csi 用例一致） */
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* MIPI-CSI 控制器 */
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"

/* ISP 核心 + 各子模块（去马赛克/降噪/gamma 等） */
#include "driver/isp.h"
#include <math.h>

/* 颜色类型 (Bayer order 枚举) */
#include "hal/color_types.h"

/* 摄像头传感器 (esp_cam_sensor 组件) */
#include "esp_cam_sensor.h"
#include "ov5647.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"

static const char *TAG = "CAMERA";

/* Gamma 曲线（与 IDF examples/peripherals/isp/multi_pipelines 默认曲线一致）：
 * 抬亮中间调，抵消当前画面整体偏暗的问题 */
static uint32_t camera_gamma_curve(uint32_t x)
{
    return (uint32_t)(pow((double)x / 256, 0.7) * 256);
}

/* ========== 内部句柄 ========== */
static esp_cam_ctlr_handle_t    s_cam_handle     = NULL;   /* CSI 控制器 */
static isp_proc_handle_t        s_isp_proc       = NULL;   /* ISP 处理器 */
static esp_cam_sensor_device_t *s_cam_sensor     = NULL;   /* 传感器设备 */
static esp_sccb_io_handle_t     s_sccb_handle    = NULL;   /* SCCB I/O */
static esp_ldo_channel_handle_t s_ldo_mipi_phy   = NULL;   /* MIPI PHY LDO */
/* headless（LCD/触摸关闭）时触摸总线不存在：SCCB 自己建一条 I2C 总线。
 * 正常模式使用触摸的共享总线，这里保持 NULL。 */
static i2c_master_bus_handle_t   s_own_i2c_bus    = NULL;

/* 双缓冲：CSI DMA 写入一个，CPU 从另一个拷贝，交替使用避免竞争 */
static void  *s_frame_buffer[2] = {NULL, NULL};  /* 帧缓冲对 (PSRAM) */
static size_t s_frame_buffer_size = 0;            /* 单帧缓冲大小 */
static int    s_frame_buffer_sel  = 0;            /* 当前 DMA 使用的缓冲索引 */

static camera_state_t s_state   = CAMERA_STATE_UNINIT;

/* 帧回调 + 预览状态 */
static camera_frame_cb_t s_frame_cb     = NULL;
static void             *s_frame_cb_ctx  = NULL;
static bool              s_previewing   = false;

/* 单次捕获同步 */
static SemaphoreHandle_t s_cap_sem       = NULL;
static void             *s_cap_buf       = NULL;
static volatile bool     s_cap_done      = false;
static volatile const void *s_cap_src    = NULL;   /* ISR 记录完成的缓冲，拷贝留到任务上下文 */
static volatile size_t   s_cap_src_len   = 0;

/* 诊断计数（只读于超时路径，便于区分“DMA 没启动”与“启动了但没完成”） */
static volatile uint32_t s_csi_new_buf_cnt  = 0;
static volatile uint32_t s_csi_finish_cnt   = 0;
static volatile uint32_t s_csi_err_cnt      = 0;
static volatile uint32_t s_csi_err_flags    = 0;

/**
 * @brief CSI 获取新帧缓冲回调 (ISR 上下文)
 *
 * 向 CSI 控制器提供空闲缓冲区。双缓冲交替，避免 DMA 写入与 CPU 读取的竞争。
 */
static bool IRAM_ATTR csi_get_new_buffer(esp_cam_ctlr_handle_t handle,
                                          esp_cam_ctlr_trans_t *trans,
                                          void *user_data)
{
    (void)handle;
    (void)user_data;
    s_csi_new_buf_cnt++;
    /* 切换到另一缓冲：DMA 写入下一个，CPU 从上一个拷贝 */
    s_frame_buffer_sel ^= 1;
    trans->buffer = s_frame_buffer[s_frame_buffer_sel];
    trans->buflen = s_frame_buffer_size;
    /* 与 IDF test_apps/csi/test_csi_ov5647.c 一致返回 false：
     * 返回 true 会被驱动理解为“回调已自行排队新事务”，实测拿不到 trans_finished。 */
    return false;
}

/**
 * @brief CSI 错误回调 (ISR 上下文)：只置位计数，由任务侧打印
 */
static bool IRAM_ATTR csi_on_error(esp_cam_ctlr_handle_t handle,
                                    const esp_cam_ctlr_error_event_data_t *edata,
                                    void *user_data)
{
    (void)handle;
    (void)user_data;
    s_csi_err_cnt++;
    if (edata) s_csi_err_flags = edata->csi_host_err_evts;
    return false;
}

/**
 * @brief CSI 帧传输完成回调 (ISR 上下文)
 *
 * 通知上层新帧已就绪。
 */
static bool IRAM_ATTR csi_frame_finished(esp_cam_ctlr_handle_t handle,
                                          esp_cam_ctlr_trans_t *trans,
                                          void *user_data)
{
    (void)handle;
    (void)user_data;
    s_csi_finish_cnt++;
    /* 预览回调 */
    if (s_previewing && s_frame_cb) {
        s_frame_cb(trans->buffer, CAMERA_FRAME_SIZE, s_frame_cb_ctx);
    }

    /* 单次捕获同步：ISR 里只记录完成缓冲，512KB+ 的 memcpy 必须留给任务上下文，
     * 否则会在中断里长时间阻塞（实测会 panic）。
     * 注意：驱动按 CSI 输入格式(RAW8)算出的 received_size 只有 512,000，
     * 但 ISP 输出 RGB565，DMA 实际写满一整帧 1,024,000 字节（已实录验证），
     * 因此这里统一按 CAMERA_FRAME_SIZE 处理，不用 received_size。 */
    if (s_cap_buf && !s_cap_done) {
        s_cap_src     = trans->buffer;
        s_cap_src_len = CAMERA_FRAME_SIZE;
        s_cap_done    = true;
        if (s_cap_sem) {
            BaseType_t need_yield = pdFALSE;
            xSemaphoreGiveFromISR(s_cap_sem, &need_yield);
            if (need_yield) portYIELD_FROM_ISR();
        }
    }

    return false;
}


/* ========== 主初始化 ========== */

esp_err_t camera_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Camera init: OV5647 MIPI-CSI 2-lane, %dx%d, %s",
             CAMERA_H_RES, CAMERA_V_RES, CAMERA_FORMAT_NAME);

    s_cap_sem = xSemaphoreCreateBinary();
    if (!s_cap_sem) return ESP_ERR_NO_MEM;

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = CAMERA_MIPI_LDO_CHAN,
        .voltage_mv = CAMERA_MIPI_LDO_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_mipi_phy);
    if (ret != ESP_OK) return ret;

    size_t align = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align) != ESP_OK || align == 0) {
        align = 128;
    }
    s_frame_buffer_size = CAMERA_FRAME_SIZE;
    s_frame_buffer_sel  = 0;
    for (int i = 0; i < 2; i++) {
        s_frame_buffer[i] = heap_caps_aligned_calloc(align, 1, s_frame_buffer_size,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frame_buffer[i]) return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[BUF] %zu bytes x2 @ %p / %p (PSRAM, align %zu)",
             s_frame_buffer_size, s_frame_buffer[0], s_frame_buffer[1], align);

    i2c_master_bus_handle_t bus = touch_get_i2c_bus();
    if (!bus) {
        /* headless：触摸/显示未初始化，SCCB 需要自己建一条总线。
         * 引脚与触摸共用 GPIO7/8，端口同为 I2C_NUM_1；正常模式不会走到这里。 */
        i2c_master_bus_config_t icfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = CONFIG_SMART_HOME_TOUCH_I2C_SDA,
            .scl_io_num = CONFIG_SMART_HOME_TOUCH_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 15,
            .flags.enable_internal_pullup = true,
        };
        ret = i2c_new_master_bus(&icfg, &s_own_i2c_bus);
        if (ret != ESP_OK) return ret;
        bus = s_own_i2c_bus;
        ESP_LOGI(TAG, "[SCCB] created private I2C bus (headless)");
    }
    if (!bus) return ESP_ERR_INVALID_STATE;
    sccb_i2c_config_t sc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x36,
        .scl_speed_hz    = CAMERA_SCCB_FREQ_HZ,
    };
    ret = sccb_new_i2c_io(bus, &sc, &s_sccb_handle);
    if (ret != ESP_OK) return ret;

    esp_cam_sensor_config_t scfg = {
        .reset_pin   = -1,
        .pwdn_pin    = -1,
        .xclk_pin    = -1,
        .sccb_handle = s_sccb_handle,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    s_cam_sensor = ov5647_detect(&scfg);
    if (!s_cam_sensor) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "[SENSOR] OV5647 detected");

    esp_cam_sensor_format_array_t fa = {0};
    ret = esp_cam_sensor_query_format(s_cam_sensor, &fa);
    if (ret != ESP_OK) return ret;
    const esp_cam_sensor_format_t *fmt = NULL;
    for (int i = 0; i < fa.count; i++) {
        if (!strcmp(fa.format_array[i].name, CAMERA_FORMAT_NAME)) {
            fmt = &fa.format_array[i];
        }
    }
    if (!fmt) return ESP_ERR_NOT_SUPPORTED;
    ret = esp_cam_sensor_set_format(s_cam_sensor, fmt);
    if (ret != ESP_OK) return ret;

    /* 注意：OV5647 在 set_format 之后写 0x3503/0x3501/0x3502/0x350B（固定曝光）
     * 会让传感器停止输出（实测一帧都收不到），因此不做传感器后配置，
     * 曝光/增益交给传感器自带的 AEC/AGC。 */
    int en = 1;
    ret = esp_cam_sensor_ioctl(s_cam_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &en);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "[SENSOR] format=%s, stream on", fmt->name);

    esp_cam_ctlr_csi_config_t cc = {
        .ctlr_id                = 0,
        .h_res                  = CAMERA_H_RES,
        .v_res                  = CAMERA_V_RES,
        .lane_bit_rate_mbps     = 200,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_lane_num          = 2,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    ret = esp_cam_new_csi_ctlr(&cc, &s_cam_handle);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "[CSI] controller created");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = csi_get_new_buffer,
        .on_trans_finished = csi_frame_finished,
        .on_error          = csi_on_error,
    };
    ret = esp_cam_ctlr_register_event_callbacks(s_cam_handle, &cbs, NULL);
    if (ret != ESP_OK) return ret;

    isp_proc_handle_t isp = NULL;
    esp_isp_processor_cfg_t ic = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = false,
        .has_line_end_packet    = false,
        .h_res                  = CAMERA_H_RES,
        .v_res                  = CAMERA_V_RES,
        .bayer_order            = COLOR_RAW_ELEMENT_ORDER_GBRG,
    };
    /* 临时诊断：保留 ISP 路径（关掉 ISP 时一帧都收不到，说明 ISP 在 DMA 数据路径上） */
    ret = esp_isp_new_processor(&ic, &isp);
    if (ret != ESP_OK) return ret;
    ret = esp_isp_enable(isp);
    if (ret != ESP_OK) return ret;
    s_isp_proc = isp;
    ESP_LOGI(TAG, "[ISP] enabled (RAW8 -> RGB565)");

    /* ISP 子模块：BF 降噪 → Demosaic 彩色还原 → Gamma 提亮。
     * 默认只开基础管线时输出接近灰度且噪点明显（实测），这里补齐。 */
    esp_isp_bf_config_t bf_cfg = {
        .denoising_level = 5,
        .padding_mode    = ISP_BF_EDGE_PADDING_MODE_SRND_DATA,
        .bf_template     = {
            {1, 2, 1},
            {2, 4, 2},
            {1, 2, 1},
        },
        .padding_line_tail_valid_start_pixel = 0,
        .padding_line_tail_valid_end_pixel   = 0,
    };
    if (esp_isp_bf_configure(isp, &bf_cfg) == ESP_OK && esp_isp_bf_enable(isp) == ESP_OK) {
        ESP_LOGI(TAG, "[ISP] BF (denoise) enabled");
    } else {
        ESP_LOGW(TAG, "[ISP] BF configure/enable failed");
    }

    esp_isp_demosaic_config_t dm_cfg = {
        .grad_ratio = { .integer = 2, .decimal = 5 },
    };
    if (esp_isp_demosaic_configure(isp, &dm_cfg) == ESP_OK && esp_isp_demosaic_enable(isp) == ESP_OK) {
        ESP_LOGI(TAG, "[ISP] demosaic enabled");
    } else {
        ESP_LOGW(TAG, "[ISP] demosaic configure/enable failed");
    }

    isp_gamma_curve_points_t gamma_pts = {};
    if (esp_isp_gamma_fill_curve_points(camera_gamma_curve, &gamma_pts) == ESP_OK) {
        esp_isp_gamma_configure(isp, COLOR_COMPONENT_R, &gamma_pts);
        esp_isp_gamma_configure(isp, COLOR_COMPONENT_G, &gamma_pts);
        esp_isp_gamma_configure(isp, COLOR_COMPONENT_B, &gamma_pts);
        if (esp_isp_gamma_enable(isp) == ESP_OK) {
            ESP_LOGI(TAG, "[ISP] gamma enabled");
        }
    }

    s_state = CAMERA_STATE_IDLE;
    ESP_LOGI(TAG, "[OK] Camera init complete");
    return ESP_OK;
}




esp_err_t camera_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing camera...");

    if (s_previewing) {
        camera_preview_stop();
    }

    if (s_isp_proc) {
        esp_isp_disable(s_isp_proc);
        esp_isp_del_processor(s_isp_proc);
        s_isp_proc = NULL;
    }

    if (s_cam_handle) {
        esp_cam_ctlr_disable(s_cam_handle);
        esp_cam_ctlr_del(s_cam_handle);
        s_cam_handle = NULL;
    }

    if (s_cam_sensor) {
        int disable = 0;
        esp_cam_sensor_ioctl(s_cam_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &disable);
        s_cam_sensor = NULL;
    }

    if (s_sccb_handle) {
        esp_sccb_del_i2c_io(s_sccb_handle);
        s_sccb_handle = NULL;
    }

    if (s_own_i2c_bus) {
        i2c_del_master_bus(s_own_i2c_bus);
        s_own_i2c_bus = NULL;
    }

    for (int i = 0; i < 2; i++) {
        if (s_frame_buffer[i]) {
            heap_caps_free(s_frame_buffer[i]);
            s_frame_buffer[i] = NULL;
        }
    }
    s_frame_buffer_size = 0;

    if (s_ldo_mipi_phy) {
        esp_ldo_release_channel(s_ldo_mipi_phy);
        s_ldo_mipi_phy = NULL;
    }

    if (s_cap_sem) {
        vSemaphoreDelete(s_cap_sem);
        s_cap_sem = NULL;
    }

    s_state = CAMERA_STATE_UNINIT;
    s_frame_cb = NULL;
    s_frame_cb_ctx = NULL;

    ESP_LOGI(TAG, "Camera deinitialized");
    return ESP_OK;
}


camera_state_t camera_get_state(void)
{
    return s_state;
}


esp_err_t camera_preview_start(camera_frame_cb_t cb, void *user_data)
{
    if (s_state != CAMERA_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_cam_handle || !s_isp_proc) {
        return ESP_ERR_INVALID_STATE;
    }

    s_frame_cb    = cb;
    s_frame_cb_ctx = user_data;
    s_previewing   = true;
    s_cap_buf      = NULL;
    s_cap_done     = false;

    /* 传感器输出流在 camera_init 阶段开启，预览只启停 CSI。
     * start() 内部会通过 on_get_new_trans 回调拿到首个缓冲并发起 DMA，
     * 不要再调用 esp_cam_ctlr_receive()：该 API 的队列只有 1 格且不会被回调模式消费，
     * 第二次调用会永久阻塞（实测第二次 capture 卡死）。 */
    esp_err_t ret = esp_cam_ctlr_enable(s_cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI enable failed: %s", esp_err_to_name(ret));
        goto stop_stream;
    }
    ret = esp_cam_ctlr_start(s_cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI start failed: %s", esp_err_to_name(ret));
        goto stop_stream;
    }

    /* 与官方用例一致补一次 receive（timeout=0：回调模式下队列不会被消费，
     * 用非阻塞避免第二次调用永久挂起） */
    {
        esp_cam_ctlr_trans_t trans = {
            .buffer = s_frame_buffer[0],
            .buflen = s_frame_buffer_size,
        };
        esp_cam_ctlr_receive(s_cam_handle, &trans, 0);
    }

    s_state = CAMERA_STATE_PREVIEW;
    ESP_LOGI(TAG, "[OK] Camera preview started");
    return ESP_OK;

stop_stream:
    {
        int disable = 0;
        esp_cam_sensor_ioctl(s_cam_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &disable);
    }
    s_previewing = false;
    return ret;
}


esp_err_t camera_preview_stop(void)
{
    if (s_state != CAMERA_STATE_PREVIEW) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret_csi = esp_cam_ctlr_stop(s_cam_handle);
    if (ret_csi != ESP_OK) {
        ESP_LOGW(TAG, "CSI stop: %s", esp_err_to_name(ret_csi));
    }
    esp_cam_ctlr_disable(s_cam_handle);

    s_previewing = false;
    s_state = CAMERA_STATE_IDLE;
    s_frame_cb = NULL;
    s_frame_cb_ctx = NULL;

    ESP_LOGI(TAG, "Camera preview stopped");
    return ESP_OK;
}


esp_err_t camera_capture_frame(void *out_buf, uint32_t timeout_ms)
{
    if (!out_buf) return ESP_ERR_INVALID_ARG;
    if (s_state != CAMERA_STATE_IDLE && s_state != CAMERA_STATE_PREVIEW) {
        return ESP_ERR_INVALID_STATE;
    }

    bool was_previewing = s_previewing;

    if (!was_previewing) {
        s_cap_buf  = out_buf;
        s_cap_done = false;
        /* 清掉上一次残留的信号，避免本次 capture 立刻返回却拿不到新帧 */
        xSemaphoreTake(s_cap_sem, 0);

        /* 传感器流在 camera_init 里已开启，这里只启停 CSI。
         * start() 会通过回调拿到首个缓冲并发起 DMA，无需再调用 receive()。 */
        esp_err_t ret = esp_cam_ctlr_enable(s_cam_handle);
        if (ret != ESP_OK) return ret;
        ret = esp_cam_ctlr_start(s_cam_handle);
        if (ret != ESP_OK) return ret;

        /* 与官方用例一致补一次 receive（timeout=0，避免队列满时阻塞） */
        {
            esp_cam_ctlr_trans_t trans = {
                .buffer = s_frame_buffer[0],
                .buflen = s_frame_buffer_size,
            };
            esp_cam_ctlr_receive(s_cam_handle, &trans, 0);
        }

        s_state = CAMERA_STATE_PREVIEW;
    } else {
        s_cap_buf  = out_buf;
        s_cap_done = false;
    }

    TickType_t ticks = (timeout_ms == UINT32_MAX) ?
                        portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    BaseType_t taken = xSemaphoreTake(s_cap_sem, ticks);

    if (taken == pdTRUE && s_cap_src != NULL) {
        /* 任务上下文里做 cache invalidate + 拷贝（ISR 只记指针） */
        size_t n = (s_cap_src_len < s_frame_buffer_size) ? s_cap_src_len
                                                          : s_frame_buffer_size;
        esp_cache_msync((void *)s_cap_src, n,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        memcpy(out_buf, (const void *)s_cap_src, n);
        /* 拷进 PSRAM 的调用方缓冲后，刷回 PSRAM，保证后续 CPU/DMA 读到新数据 */
        esp_cache_msync(out_buf, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    if (!was_previewing) {
        esp_cam_ctlr_stop(s_cam_handle);
        esp_cam_ctlr_disable(s_cam_handle);
        s_state = CAMERA_STATE_IDLE;
    }

    s_cap_buf  = NULL;
    s_cap_src  = NULL;
    s_cap_src_len = 0;

    if (taken != pdTRUE) {
        s_cap_done = false;
        ESP_LOGW(TAG, "capture timeout: new_buf=%u finished=%u err=%u flags=0x%x",
                 (unsigned)s_csi_new_buf_cnt, (unsigned)s_csi_finish_cnt,
                 (unsigned)s_csi_err_cnt, (unsigned)s_csi_err_flags);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


const void *camera_get_frame_buffer(void)
{
    /* 返回最近完成的一帧 (当前未由 DMA 写入的那一个) */
    return s_frame_buffer[s_frame_buffer_sel ^ 1];
}
