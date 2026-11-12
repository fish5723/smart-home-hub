/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * wallpaper_player.c — JWP 动态壁纸播放器实现
 *
 * 参考来源: https://github.com/maxiumlee/ESP32-S3-LVGL-Clock (Apache-2.0),
 *           HEAD 299ab2e63ea85f3463edc8fd8fd75142573c50fa。
 * 移植范围: "SD 流式读取 → 逐帧 JPEG 解码 → RGB565 帧缓冲 → 显示" 的流水线
 *           思路与 循环/暂停 语义；分辨率、缓冲数量、帧率控制与解码后端
 *           全部按 ESP32-P4 + 1024×600 重新实现。
 *
 * 解码后端（Kconfig SMART_HOME_WALLPAPER_DECODER_*）：
 *   HW = esp_driver_jpeg（ESP-IDF 内置 P4 硬件 JPEG 编解码器，走 2D-DMA）
 *        实测性能下限见 IDF 自测：1080p→RGB565 ≥ 40fps，1024×600 余量充足。
 *   SW = esp_new_jpeg（纯软件，不占用 2D-DMA；用于与 Wi-Fi/SDIO 共存的对照实验）
 *
 * ⚠️ HW 后端使用 DMA2D（2D-DMA）外设。本工程历史上观察到 DMA2D ISR 与
 *    esp_hosted SDIO 存在冲突（见 sdkconfig.defaults 的 SMART_HOME_DMA2D_ENABLE
 *    注释）。因此 HW 后端必须实机与 Wi-Fi 并发验证；SW 后端作为安全回退。
 *
 * 边界约定（重要）:
 *  - 本模块不 include LVGL，也不创建/销毁任何 LVGL 对象。
 *  - 帧通过 frame_sink 回调以"缓冲区索引"形式交给上层；上层在 LVGL
 *    任务上下文中用 lv_async_call 映射成 lv_image_set_src（见 standby_ui.c）。
 *  - 播放器拥有 N(=3) 个 RGB565 缓冲并轮转写入；索引至少经过 3 次发布后
 *    才会被复用，配合 LVGL 的异步消费节奏避免撕裂/踩踏。
 *
 * 隔离测试：SMART_HOME_WALLPAPER_ENABLED=n 时本文件编译为空，
 *           待机层自动回落到渐变背景（standby_ui.c 里已做条件编译）。
 */

#include "wallpaper_player.h"

/* 下面两个 include 必须放在条件编译之外：DISABLED 分支的桩函数也要用 */
#include <string.h>
#include "esp_log.h"

#if CONFIG_SMART_HOME_WALLPAPER_ENABLED

#include "jwp_reader.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
#include "driver/jpeg_decode.h"
#else
#include "esp_jpeg_dec.h"
#endif

static const char *TAG = "WALLPAPER";

#ifndef CONFIG_SMART_HOME_WALLPAPER_FPS
#define CONFIG_SMART_HOME_WALLPAPER_FPS 15
#endif
#ifndef CONFIG_SMART_HOME_WALLPAPER_MAX_FRAME_KB
#define CONFIG_SMART_HOME_WALLPAPER_MAX_FRAME_KB 512
#endif

#define PLAYER_TASK_STACK   8192
#define PLAYER_TASK_PRIO    4
#define PATH_MAX_LEN        192
#define MAX_CONSEC_ERRORS   10

#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
# define DECODER_NAME "esp_driver_jpeg (P4 HW, DMA2D)"
#else
# define DECODER_NAME "esp_new_jpeg (software)"
#endif

typedef enum {
    WCMD_PLAY = 0,
    WCMD_PAUSE,
    WCMD_RESUME,
    WCMD_STOP,
    WCMD_SET_FPS,
    WCMD_EXIT,
} wallpaper_cmd_type_t;

typedef struct {
    wallpaper_cmd_type_t type;
    char                 path[PATH_MAX_LEN];
    uint32_t             fps;
} wallpaper_cmd_t;

/* ── 模块状态 ── */
static uint16_t  s_w, s_h;              /* 可见分辨率（JWP 头部） */
static uint16_t  s_padded_w, s_padded_h;/* 16 像素对齐后的解码尺寸 */
static size_t    s_buf_bytes;           /* 解码缓冲实际可用大小 */
static size_t    s_frame_bytes;         /* 可见区域大小 = w*h*2 */
static uint8_t  *s_bufs[WALLPAPER_BUFFER_COUNT];
/*
 * A bit is set while a buffer is being decoded or is owned by the display
 * side.  The old implementation used a blind 0,1,2 rotation and assumed
 * that three lv_async callbacks implied that the first buffer was reusable.
 * That assumption is not valid when LVGL/display flushing is slower than the
 * decoder.  Claim/release is atomic because release is normally performed by
 * the LVGL task while the producer runs in its own FreeRTOS task.
 */
static volatile uint32_t s_inflight_mask;
/* 每个缓冲的"发布时刻"（微秒，截断到 uint32 以保证 32 位核上的读写原子）。
 * 发布时打点、release 时取差 = 显示侧持有该缓冲的时间；0 表示该缓冲当前
 * 不处于"已发布待释放"状态（错误路径里"尚未发布就回滚"的缓冲会保持 0，不
 * 会把半截时间算进统计）。这是排查"半个 tick 拿不到空闲缓冲"的关键读数。 */
static volatile uint32_t s_publish_us[WALLPAPER_BUFFER_COUNT];
static uint8_t  *s_jpeg_in;
static size_t    s_jpeg_cap;
static QueueHandle_t s_cmd_q;
static TaskHandle_t  s_task;
static SemaphoreHandle_t s_stats_lock;
static wallpaper_frame_sink_t s_sink;
static void     *s_sink_user;
static wallpaper_loop_cb_t s_loop_cb;
static void     *s_loop_cb_user;
static volatile bool s_playing;
static volatile bool s_paused;
static volatile uint32_t s_target_fps = CONFIG_SMART_HOME_WALLPAPER_FPS;
static char      s_current_path[PATH_MAX_LEN];
/* 网络就绪门控：false 时 play 请求只暂存，避免开机阶段与 Wi-Fi 首轮突发
 * 抢 SDMMC 主机（见 wallpaper_player_set_network_ready 注释）。 */
static volatile bool s_net_ready = true;
static bool      s_has_pending;
static char      s_pending_path[PATH_MAX_LEN];
static wallpaper_stats_t s_stats;

#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
typedef jpeg_decoder_handle_t wp_decoder_t;
static jpeg_decode_cfg_t s_dec_cfg;
#else
typedef jpeg_dec_handle_t wp_decoder_t;
#endif

/* ── 对齐工具 ── */
static inline size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/* ── 缓冲分配 ── */
static void *alloc_decoded_buffer(size_t size, size_t *real_size)
{
#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
    jpeg_decode_memory_alloc_cfg_t cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    return jpeg_alloc_decoder_mem(size, &cfg, real_size);
#else
    /* 软件解码路径：LVGL 与解码器都只需 16 字节对齐 */
    size_t s = align_up(size, 64);
    if (real_size) *real_size = s;
    return heap_caps_aligned_calloc(16, 1, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void *alloc_bitstream_buffer(size_t size, size_t *real_size)
{
#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
    jpeg_decode_memory_alloc_cfg_t cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    return jpeg_alloc_decoder_mem(size, &cfg, real_size);
#else
    size_t s = align_up(size, 64);
    if (real_size) *real_size = s;
    return heap_caps_aligned_calloc(16, 1, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

/* ── 解码后端 ── */
static esp_err_t decoder_open(wp_decoder_t *out)
{
#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
    jpeg_decode_engine_cfg_t eng = {
        .intr_priority = 0,
        .timeout_ms    = 120,   /* 单帧上限；实测远小于此值 */
    };
    esp_err_t err = jpeg_new_decoder_engine(&eng, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
        return err;
    }
    /* RGB565 + BGR(小端) = 与 LVGL(LV_COLOR_DEPTH_16) 的字节序一致 */
    s_dec_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    s_dec_cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    s_dec_cfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT601;
    return ESP_OK;
#else
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type  = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.rotate       = JPEG_ROTATE_0D;
    cfg.scale.width  = 0;
    cfg.scale.height = 0;
    cfg.clipper.width  = 0;
    cfg.clipper.height = 0;
    cfg.block_enable = false;
    if (jpeg_dec_open(&cfg, out) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        return ESP_FAIL;
    }
    return ESP_OK;
#endif
}

static void decoder_close(wp_decoder_t *dec)
{
    if (!dec || !*dec) return;
#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
    jpeg_del_decoder_engine(*dec);
#else
    jpeg_dec_close(*dec);
#endif
    *dec = NULL;
}

/**
 * @brief 解码一帧 JPEG 到 out（out 大小须为 s_buf_bytes）
 * @return ESP_OK / ESP_ERR_INVALID_SIZE（尺寸不符）/ ESP_FAIL
 */
static esp_err_t decoder_process(wp_decoder_t dec, const uint8_t *jpeg, size_t jpeg_len,
                                 uint8_t *out)
{
#if CONFIG_SMART_HOME_WALLPAPER_DECODER_HW
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpeg, (uint32_t)jpeg_len, &info) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_get_info failed");
        return ESP_FAIL;
    }
    if (info.width != s_w || info.height != s_h) {
        ESP_LOGW(TAG, "frame size %lux%lu != expected %ux%u",
                 (unsigned long)info.width, (unsigned long)info.height, s_w, s_h);
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t out_len = 0;
    esp_err_t err = jpeg_decoder_process(dec, &s_dec_cfg,
                                         jpeg, (uint32_t)jpeg_len,
                                         out, (uint32_t)s_buf_bytes, &out_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(err));
    } else {
        /* The HW JPEG engine writes the PSRAM destination through DMA.  The
         * CPU/LVGL side may still have stale cache lines for a recycled
         * buffer; invalidate the complete padded allocation before publishing
         * it to the display path.  Without this barrier, old lines can be
         * mixed with the new frame, producing washed-out colours and stripes.
         */
        esp_cache_msync(out, s_buf_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }
    return err;
#else
    jpeg_dec_io_t io;
    memset(&io, 0, sizeof(io));
    io.inbuf     = (uint8_t *)jpeg;
    io.inbuf_len = (int)jpeg_len;

    jpeg_dec_header_info_t info;
    memset(&info, 0, sizeof(info));
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_parse_header failed");
        return ESP_FAIL;
    }
    if (info.width != s_w || info.height != s_h) {
        ESP_LOGW(TAG, "frame size %ux%u != expected %ux%u", info.width, info.height, s_w, s_h);
        return ESP_ERR_INVALID_SIZE;
    }
    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK || (size_t)out_len > s_buf_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    io.outbuf = out;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_process failed");
        return ESP_FAIL;
    }
    return ESP_OK;
#endif
}

/* ── 工具 ── */
static void player_close(jwp_reader_t *reader, wp_decoder_t *dec)
{
    decoder_close(dec);
    if (reader && reader->file) {
        jwp_reader_close(reader);
    }
}

static void stats_lock(void)   { if (s_stats_lock) xSemaphoreTake(s_stats_lock, portMAX_DELAY); }
static void stats_unlock(void) { if (s_stats_lock) xSemaphoreGive(s_stats_lock); }

/** Claim one free RGB565 buffer before starting a decode. */
static bool frame_try_acquire(uint32_t *out_index)
{
    if (!out_index) return false;

    uint32_t mask = __atomic_load_n(&s_inflight_mask, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t free_bits = (~mask) & ((1u << WALLPAPER_BUFFER_COUNT) - 1u);
        if (!free_bits) return false;

        /* Pick the lowest free bit.  The exact rotation is not important;
         * choosing a free buffer is what prevents a display-side overwrite. */
        uint32_t bit = free_bits & (0u - free_bits);
        uint32_t wanted = mask | bit;
        if (__atomic_compare_exchange_n(&s_inflight_mask, &mask, wanted, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            *out_index = (uint32_t)__builtin_ctz(bit);
            s_publish_us[*out_index] = 0;   /* 本次占有尚未发布 */
            return true;
        }
        /* CAS updates mask with the current value; retry against it. */
    }
}

/** Release a claimed buffer after LVGL/display has finished consuming it. */
static void frame_release_internal(uint32_t index)
{
    if (index >= WALLPAPER_BUFFER_COUNT) return;

    /* 若该缓冲已发布过，把"发布 → 释放"的存活时间记进统计（uint32 微秒的
     * 减法天然处理回绕；存活时间远小于 71 分钟）。release 可能来自播放器
     * 任务（错误回滚 / 上层 sink）或 LVGL 任务（REFR_READY），故用 volatile
     * 的 uint32 保证原子，再统一经 stats_lock 汇总。 */
    uint32_t pub = s_publish_us[index];
    if (pub) {
        s_publish_us[index] = 0;
        uint32_t hold_us = (uint32_t)esp_timer_get_time() - pub;
        stats_lock();
        s_stats.last_hold_us = hold_us;
        if (hold_us > s_stats.max_hold_us) s_stats.max_hold_us = hold_us;
        stats_unlock();
    }

    __atomic_fetch_and(&s_inflight_mask, ~(1u << index), __ATOMIC_RELEASE);
}

/* ── 播放任务 ── */
static void wallpaper_task(void *arg)
{
    (void)arg;
    jwp_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    wp_decoder_t dec = NULL;
    bool opened = false;
    int64_t next_us = 0;
    uint32_t consecutive_errors = 0;

    ESP_LOGI(TAG, "player task started: %ux%u (decode %ux%u), %u buffers, target %lu fps, decoder=%s",
             s_w, s_h, s_padded_w, s_padded_h,
             (unsigned)WALLPAPER_BUFFER_COUNT, (unsigned long)s_target_fps, DECODER_NAME);

    for (;;) {
        bool producing = opened && s_playing && !s_paused;

        TickType_t wait = portMAX_DELAY;
        if (producing) {
            int64_t now = esp_timer_get_time();
            if (next_us <= now) {
                wait = 0;
            } else {
                int64_t remain_ms = (next_us - now) / 1000;
                wait = pdMS_TO_TICKS(remain_ms > 0 ? remain_ms : 1);
            }
        }

        wallpaper_cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, wait) == pdTRUE) {
            switch (cmd.type) {
            case WCMD_PLAY: {
                player_close(&reader, &dec);
                opened = false;
                esp_err_t e = jwp_reader_open(&reader, cmd.path);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "cannot play '%s': %s", cmd.path, esp_err_to_name(e));
                    s_playing = false;
                    s_paused = false;
                    break;
                }
                if (reader.width != s_w || reader.height != s_h) {
                    ESP_LOGE(TAG, "'%s' is %ux%u, player configured for %ux%u — rejected",
                             cmd.path, reader.width, reader.height, s_w, s_h);
                    jwp_reader_close(&reader);
                    s_playing = false;
                    break;
                }
                if (decoder_open(&dec) != ESP_OK) {
                    jwp_reader_close(&reader);
                    s_playing = false;
                    break;
                }
                strlcpy(s_current_path, cmd.path, sizeof(s_current_path));
                stats_lock();
                s_stats.source_fps    = (reader.fps_times_100 + 50) / 100;
                s_stats.source_frames = reader.frame_count;
                s_stats.width         = reader.width;
                s_stats.height        = reader.height;
                s_stats.target_fps    = s_target_fps;
                stats_unlock();
                consecutive_errors = 0;
                opened    = true;
                s_paused  = false;
                s_playing = true;
                next_us   = esp_timer_get_time();
                ESP_LOGI(TAG, "playing %s (src %lu fps, %lu frames)",
                         cmd.path, (unsigned long)s_stats.source_fps,
                         (unsigned long)s_stats.source_frames);
                break;
            }
            case WCMD_PAUSE:
                s_paused = true;
                break;
            case WCMD_RESUME:
                if (opened) {
                    s_paused = false;
                    next_us = esp_timer_get_time();
                }
                break;
            case WCMD_STOP:
                player_close(&reader, &dec);
                opened = false;
                s_playing = false;
                s_paused = false;
                break;
            case WCMD_SET_FPS:
                s_target_fps = cmd.fps ? cmd.fps : 1;
                stats_lock();
                s_stats.target_fps = s_target_fps;
                stats_unlock();
                ESP_LOGI(TAG, "target fps -> %lu", (unsigned long)s_target_fps);
                break;
            case WCMD_EXIT:
                player_close(&reader, &dec);
                s_playing = false;
                s_task = NULL;
                vTaskDelete(NULL);
                return;
            }
            continue;
        }

        if (!producing) {
            continue;
        }

        /* Back-pressure happens before touching the SD card or JPEG decoder.
         * If LVGL is still displaying all buffers, skip this tick instead of
         * queuing another async callback or overwriting an in-use buffer. */
        uint32_t write_idx = 0;
        if (!frame_try_acquire(&write_idx)) {
            stats_lock();
            s_stats.frames_dropped++;
            stats_unlock();
            int64_t now = esp_timer_get_time();
            int64_t interval = 1000000 / (s_target_fps ? s_target_fps : 1);
            next_us = now + interval;
            continue;
        }

        /* ── 1. 从 SD 读一帧 JPEG ── */
        size_t jpeg_size = 0;
        /* 循环回跳是 jwp_reader 内部静默完成的（current_frame 归零重数），
         * 这里靠它前后的大小关系识别“刚播完一轮”。 */
        uint32_t loop_pos_before = reader.current_frame;
        /*
         * The display path is currently a stable 15 FPS.  When a 30 FPS JWP
         * is used, consume every second source frame so playback keeps normal
         * wall-clock speed instead of running 2x fast; importantly, this also
         * avoids feeding the display queue at a rate it cannot present.
         */
        uint32_t source_fps = reader.fps_times_100 / 100u;
        uint32_t target_fps = s_target_fps ? s_target_fps : 1u;
        uint32_t consume = (source_fps > target_fps)
                         ? (source_fps + target_fps / 2u) / target_fps : 1u;
        for (uint32_t skip = 1; skip < consume; ++skip) {
            if (jwp_reader_read_frame(&reader, s_jpeg_in, s_jpeg_cap, NULL) != ESP_OK) {
                jwp_reader_rewind_loop(&reader);
                break;
            }
        }
        esp_err_t rerr = jwp_reader_read_frame(&reader, s_jpeg_in, s_jpeg_cap, &jpeg_size);
        if (rerr == ESP_OK && s_loop_cb && reader.current_frame <= loop_pos_before) {
            /* 刚跨过一次循环回跳：通知上层（它可以借此换一段变体）。
             * 回调只投递队列命令，不会重入本任务。 */
            s_loop_cb(s_loop_cb_user);
        }
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "read frame failed: %s", esp_err_to_name(rerr));
            stats_lock();
            s_stats.decode_errors++;
            stats_unlock();
            frame_release_internal(write_idx);
            jwp_reader_rewind_loop(&reader);
            if (++consecutive_errors >= MAX_CONSEC_ERRORS) {
                ESP_LOGE(TAG, "%u consecutive read errors — stopping playback",
                         (unsigned)consecutive_errors);
                s_playing = false;
            }
            next_us = esp_timer_get_time() + (1000000 / (s_target_fps ? s_target_fps : 1));
            continue;
        }

        /* ── 2. 解码到 RGB565 ── */
        int64_t t0 = esp_timer_get_time();
        esp_err_t derr = decoder_process(dec, s_jpeg_in, jpeg_size, s_bufs[write_idx]);
        int64_t dt_us = esp_timer_get_time() - t0;

        if (derr != ESP_OK) {
            stats_lock();
            s_stats.decode_errors++;
            stats_unlock();
            frame_release_internal(write_idx);
            jwp_reader_rewind_loop(&reader);
            if (++consecutive_errors >= MAX_CONSEC_ERRORS) {
                ESP_LOGE(TAG, "%u consecutive decode errors — stopping playback",
                         (unsigned)consecutive_errors);
                s_playing = false;
            }
            next_us = esp_timer_get_time() + (1000000 / (s_target_fps ? s_target_fps : 1));
            continue;
        }

        /* ── 3. 发布帧（回调内由上层 lv_async_call 切到 LVGL 上下文） ── */
        if (s_sink) {
            /* 发布前打点（release 侧据此算"缓冲持有时间"），并测本次 sink
             * 的停留时间 —— 上层 sink 要先取 LVGL 适配器锁才能 lv_async_call，
             * 播放器会在这里被显示侧卡住；这两个读数共同定位帧率瓶颈。 */
            uint32_t t_pub = (uint32_t)esp_timer_get_time();
            s_publish_us[write_idx] = t_pub;
            s_sink(write_idx, s_sink_user);
            uint32_t sink_us = (uint32_t)esp_timer_get_time() - t_pub;
            stats_lock();
            s_stats.last_sink_us = sink_us;
            if (sink_us > s_stats.max_sink_us) s_stats.max_sink_us = sink_us;
            stats_unlock();
        } else {
            /* No consumer owns the frame.  Do not permanently exhaust the
             * pool when playback starts before standby_ui attaches its sink. */
            frame_release_internal(write_idx);
        }
        consecutive_errors = 0;

        stats_lock();
        s_stats.frames_decoded++;
        s_stats.last_decode_us = (uint32_t)dt_us;
        s_stats.avg_decode_us = (s_stats.avg_decode_us * 7u + (uint32_t)dt_us) / 8u;
        stats_unlock();

        /* ── 4. 帧率节流；解码跟不上则重置节拍并记账 ── */
        int64_t now = esp_timer_get_time();
        int64_t interval = 1000000 / (s_target_fps ? s_target_fps : 1);
        next_us += interval;
        if (next_us < now) {
            /*
             * Do not carry a stale deadline into the next loop.  Carrying it
             * forward makes a slow SD/decode burst immediately start another
             * decode, which creates a catch-up burst and visible judder.  A
             * single interval from now restores a stable cadence while the
             * sink-side coalescing keeps the newest displayed frame.
             */
            next_us = now + interval;
            stats_lock();
            s_stats.frames_dropped++;
            stats_unlock();
        }
    }
}

/* ── 公共接口 ── */
esp_err_t wallpaper_player_init(uint16_t width, uint16_t height)
{
    if (width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_w = width;
    s_h = height;
    __atomic_store_n(&s_inflight_mask, 0u, __ATOMIC_RELEASE);
    s_padded_w = (uint16_t)align_up(width, 16);
    s_padded_h = (uint16_t)align_up(height, 16);
    s_frame_bytes = (size_t)width * height * 2u;
    s_jpeg_cap  = (size_t)CONFIG_SMART_HOME_WALLPAPER_MAX_FRAME_KB * 1024u;

    for (int i = 0; i < WALLPAPER_BUFFER_COUNT; i++) {
        size_t real = 0;
        s_bufs[i] = (uint8_t *)alloc_decoded_buffer((size_t)s_padded_w * s_padded_h * 2u, &real);
        if (!s_bufs[i]) {
            ESP_LOGE(TAG, "frame buffer %d alloc failed (padded %ux%u)",
                     i, s_padded_w, s_padded_h);
            goto fail;
        }
        /* 所有缓冲必须同样大小：取最小值作为统一可用长度 */
        if (i == 0) {
            s_buf_bytes = real;
        } else if (real < s_buf_bytes) {
            s_buf_bytes = real;
        }
    }
    if (s_buf_bytes < s_frame_bytes) {
        ESP_LOGE(TAG, "decoded buffer %u B < visible frame %u B",
                 (unsigned)s_buf_bytes, (unsigned)s_frame_bytes);
        goto fail;
    }

    {
        size_t real = 0;
        s_jpeg_in = (uint8_t *)alloc_bitstream_buffer(s_jpeg_cap, &real);
        if (!s_jpeg_in) {
            ESP_LOGE(TAG, "jpeg input buffer alloc failed (%u bytes)", (unsigned)s_jpeg_cap);
            goto fail;
        }
        s_jpeg_cap = real;   /* HW 路径会按 cache line 向上对齐 */
    }

    s_stats_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(6, sizeof(wallpaper_cmd_t));
    if (!s_stats_lock || !s_cmd_q) {
        ESP_LOGE(TAG, "queue/mutex create failed");
        goto fail;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.width = width;
    s_stats.height = height;
    s_stats.target_fps = s_target_fps;

    if (xTaskCreate(wallpaper_task, "wallpaper", PLAYER_TASK_STACK,
                    NULL, PLAYER_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
        goto fail;
    }

    ESP_LOGI(TAG, "initialized: %ux%u, %u×%u B PSRAM buffers, decoder=%s",
             width, height, (unsigned)WALLPAPER_BUFFER_COUNT,
             (unsigned)s_buf_bytes, DECODER_NAME);
    return ESP_OK;

fail:
    for (int i = 0; i < WALLPAPER_BUFFER_COUNT; i++) {
        if (s_bufs[i]) { heap_caps_free(s_bufs[i]); s_bufs[i] = NULL; }
    }
    if (s_jpeg_in) { heap_caps_free(s_jpeg_in); s_jpeg_in = NULL; }
    if (s_cmd_q) { vQueueDelete(s_cmd_q); s_cmd_q = NULL; }
    if (s_stats_lock) { vSemaphoreDelete(s_stats_lock); s_stats_lock = NULL; }
    return ESP_ERR_NO_MEM;
}

void wallpaper_player_deinit(void)
{
    if (!s_task || !s_cmd_q) {
        return;
    }
    wallpaper_cmd_t cmd = { .type = WCMD_EXIT };
    xQueueSend(s_cmd_q, &cmd, portMAX_DELAY);
    for (int i = 0; i < 50 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < WALLPAPER_BUFFER_COUNT; i++) {
        if (s_bufs[i]) { heap_caps_free(s_bufs[i]); s_bufs[i] = NULL; }
    }
    if (s_jpeg_in) { heap_caps_free(s_jpeg_in); s_jpeg_in = NULL; }
    if (s_cmd_q) { vQueueDelete(s_cmd_q); s_cmd_q = NULL; }
    if (s_stats_lock) { vSemaphoreDelete(s_stats_lock); s_stats_lock = NULL; }
    s_sink = NULL;
    s_sink_user = NULL;
    __atomic_store_n(&s_inflight_mask, 0u, __ATOMIC_RELEASE);
}

uint32_t wallpaper_player_buffer_count(void) { return WALLPAPER_BUFFER_COUNT; }

const void *wallpaper_player_buffer(uint32_t index)
{
    return (index < WALLPAPER_BUFFER_COUNT) ? s_bufs[index] : NULL;
}

size_t wallpaper_player_buffer_bytes(void) { return s_frame_bytes; }

void wallpaper_player_frame_release(uint32_t index)
{
    frame_release_internal(index);
}

void wallpaper_player_set_sink(wallpaper_frame_sink_t sink, void *user)
{
    s_sink = sink;
    s_sink_user = user;
}

void wallpaper_player_set_loop_cb(wallpaper_loop_cb_t cb, void *user)
{
    s_loop_cb = cb;
    s_loop_cb_user = user;
}

esp_err_t wallpaper_player_play(const char *jwp_path)
{
    if (!jwp_path || !jwp_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 网络未就绪：只记住请求，等门控打开后补播（避免与 Wi-Fi 首轮突发抢 SDMMC） */
    if (!s_net_ready) {
        strlcpy(s_pending_path, jwp_path, sizeof(s_pending_path));
        s_has_pending = true;
        ESP_LOGI(TAG, "playback deferred until network ready: %s", jwp_path);
        return ESP_OK;
    }
    wallpaper_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = WCMD_PLAY;
    strlcpy(cmd.path, jwp_path, sizeof(cmd.path));
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, play request dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wallpaper_player_set_network_ready(bool ready)
{
    s_net_ready = ready;
    if (!ready) return;

    if (s_has_pending) {
        s_has_pending = false;
        ESP_LOGI(TAG, "network ready — starting deferred playback: %s", s_pending_path);
        (void)wallpaper_player_play(s_pending_path);
    }
}

bool wallpaper_player_network_ready(void)
{
    return s_net_ready;
}

void wallpaper_player_pause(void)
{
    if (!s_cmd_q) return;
    wallpaper_cmd_t cmd = { .type = WCMD_PAUSE };
    xQueueSend(s_cmd_q, &cmd, 0);
}

void wallpaper_player_resume(void)
{
    if (!s_cmd_q) return;
    wallpaper_cmd_t cmd = { .type = WCMD_RESUME };
    xQueueSend(s_cmd_q, &cmd, 0);
}

void wallpaper_player_stop(void)
{
    if (!s_cmd_q) return;
    wallpaper_cmd_t cmd = { .type = WCMD_STOP };
    xQueueSend(s_cmd_q, &cmd, 0);
}

bool wallpaper_player_is_playing(void) { return s_playing; }
bool wallpaper_player_is_paused(void)  { return s_paused; }

void wallpaper_player_set_target_fps(uint32_t fps)
{
    if (!s_cmd_q) return;
    wallpaper_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = WCMD_SET_FPS;
    cmd.fps  = fps;
    xQueueSend(s_cmd_q, &cmd, 0);
}

uint32_t wallpaper_player_target_fps(void) { return s_target_fps; }

void wallpaper_player_get_stats(wallpaper_stats_t *out)
{
    if (!out) return;
    stats_lock();
    *out = s_stats;
    stats_unlock();
}

#else  /* !CONFIG_SMART_HOME_WALLPAPER_ENABLED —— 隔离测试：整模块空实现 */

esp_err_t wallpaper_player_init(uint16_t width, uint16_t height)
{
    (void)width; (void)height;
    return ESP_ERR_NOT_SUPPORTED;
}
void wallpaper_player_deinit(void) {}
uint32_t wallpaper_player_buffer_count(void) { return 0; }
const void *wallpaper_player_buffer(uint32_t index) { (void)index; return NULL; }
size_t wallpaper_player_buffer_bytes(void) { return 0; }
void wallpaper_player_frame_release(uint32_t index) { (void)index; }
void wallpaper_player_set_sink(wallpaper_frame_sink_t sink, void *user) { (void)sink; (void)user; }
esp_err_t wallpaper_player_play(const char *jwp_path) { (void)jwp_path; return ESP_ERR_NOT_SUPPORTED; }
void wallpaper_player_pause(void) {}
void wallpaper_player_resume(void) {}
void wallpaper_player_stop(void) {}
bool wallpaper_player_is_playing(void) { return false; }
bool wallpaper_player_is_paused(void) { return false; }
void wallpaper_player_set_target_fps(uint32_t fps) { (void)fps; }
uint32_t wallpaper_player_target_fps(void) { return 0; }
void wallpaper_player_set_network_ready(bool ready) { (void)ready; }
bool wallpaper_player_network_ready(void) { return true; }
void wallpaper_player_get_stats(wallpaper_stats_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

#endif /* CONFIG_SMART_HOME_WALLPAPER_ENABLED */
