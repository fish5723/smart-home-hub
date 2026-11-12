/**
 * @file camera_manager.c
 * @brief 摄像头管理器实现
 *
 * 管理摄像头生命周期, 提供应用层接口。
 * 预览帧通过异步方式推送到 LVGL, 确保线程安全。
 */

#include "camera_manager.h"
#include "camera.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>

/* 直接显示调试 (绕过 LVGL) */
#include "bsp/lcd/lcd.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "CAM_MGR";

/* ========== 内部状态 ========== */
static bool s_initialized      = false;
static bool s_previewing       = false;
/* 热拔插：capture 超时后置位。下一次预览前做一次受控 deinit/init，
 * 避免在已经失同步的 CSI/ISP 状态上直接重启流（实测会触发 ISP 错误风暴 +
 * Interrupt WDT panic）。 */
static bool s_camera_lost      = false;

static camera_mgr_cb_t  s_event_cb    = NULL;
static void            *s_event_ctx   = NULL;

/* LVGL 显示相关 */
static lv_obj_t        *s_cam_img      = NULL;
static lv_color_t      *s_disp_buf     = NULL;   /* 显示缓冲区 */

/* RGB565 = 2 字节/像素 (lv_color_t 在 LVGL v9 是 3 字节 RGB888, 但图像数据是 RGB565) */
#define CAM_BYTES_PER_PIX    2
#define CAM_FRAME_BYTES      (CAMERA_H_RES * CAMERA_V_RES * CAM_BYTES_PER_PIX)

/* 预览输出：PPA 硬件缩放到屏幕尺寸（1024×600），LVGL 按 1:1 贴图 */
#define DISP_W               LCD_WIDTH
#define DISP_H               LCD_HEIGHT
#define DISP_BYTES           (DISP_W * DISP_H * CAM_BYTES_PER_PIX)
/* 保持比例铺满：从 800×640 中间裁 470 行，再缩放到 1024×600 */
#define PPA_IN_H             470
#define PPA_IN_OFFSET_Y      ((CAMERA_V_RES - PPA_IN_H) / 2)

static ppa_client_handle_t s_ppa = NULL;

static volatile bool    s_frame_pending = false;  /* 有新帧待更新 */
static volatile bool    s_refresh_enabled = true; /* 动画期间可暂停刷新 */


/* ========== 帧回调 (CSI ISR 上下文) ========== */
/* 注意: ISR 中绝对不能调用 memcpy 拷贝大量数据，否则会阻塞系统。
 * CSI ping-pong 缓冲已经保证 frame_buffer 在当前帧期间不会被 DMA 覆写，
 * 我们只需记录"有新帧到达"，LVGL 定时器会从安全的 CSI 缓冲拷贝到显示缓冲。 */

static void on_camera_frame(const void *frame_buffer, size_t frame_size, void *user_data)
{
    (void)frame_buffer;
    (void)frame_size;
    (void)user_data;

    if (!s_previewing) return;

    /* 仅置标志：告知 LVGL 定时器有新鲜帧可读 */
    s_frame_pending = true;

    /* 通知外部回调 */
    if (s_event_cb) {
        s_event_cb(CAM_MGR_EVENT_FRAME_READY, s_event_ctx);
    }
}


/* ========== 公开 API ========== */

esp_err_t camera_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing camera manager...");

    esp_err_t ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 分配显示缓冲区 (LVGL 图像源, PPA 输出为屏幕尺寸 1024×600 RGB565) */
    size_t buf_size = DISP_BYTES;
    s_disp_buf = heap_caps_aligned_calloc(128, 1, buf_size,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_disp_buf) {
        ESP_LOGE(TAG, "Failed to allocate display buffer (%zu bytes)", buf_size);
        camera_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[BUF] Display buffer allocated (%zu bytes, %dx%d)", buf_size, DISP_W, DISP_H);

    /* PPA：把 800×640 硬件缩放成 1024×600，LVGL 不再做软件缩放 */
    ppa_client_config_t ppa_cfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ppa_err = ppa_register_client(&ppa_cfg, &s_ppa);
    if (ppa_err != ESP_OK) {
        ESP_LOGW(TAG, "[PPA] register failed (%s), fallback to letterbox copy",
                 esp_err_to_name(ppa_err));
        s_ppa = NULL;
    } else {
        ESP_LOGI(TAG, "[PPA] SRM ready: %dx%d → %dx%d", CAMERA_H_RES, PPA_IN_H, DISP_W, DISP_H);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "[OK] Camera manager ready");
    return ESP_OK;
}


esp_err_t camera_manager_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_previewing) {
        camera_manager_preview_stop();
    }

    if (s_disp_buf) {
        heap_caps_free(s_disp_buf);
        s_disp_buf = NULL;
    }
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    s_cam_img = NULL;
    s_frame_pending = false;
    s_camera_lost = false;

    esp_err_t ret = camera_deinit();
    s_initialized = false;

    ESP_LOGI(TAG, "Camera manager deinitialized");
    return ret;
}


esp_err_t camera_manager_register_cb(camera_mgr_cb_t cb, void *user_data)
{
    s_event_cb  = cb;
    s_event_ctx = user_data;
    return ESP_OK;
}


esp_err_t camera_manager_preview_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_previewing) {
        return ESP_OK;
    }

    /* 上一次 capture 超时说明摄像头/排线已经掉了：先完整重建，
     * 不要直接 camera_preview_start（旧 CSI/ISP 状态会刷 data type error）。 */
    if (s_camera_lost) {
        ESP_LOGW(TAG, "Camera marked lost — reinitializing before preview");
        camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_err_t rret = camera_init();
        if (rret != ESP_OK) {
            ESP_LOGW(TAG, "Camera recovery failed: %s", esp_err_to_name(rret));
            camera_deinit();   /* 清理半初始化资源，下一次事件再重试 */
            return rret;
        }
        s_camera_lost = false;
        ESP_LOGI(TAG, "[OK] Camera recovered after hot-plug");
    }

    ESP_LOGI(TAG, "Starting camera preview...");

    esp_err_t ret = camera_preview_start(on_camera_frame, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Preview start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_previewing = true;

    if (s_event_cb) {
        s_event_cb(CAM_MGR_EVENT_PREVIEW_STARTED, s_event_ctx);
    }

    ESP_LOGI(TAG, "[OK] Camera preview running");
    return ESP_OK;
}


esp_err_t camera_manager_preview_stop(void)
{
    if (!s_previewing) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping camera preview...");

    esp_err_t ret = camera_preview_stop();
    s_previewing = false;
    s_frame_pending = false;

    if (s_event_cb) {
        s_event_cb(CAM_MGR_EVENT_PREVIEW_STOPPED, s_event_ctx);
    }

    ESP_LOGI(TAG, "Camera preview stopped");
    return ret;
}


bool camera_manager_is_previewing(void)
{
    return s_previewing;
}


void camera_manager_set_refresh_enabled(bool enabled)
{
    s_refresh_enabled = enabled;
}


esp_err_t camera_manager_capture(void *buf, uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = camera_capture_frame(buf, timeout_ms);
    if (ret == ESP_ERR_TIMEOUT) {
        /* 摄像头掉线/排线接触不良：下一次预览前重建 */
        s_camera_lost = true;
    }
    return ret;
}


const void *camera_manager_get_frame(void)
{
    return camera_get_frame_buffer();
}


size_t camera_manager_get_frame_size(void)
{
    return CAMERA_FRAME_SIZE;   /* ISP 输出 RGB565，整帧 1,024,000 字节 */
}


/* ========== LVGL 图像控件辅助 ========== */

lv_obj_t *camera_manager_create_lvgl_image(lv_obj_t *parent, int x, int y)
{
    if (!s_disp_buf) return NULL;

    s_cam_img = lv_img_create(parent);
    if (!s_cam_img) return NULL;

    /* LVGL v9: 使用 lv_image_dsc_t */
    lv_image_dsc_t *img_dsc = (lv_image_dsc_t *)lv_malloc(sizeof(lv_image_dsc_t));
    if (!img_dsc) {
        lv_obj_delete(s_cam_img);
        s_cam_img = NULL;
        return NULL;
    }

    img_dsc->header.cf          = LV_COLOR_FORMAT_RGB565;
    img_dsc->header.w           = DISP_W;
    img_dsc->header.h           = DISP_H;
    img_dsc->header.magic       = LV_IMAGE_HEADER_MAGIC;
    img_dsc->header.flags       = 0;
    img_dsc->header.stride      = DISP_W * CAM_BYTES_PER_PIX;
    img_dsc->data_size          = DISP_BYTES;
    img_dsc->data               = (const uint8_t *)s_disp_buf;

    lv_image_set_src(s_cam_img, img_dsc);
    lv_obj_set_size(s_cam_img, DISP_W, DISP_H);
    lv_obj_set_pos(s_cam_img, x, y);

    ESP_LOGI(TAG, "[LVGL] Camera image created at (%d,%d) %dx%d",
             x, y, DISP_W, DISP_H);
    return s_cam_img;
}


/**
 * @brief 调试开关: 绕过 LVGL, 将 CSI 缓冲直接写入 LCD 面板
 *
 *  = 0 → 走 LVGL 图像控件 (正常显示模式, 带 V-Sync 防撕裂)
 *  = 1 → 调用 esp_lcd_panel_draw_bitmap() 直接写屏 (调试模式, 可能撕裂)
 */
#define CAMERA_BYPASS_LVGL 0

/**
 * @brief 在 LVGL 任务中刷新一帧 (由外部定时器或事件驱动)
 *
 * 应定期调用 (例如 LVGL 定时器中), 将暂存的最新帧刷新到显示。
 */
void camera_manager_lvgl_refresh(void)
{
    if (!s_previewing || !s_cam_img || !s_disp_buf) return;
    if (!s_refresh_enabled) return;
    if (!s_frame_pending) return;

    const void *csi_buf = camera_get_frame_buffer();
    if (!csi_buf) return;

    /* ESP32-P4: 从 PSRAM 读数据前必须 invalidate CPU cache */
    esp_cache_msync((void *)csi_buf, CAMERA_FRAME_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

#if CAMERA_BYPASS_LVGL
    /* ── 调试路径: 绕过 LVGL, 直接写到 LCD 面板 ── */
    {
        esp_lcd_panel_handle_t panel = lcd_get_panel();
        if (panel) {
            esp_lcd_panel_draw_bitmap(panel, 0, 0, CAMERA_H_RES, LCD_HEIGHT, csi_buf);
        }
    }
#else
    /* ── 正常 LVGL 路径 ── */
    int64_t t0 = esp_timer_get_time();

    if (s_ppa) {
        /* PPA 是 DMA 主设备：源/目的都直接读写 PSRAM，不需要 CPU cache 维护。
         * PPA 写完后只 invalidate 目的缓冲一次，丢掉 CPU 手里的旧行。 */
        ppa_srm_oper_config_t cfg = {0};
        cfg.in.buffer          = csi_buf;
        cfg.in.pic_w           = CAMERA_H_RES;
        cfg.in.pic_h           = CAMERA_V_RES;
        cfg.in.block_w         = CAMERA_H_RES;
        cfg.in.block_h         = PPA_IN_H;
        cfg.in.block_offset_x  = 0;
        cfg.in.block_offset_y  = PPA_IN_OFFSET_Y;
        cfg.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;

        cfg.out.buffer         = s_disp_buf;
        cfg.out.buffer_size    = DISP_BYTES;
        cfg.out.pic_w          = DISP_W;
        cfg.out.pic_h          = DISP_H;
        cfg.out.block_offset_x = 0;
        cfg.out.block_offset_y = 0;
        cfg.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

        cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        cfg.scale_x        = (float)DISP_W / (float)CAMERA_H_RES;   /* 1.28 */
        cfg.scale_y        = (float)DISP_H / (float)PPA_IN_H;       /* 600/470 */
        cfg.mode           = PPA_TRANS_MODE_BLOCKING;

        esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &cfg);
        if (err != ESP_OK) {
            static bool s_ppa_err_logged;
            if (!s_ppa_err_logged) {
                ESP_LOGW(TAG, "[PPA] SRM failed: %s", esp_err_to_name(err));
                s_ppa_err_logged = true;
            }
            esp_cache_msync((void *)csi_buf, CAMERA_FRAME_SIZE,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            memcpy(s_disp_buf, csi_buf, CAM_FRAME_BYTES);   /* 退化：左上角 800×640 */
            esp_cache_msync(s_disp_buf, CAM_FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        } else {
            /* PPA 已把新画面写进 PSRAM，丢弃 CPU 缓存中的旧行 */
            esp_cache_msync(s_disp_buf, DISP_BYTES,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        }
    } else {
        /* PPA 不可用：按行拷到中间（左右留黑边），保证画面仍可用 */
        esp_cache_msync((void *)csi_buf, CAMERA_FRAME_SIZE,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        const int dx = (DISP_W - CAMERA_H_RES) / 2;
        for (int y = 0; y < CAMERA_V_RES; y++) {
            memcpy((uint8_t *)s_disp_buf + ((size_t)y * DISP_W + dx) * CAM_BYTES_PER_PIX,
                   (const uint8_t *)csi_buf + (size_t)y * CAMERA_H_RES * CAM_BYTES_PER_PIX,
                   CAMERA_H_RES * CAM_BYTES_PER_PIX);
        }
        esp_cache_msync(s_disp_buf, DISP_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    lv_obj_invalidate(s_cam_img);

    /* 预览帧率/单帧刷新耗时（每秒一行，便于量化是否掉帧） */
    int64_t dt = esp_timer_get_time() - t0;
    static int64_t  s_log_at_us;
    static uint32_t s_cnt;
    static int64_t  s_sum_us;
    s_sum_us += dt;
    s_cnt++;
    int64_t now = esp_timer_get_time();
    if (s_log_at_us == 0) {
        s_log_at_us = now;
    } else if (now - s_log_at_us >= 1000000LL) {
        ESP_LOGI(TAG, "[PREVIEW] %lu fps, refresh avg %.1f ms",
                 (unsigned long)s_cnt, (double)s_sum_us / s_cnt / 1000.0);
        s_log_at_us = now;
        s_cnt = 0;
        s_sum_us = 0;
    }
#endif

    s_frame_pending = false;
}