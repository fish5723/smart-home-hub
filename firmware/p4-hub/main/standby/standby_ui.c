/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * standby_ui.c — 全屏待机层实现（时钟 / 日期 / Wi-Fi / 天气 / 动态壁纸）
 *
 * 线程模型：
 *   - 只有 update_* / notify_activity 可从任意任务调用，
 *     它们仅写入互斥保护的状态快照，不触碰 LVGL。
 *   - 所有 LVGL 绘制集中在 1s lv_timer 与 lv_async_call 回调里（LVGL 任务上下文）。
 *
 * 隔离测试（Kconfig）：
 *   SMART_HOME_STANDBY_ENABLED        —— 整个待机层的总开关（关闭则本文件为空）
 *   SMART_HOME_WALLPAPER_ENABLED      —— 关闭后待机层只用渐变背景，不依赖播放器
 *   SMART_HOME_WEATHER_ENABLED        —— 关闭后不启天气服务（时间/日期照常）
 *   SMART_HOME_STANDBY_SELFTEST       —— 每 5s 打一条自检日志（时钟有效性/天气/
 *                                        场景/壁纸帧率），便于单模块上板验证
 *   SMART_HOME_WALLPAPER_TEST_SCENE   —— 非空时强制使用该场景壁纸，绕过天气
 */

#include "standby_ui.h"

#if CONFIG_SMART_HOME_STANDBY_ENABLED

#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"

#include "ui_tokens.h"
#include "ui_pages.h"
#include "app_proto.h"
#include "protocol.h"
#include "storage.h"
#include "images/reicon_nav_assets.h"
#include "wifi.h"
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
#include "wallpaper_player.h"
#endif

/* 待机层最多接多少个播放器帧缓冲（与 wallpaper_player 的 3 缓冲对应） */
#define STANDBY_WALL_MAX_FRAMES 3

static const char *TAG = "STANDBY";

#ifndef CONFIG_SMART_HOME_STANDBY_IDLE_SEC
#define CONFIG_SMART_HOME_STANDBY_IDLE_SEC 60
#endif
#ifndef CONFIG_SMART_HOME_STANDBY_DOUBLE_CLICK_MS
#define CONFIG_SMART_HOME_STANDBY_DOUBLE_CLICK_MS 400
#endif
#ifndef CONFIG_SMART_HOME_WALLPAPER_TEST_SCENE
#define CONFIG_SMART_HOME_WALLPAPER_TEST_SCENE "sunny"
#endif
#ifndef CONFIG_SMART_HOME_WALLPAPER_TEST_NIGHT
#define CONFIG_SMART_HOME_WALLPAPER_TEST_NIGHT 0
#endif

#define COLOR_INK    0x0C0C0B
#define COLOR_CREAM  0xF3EFE7
#define COLOR_MUTED  0xAAA39A
#define COLOR_METAL  0xC69A59
#define COLOR_OK     0x9AD29A

LV_FONT_DECLARE(lv_font_montserrat_48);

/* ══════════════════════════════════════════════════════════════════
 * 视频窗口 / 媒体卡圆角 = 直角（性能取舍，2026-10-19 用户拍板）
 *
 * 只要这块区域参与每帧重绘而又带圆角，LVGL 就要为它生成圆角 mask；
 * 壁纸每帧整块失效 → 每帧都付这笔钱。适配器的 PPA 路径既明确拒绝复杂
 * mask，又要求目标图层是 RGB565，于是整块退回 CPU 软件混合。
 * 实测（P4 / JD9165BA / DOUBLE_DIRECT，同一台机器）：
 *   - radius 22/28 + clip_corner   ：draw 48ms，dispfps 17，drop 44/5s
 *   - radius 22/28 + 静态角片      ：draw 14~34ms，dispfps 20，drop 50/5s
 *     （角片方案已试过并放弃，见 commit 说明）
 *   - radius 与 clip_corner 全部归零：draw 13ms，dispfps 30，drop 3/5s
 * 故本轮直接采用直角。若将来要恢复圆角，必须让圆角不参与每帧重绘
 * （例如把圆角直接烤进 JWP 帧像素），不要改回 clip_corner。
 * ══════════════════════════════════════════════════════════════════ */
#define COLOR_CARD     0x151514
#define VIDEO_W        (TK_CANVAS_W - 88)
#define VIDEO_H        420

/* ── 状态 ── */
static lv_obj_t *s_cover, *s_bg, *s_card, *s_video, *s_footer, *s_wall;
static lv_obj_t *s_time, *s_temp, *s_weather_icon;
static lv_obj_t *s_badge, *s_badge_label;
static lv_timer_t *s_tick;
static bool     s_visible;
static uint32_t s_last_activity_ms;
static uint32_t s_last_click_ms;
static bool     s_activity_hook_installed;
static bool     s_time_valid;

static SemaphoreHandle_t s_lock;
static weather_state_t   s_wx;

static weather_scene_t s_applied_scene = WEATHER_SCENE_UNKNOWN;
static bool            s_applied_night;
static bool            s_scene_applied;

static lv_image_dsc_t s_dsc[STANDBY_WALL_MAX_FRAMES];
static uint32_t       s_dsc_count;
static bool           s_wallpaper_attached;
static bool           s_first_frame_shown;
static uint32_t       s_displayed_frame = UINT32_MAX;
static uint32_t       s_pending_frame = UINT32_MAX;
static bool           s_frame_async_queued;
static uint32_t       s_retired_frames_mask;
static SemaphoreHandle_t s_frame_lock;

#if CONFIG_SMART_HOME_STANDBY_SELFTEST
/* 一帧显示周期的分段计时（壁纸帧率排查）。用 LVGL 公开的 display 事件切分：
 *   REFR_START → RENDER_START   = prep  （layout / 失效区合并 / 双缓冲同步区准备）
 *   RENDER_START → RENDER_READY = draw  （真正画像素，直写模式下含同步区重绘）
 *   RENDER_READY → REFR_READY   = flush （提交 + 等面板帧缓冲切换完成）
 * 三者之和 ≈ 显示帧周期（1 / dispfps），用来判断瓶颈落在哪一段。 */
static uint32_t s_t_refr_start_us;
static uint32_t s_t_render_start_us;
static uint32_t s_t_render_ready_us;
static uint32_t s_last_prep_us;
static uint32_t s_last_draw_us;
static uint32_t s_last_flush_us;
#endif

#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
static void release_retired_frames(void)
{
    uint32_t retired = 0;
    if (s_frame_lock) xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    retired = s_retired_frames_mask;
    s_retired_frames_mask = 0;
    if (s_frame_lock) xSemaphoreGive(s_frame_lock);
    for (uint32_t i = 0; i < STANDBY_WALL_MAX_FRAMES; ++i) {
        if (retired & (1u << i)) wallpaper_player_frame_release(i);
    }
}
#endif

static standby_scene_cb_t s_scene_cb;
static void              *s_scene_cb_user;

/* ── 快照读写 ── */
static void snapshot_take(weather_state_t *wx)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (wx)   *wx = s_wx;
    if (s_lock) xSemaphoreGive(s_lock);
}

static void snapshot_put_weather(const weather_state_t *wx)
{
    if (!wx) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wx = *wx;
    if (s_lock) xSemaphoreGive(s_lock);
}

/* ── 场景 → 渐变配色（无壁纸/无网络时的兜底静态背景） ── */
static void scene_colors(weather_scene_t scene, bool night, uint32_t *top, uint32_t *bottom)
{
    switch (scene) {
    case WEATHER_SCENE_CLEAR:
        if (night) { *top = 0x0B1024; *bottom = 0x1B2A4A; }
        else       { *top = 0x2E6FB7; *bottom = 0x9CC8E8; }
        break;
    case WEATHER_SCENE_CLOUDY:   *top = 0x3A4450; *bottom = 0x707C88; break;
    case WEATHER_SCENE_OVERCAST: *top = 0x2F3841; *bottom = 0x5B6773; break;
    case WEATHER_SCENE_RAIN:     *top = 0x1E2A33; *bottom = 0x4A5A66; break;
    case WEATHER_SCENE_THUNDER:  *top = 0x14141F; *bottom = 0x3A3A50; break;
    case WEATHER_SCENE_SNOW:     *top = 0x38485A; *bottom = 0xA8BBC9; break;
    case WEATHER_SCENE_FOG:      *top = 0x45453F; *bottom = 0x8A8A80; break;
    case WEATHER_SCENE_HAZE:     *top = 0x4A4234; *bottom = 0x8A7E66; break;
    case WEATHER_SCENE_NIGHT:    *top = 0x080C18; *bottom = 0x1A2038; break;
    default:
        if (night) { *top = 0x0B1024; *bottom = 0x1B2A4A; }
        else       { *top = 0x243B5A; *bottom = 0x6E88A8; }
        break;
    }
}

static bool hour_is_night(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int h = tmv.tm_hour;
    return (h < 6 || h >= 18);
}

void standby_ui_set_scene(weather_scene_t scene, bool is_night)
{
    if (!s_bg && !s_video) return;
    uint32_t top, bottom;
    scene_colors(scene, is_night, &top, &bottom);
    /* Keep the outside shell opaque to prevent the previous page leaking
     * through; only the video viewport receives the scene fallback gradient. */
    if (s_bg) {
        lv_obj_set_style_bg_color(s_bg, lv_color_hex(COLOR_INK), 0);
        lv_obj_set_style_bg_grad_color(s_bg, lv_color_hex(COLOR_INK), 0);
    }
    if (s_video) {
        lv_obj_set_style_bg_color(s_video, lv_color_hex(top), 0);
        lv_obj_set_style_bg_grad_color(s_video, lv_color_hex(bottom), 0);
    }
    s_applied_scene = scene;
    s_applied_night = is_night;
    s_scene_applied = true;
}

void standby_ui_set_scene_callback(standby_scene_cb_t cb, void *user)
{
    s_scene_cb = cb;
    s_scene_cb_user = user;
}

/* ── 时间渲染（LVGL 任务上下文） ──
 * 待机层只保留时钟：中文日期放不进 66px 页脚（原 y=73 会被裁掉），
 * 按"待机层不要中文日期"的定稿直接去掉。 */
void standby_ui_update_time(void)
{
    if (!s_time) return;

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int year = tmv.tm_year + 1900;
    s_time_valid = (year >= 2024);

    if (!s_time_valid) {
        lv_label_set_text(s_time, "--:--");
        return;
    }

    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(s_time, buf);
}

bool standby_ui_time_valid(void) { return s_time_valid; }

/* ── 壁纸帧接入 ── */
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED

static void frame_async_cb(void *arg)
{
    uint32_t idx = (uint32_t)(uintptr_t)arg;
    if (s_frame_lock) xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    s_frame_async_queued = false;
    if (!s_visible || idx >= s_dsc_count || !s_wall || !s_video) {
        wallpaper_player_frame_release(idx);
        if (s_pending_frame != UINT32_MAX) {
            wallpaper_player_frame_release(s_pending_frame);
            s_pending_frame = UINT32_MAX;
        }
        if (s_frame_lock) xSemaphoreGive(s_frame_lock);
        return;
    }
    lv_image_set_src(s_wall, &s_dsc[idx]);
    /* The source is already 1024×600.  Size/position are configured once
     * when the image is created; repeating them for every frame causes extra
     * layout invalidation and needlessly extends the display flush window. */
    if (s_displayed_frame != UINT32_MAX && s_displayed_frame != idx) {
        s_retired_frames_mask |= (1u << s_displayed_frame);
    }
    s_displayed_frame = idx;
    if (!s_first_frame_shown) {
        lv_obj_clear_flag(s_wall, LV_OBJ_FLAG_HIDDEN);
        s_first_frame_shown = true;
        ESP_LOGI(TAG, "first wallpaper frame displayed");
    }

    if (s_pending_frame != UINT32_MAX) {
        uint32_t next = s_pending_frame;
        s_pending_frame = UINT32_MAX;
        s_frame_async_queued = true;
        if (lv_async_call(frame_async_cb, (void *)(uintptr_t)next) == LV_RESULT_OK) {
            /* Keep the queue flag set until the chained callback consumes it. */
        } else {
            s_frame_async_queued = false;
            wallpaper_player_frame_release(next);
        }
    }
    if (s_frame_lock) xSemaphoreGive(s_frame_lock);
}

static void wallpaper_sink_cb(uint32_t buffer_index, void *user)
{
    (void)user;
    if (s_frame_lock) xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    if (!s_visible) {
        wallpaper_player_frame_release(buffer_index);
        if (s_frame_lock) xSemaphoreGive(s_frame_lock);
        return;
    }
    /* Coalesce async callbacks.  The newest frame wins; a replaced frame was
     * decoded successfully but never handed to LVGL, so release it now. */
    if (s_frame_async_queued) {
        if (s_pending_frame != UINT32_MAX) {
            wallpaper_player_frame_release(s_pending_frame);
        }
        s_pending_frame = buffer_index;
        if (s_frame_lock) xSemaphoreGive(s_frame_lock);
        return;
    }
    /* Reserve the callback slot while protected, then release the frame
     * mutex before taking the LVGL adapter lock. This keeps lock ordering
     * consistent with frame_async_cb (LVGL -> frame mutex). */
    s_frame_async_queued = true;
    s_pending_frame = UINT32_MAX;
    if (s_frame_lock) xSemaphoreGive(s_frame_lock);

    /* wallpaper_player runs outside the LVGL task. lv_async_call mutates
     * LVGL's timer list, so serialize that call with the adapter task. */
    esp_err_t lv_lock = esp_lv_adapter_lock(-1);
    lv_result_t async_result = LV_RESULT_INVALID;
    if (lv_lock == ESP_OK) {
        async_result = lv_async_call(frame_async_cb, (void *)(uintptr_t)buffer_index);
        esp_lv_adapter_unlock();
    }
    if (async_result != LV_RESULT_OK) {
        if (s_frame_lock) xSemaphoreTake(s_frame_lock, portMAX_DELAY);
        wallpaper_player_frame_release(buffer_index);
        s_frame_async_queued = false;
        if (s_frame_lock) xSemaphoreGive(s_frame_lock);
        return;
    }
}

static void display_refr_ready_cb(lv_event_t *e)
{
    (void)e;
#if CONFIG_SMART_HOME_STANDBY_SELFTEST
    if (s_t_render_ready_us) {
        uint32_t now = (uint32_t)esp_timer_get_time();
        uint32_t d = now - s_t_render_ready_us;
        if (d) s_last_flush_us = d;
    }
#endif
    release_retired_frames();
}

#if CONFIG_SMART_HOME_STANDBY_SELFTEST
/* 渲染阶段分段计时（只测量，不改行为） */
static void display_render_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (code == LV_EVENT_REFR_START) {
        s_t_refr_start_us = now;
    } else if (code == LV_EVENT_RENDER_START) {
        s_t_render_start_us = now;
        if (s_t_refr_start_us) s_last_prep_us = now - s_t_refr_start_us;
    } else if (code == LV_EVENT_RENDER_READY) {
        s_t_render_ready_us = now;
        if (s_t_render_start_us) s_last_draw_us = now - s_t_render_start_us;
    }
}
#endif

void standby_ui_attach_wallpaper(void)
{
    if (s_wallpaper_attached || !s_wall) return;

    lv_display_t *disp = lv_display_get_default();
    int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : TK_CANVAS_W;
    int32_t h = disp ? lv_display_get_vertical_resolution(disp) : TK_CANVAS_H;
    size_t bytes = wallpaper_player_buffer_bytes();

    uint32_t n = wallpaper_player_buffer_count();
    if (n > STANDBY_WALL_MAX_FRAMES) n = STANDBY_WALL_MAX_FRAMES;
    for (uint32_t i = 0; i < n; i++) {
        const void *p = wallpaper_player_buffer(i);
        if (!p) { n = i; break; }
        memset(&s_dsc[i], 0, sizeof(s_dsc[i]));
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf    = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w     = (uint32_t)w;
        s_dsc[i].header.h     = (uint32_t)h;
        /* Keep the descriptor stride explicit: the decoder allocates padded
         * rows, while the visible LVGL image is tightly packed RGB565. */
        s_dsc[i].header.stride = (uint32_t)w * 2u;
        s_dsc[i].data_size    = (uint32_t)bytes;
        s_dsc[i].data         = (const uint8_t *)p;
    }
    s_dsc_count = n;
    wallpaper_player_set_sink(wallpaper_sink_cb, NULL);
    if (disp) {
        lv_display_add_event_cb(disp, display_refr_ready_cb, LV_EVENT_REFR_READY, NULL);
#if CONFIG_SMART_HOME_STANDBY_SELFTEST
        lv_display_add_event_cb(disp, display_render_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, display_render_cb, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(disp, display_render_cb, LV_EVENT_RENDER_READY, NULL);
#endif
    }
    s_wallpaper_attached = true;
    ESP_LOGI(TAG, "wallpaper attached: %" PRIu32 " buffers × %u bytes",
             s_dsc_count, (unsigned)bytes);
}

#else  /* !CONFIG_SMART_HOME_WALLPAPER_ENABLED */

void standby_ui_attach_wallpaper(void)
{
    ESP_LOGI(TAG, "wallpaper disabled by Kconfig — 仅使用渐变背景");
}

#endif /* CONFIG_SMART_HOME_WALLPAPER_ENABLED */

/* ── 播放器控制（关闭壁纸时为空实现，待机层逻辑无需改动） ── */
static void wall_resume(void)
{
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    wallpaper_player_resume();
#endif
}

static void wall_pause(void)
{
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    wallpaper_player_pause();
#endif
}

/* ── 交互 ── */
void standby_ui_notify_activity(void)
{
    s_last_activity_ms = lv_tick_get();
}

static void indev_activity_cb(lv_event_t *e)
{
    /* LVGL 9 indev events do not guarantee lv_event_get_indev() is the
     * originating input device. Keep the device passed as callback user data. */
    lv_indev_t *indev = lv_event_get_user_data(e);
    standby_ui_notify_activity();

    /* Detect the wake gesture before LVGL dispatches the press to the cover
     * or one of its children. Object CLICKED callbacks can be skipped when
     * a child claims the touch or when the release is interrupted. */
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER ||
        !s_visible || !s_cover) {
        return;
    }

    uint32_t now = lv_tick_get();
    bool second_press = s_last_click_ms != 0 &&
                        (now - s_last_click_ms) <=
                        (uint32_t)CONFIG_SMART_HOME_STANDBY_DOUBLE_CLICK_MS;
    ESP_LOGI(TAG, "touch press: t=%" PRIu32 " previous=%" PRIu32 " second=%d",
             now, s_last_click_ms, second_press);
    s_last_click_ms = second_press ? 0 : now;
    if (second_press) {
        ESP_LOGI(TAG, "double-click -> wake main UI");
        lv_indev_stop_processing(indev);
        standby_ui_hide();
    }
}

static void install_activity_hook(void)
{
    if (s_activity_hook_installed) return;
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        lv_indev_add_event_cb(i, indev_activity_cb, LV_EVENT_PRESSED, i);
    }
    s_activity_hook_installed = true;
}

/* ── 1s 刷新 ──
 * 页脚只放"图标 + 温度"：天气含义由 ReIcon 图标表达，不放条件文本
 * （用户 2026-10-18 定稿：去掉"阴/多云"这类文字，靠图标与温度的比例说话）。 */

/* 待机超时：优先取设置页写入的 NVS 值，未配置时回退到 Kconfig 常量 */
static uint32_t idle_sec(void)
{
    uint32_t v = 0;
    if (storage_get_u32("ui_idle_sec", &v) == ESP_OK && v >= 5) return v;
    return (uint32_t)CONFIG_SMART_HOME_STANDBY_IDLE_SEC;
}

/* Scene → footer weather icon; thermometer is the neutral fallback. */
static const lv_image_dsc_t *weather_icon_for_scene(weather_scene_t scene)
{
    switch (scene) {
    case WEATHER_SCENE_CLEAR:    return &reicon_weather_sun;
    case WEATHER_SCENE_CLOUDY:   return &reicon_weather_cloud_sun;
    case WEATHER_SCENE_OVERCAST: return &reicon_weather_cloud;
    case WEATHER_SCENE_RAIN:     return &reicon_weather_cloud_rain;
    case WEATHER_SCENE_THUNDER:  return &reicon_weather_cloud_bolt;
    case WEATHER_SCENE_SNOW:     return &reicon_weather_cloud_snow;
    case WEATHER_SCENE_FOG:      return &reicon_weather_cloud_fog;
    case WEATHER_SCENE_HAZE:     return &reicon_weather_sun_fog;
    case WEATHER_SCENE_NIGHT:    return &reicon_weather_moon;
    default:                     return &reicon_weather_thermometer;
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;

    standby_ui_update_time();

    weather_state_t wx;
    snapshot_take(&wx);

    /* Keep the connectivity sample for diagnostics, but do not render an
     * ONLINE/OFFLINE label on the standby surface. */
    int wifi = wifi_is_connected();

    char buf[48];
    /* 只留图标 + 温度：天气条件不再以文本呈现。 */
    if (s_weather_icon) {
        const lv_image_dsc_t *icon = wx.valid ? weather_icon_for_scene(wx.scene)
                                              : &reicon_weather_thermometer;
        lv_image_set_src(s_weather_icon, icon);
    }
    if (!wx.valid) {
        lv_label_set_text(s_temp, "--°C");
    } else {
        lv_snprintf(buf, sizeof(buf), "%d°C", wx.temperature_c);
        lv_label_set_text(s_temp, buf);
    }

    /* 页脚告警小提示：只有 ALARM_EVENT 处于 active 且链路在线时才出现；
     * 离线或无告警一律隐藏，不显示编造的数值。 */
    if (s_badge) {
        alarm_shadow_t al;
        bool active = AppProto_GetActiveAlarm(&al) && al.active &&
                      AppProto_GetLinkState() == LINK_UP;
        if (active) {
            const char *kind = al.alarm_id == PROTO_ALARM_GAS_MQ4   ? "可燃气超标" :
                               al.alarm_id == PROTO_ALARM_AIR_MQ135 ? "空气质量超标" :
                               al.alarm_id == PROTO_ALARM_PUMP      ? "水泵保护" : "设备告警";
            if (al.value > 0.0f) lv_snprintf(buf, sizeof(buf), "%s · %.0f%%", kind, (double)al.value);
            else                 lv_snprintf(buf, sizeof(buf), "%s", kind);
            lv_label_set_text(s_badge_label, buf);
            lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 有真实天气数据时，由天气驱动场景；否则保留 create 时的昼/夜兜底配色 */
#if CONFIG_SMART_HOME_WALLPAPER_TEST_FORCE
    /* 隔离测试：强制固定场景，绕过天气（无网络/无 Key 也能验证壁纸切换） */
    {
        static bool forced_done;
        if (!forced_done) {
            forced_done = true;
            weather_scene_t fs = weather_scene_from_string(CONFIG_SMART_HOME_WALLPAPER_TEST_SCENE);
            bool fn = CONFIG_SMART_HOME_WALLPAPER_TEST_NIGHT;
            ESP_LOGW(TAG, "[TEST] forced scene=%s night=%d (SMART_HOME_WALLPAPER_TEST_FORCE)",
                     CONFIG_SMART_HOME_WALLPAPER_TEST_SCENE, fn);
            standby_ui_set_scene(fs, fn);
            if (s_scene_cb) {
                s_scene_cb(fs, fn, s_scene_cb_user);
            }
        }
    }
#else
    if (wx.valid) {
        if (!s_scene_applied || wx.scene != s_applied_scene || wx.is_night != s_applied_night) {
            standby_ui_set_scene(wx.scene, wx.is_night);
            if (s_scene_cb) {
                s_scene_cb(wx.scene, wx.is_night, s_scene_cb_user);
            }
        }
    }
#endif /* CONFIG_SMART_HOME_WALLPAPER_TEST_FORCE */

    /* 空闲超时 → 回到待机（超时值可由设置页运行时修改） */
    uint32_t idle = idle_sec();
    if (!s_visible && lv_tick_elaps(s_last_activity_ms) > idle * 1000u) {
        ESP_LOGI(TAG, "idle %" PRIu32 " s → standby", idle);
        standby_ui_show();
    }

#if CONFIG_SMART_HOME_STANDBY_SELFTEST
    /* 隔离自检：每 5 次 tick 打一条汇总，便于单模块上板核对 */
    static uint32_t s_tick_count;
    if (++s_tick_count % 5 == 0) {
        struct tm current_tm;
        time_t now_ts = time(NULL);
        localtime_r(&now_ts, &current_tm);
# if CONFIG_SMART_HOME_WALLPAPER_ENABLED
        wallpaper_stats_t st;
        wallpaper_player_get_stats(&st);
        /* 显示侧真实刷新帧率（适配器自带统计）。适配器锁是可重入的，
         * LVGL 任务内（本 tick_cb）调用不会自锁。 */
        uint32_t disp_fps = 0;
#  if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
        (void)esp_lv_adapter_get_fps(NULL, &disp_fps);
#  endif
        ESP_LOGI(TAG,
                 "[SELFTEST] time=%s t=%04d-%02d-%02d %02d:%02d wx=%s %d°C code=%d "
                 "scene=%s night=%d wifi=%d vis=%d wall=%d frames=%lu drop=%lu err=%lu "
                 "last=%luus avg=%luus target=%lufps "
                 "sink=%lu/%luus hold=%lu/%luus dispfps=%lu "
                 "prep=%luus draw=%luus flush=%luus",
                 s_time_valid ? "ok" : "invalid",
                 current_tm.tm_year + 1900, current_tm.tm_mon + 1, current_tm.tm_mday,
                 current_tm.tm_hour, current_tm.tm_min,
                 wx.valid ? wx.condition : "-", wx.temperature_c, wx.weather_code,
                 weather_scene_to_string(wx.scene), wx.is_night,
                 wifi, s_visible, wallpaper_player_is_playing(),
                 (unsigned long)st.frames_decoded, (unsigned long)st.frames_dropped,
                 (unsigned long)st.decode_errors, (unsigned long)st.last_decode_us,
                 (unsigned long)st.avg_decode_us, (unsigned long)st.target_fps,
                 (unsigned long)st.last_sink_us, (unsigned long)st.max_sink_us,
                 (unsigned long)st.last_hold_us, (unsigned long)st.max_hold_us,
                 (unsigned long)disp_fps,
                 (unsigned long)s_last_prep_us, (unsigned long)s_last_draw_us,
                 (unsigned long)s_last_flush_us);
# else
        ESP_LOGI(TAG,
                 "[SELFTEST] time=%s t=%04d-%02d-%02d %02d:%02d wx=%s %d°C code=%d "
                 "scene=%s night=%d wifi=%d vis=%d wall=off",
                 s_time_valid ? "ok" : "invalid",
                 current_tm.tm_year + 1900, current_tm.tm_mon + 1, current_tm.tm_mday,
                 current_tm.tm_hour, current_tm.tm_min,
                 wx.valid ? wx.condition : "-", wx.temperature_c, wx.weather_code,
                 weather_scene_to_string(wx.scene), wx.is_night,
                 wifi, s_visible);
# endif
    }
#endif /* CONFIG_SMART_HOME_STANDBY_SELFTEST */
}

/* ── 创建 ── */
static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

void standby_ui_create(lv_obj_t *parent)
{
    if (s_cover) {
        ESP_LOGW(TAG, "already created");
        return;
    }
    if (!parent) {
        parent = lv_screen_active();
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_frame_lock) {
        s_frame_lock = xSemaphoreCreateMutex();
    }

    const lv_font_t *f_cn14 = ui_pages_font(14);
    const lv_font_t *f_time = &lv_font_montserrat_48;
    const lv_font_t *f_temp = ui_pages_font(48);

    /* 1. 全屏覆盖层：CLICKABLE → 吞掉底层 UI 的点击 */
    s_cover = lv_obj_create(parent);
    lv_obj_remove_style_all(s_cover);
    lv_obj_set_pos(s_cover, 0, 0);
    lv_obj_set_size(s_cover, TK_CANVAS_W, TK_CANVAS_H);
    lv_obj_set_style_bg_color(s_cover, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_CLICKABLE);

    /* 2. Opaque shell.  Keeping this layer fully opaque prevents the page
     * below from showing through while the video card is being refreshed. */
    s_bg = lv_obj_create(s_cover);
    lv_obj_remove_style_all(s_bg);
    lv_obj_set_pos(s_bg, 0, 0);
    lv_obj_set_size(s_bg, TK_CANVAS_W, TK_CANVAS_H);
    lv_obj_set_style_bg_opa(s_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bg, lv_color_hex(COLOR_INK), 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_bg, LV_OBJ_FLAG_CLICKABLE);

    /* 3. Rounded media card.  The parent clips its child, so the JPEG frame
     * itself stays a plain LVGL image and does not need per-frame masking. */
    s_card = lv_obj_create(s_cover);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_pos(s_card, 28, 18);
    lv_obj_set_size(s_card, TK_CANVAS_W - 56, TK_CANVAS_H - 36);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    /* 直角 + 不做圆角裁剪：见文件头说明，圆角会让 PPA 每帧退回 CPU 混合。 */
    lv_obj_set_style_radius(s_card, 0, 0);
    lv_obj_set_style_clip_corner(s_card, false, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    s_video = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_video);
    lv_obj_set_pos(s_video, 16, 16);
    lv_obj_set_size(s_video, VIDEO_W, VIDEO_H);
    lv_obj_set_style_bg_color(s_video, lv_color_hex(0x243B5A), 0);
    lv_obj_set_style_bg_opa(s_video, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_video, 0, 0);
    lv_obj_set_style_clip_corner(s_video, false, 0);
    lv_obj_clear_flag(s_video, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_video, LV_OBJ_FLAG_CLICKABLE);

    /* Dynamic wallpaper (hidden until the first decoded frame). */
    s_wall = lv_image_create(s_video);
    /* Keep the 1024×600 frame 1:1 and center-crop the fixed video viewport.
     * This geometry never changes between frames. */
    lv_obj_set_size(s_wall, TK_CANVAS_W, TK_CANVAS_H);
    lv_obj_set_pos(s_wall,
                   -((int32_t)TK_CANVAS_W - VIDEO_W) / 2,
                   -((int32_t)TK_CANVAS_H - VIDEO_H) / 2);
    lv_obj_add_flag(s_wall, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_wall, LV_OBJ_FLAG_CLICKABLE);

    /* 4. Footer metadata.  Labels use Latin-safe strings; weather meaning is
     * carried by the actual ReIcon SVG, avoiding missing CJK glyph boxes. */
    s_footer = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_footer);
    lv_obj_set_pos(s_footer, 16, 450);
    lv_obj_set_size(s_footer, TK_CANVAS_W - 88, 66);
    lv_obj_set_style_bg_color(s_footer, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_footer, LV_OBJ_FLAG_CLICKABLE);

    /* 时钟占页脚左端（66px 高，只放时间；中文日期已按定稿移除）。 */
    s_time = make_label(s_footer, f_time, COLOR_CREAM, "--:--");
    lv_obj_set_pos(s_time, 6, 0);
    lv_obj_set_size(s_time, 180, 56);

    /* 页脚告警小提示：只在告警进行中出现（不进视频区）。 */
    s_badge = lv_obj_create(s_footer);
    lv_obj_remove_style_all(s_badge);
    lv_obj_set_pos(s_badge, 190, 22);
    lv_obj_set_size(s_badge, 250, 24);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x2A1A1A), 0);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_badge, 8, 0);
    lv_obj_set_style_border_width(s_badge, 1, 0);
    lv_obj_set_style_border_color(s_badge, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_border_opa(s_badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_CLICKABLE);
    s_badge_label = make_label(s_badge, f_cn14, 0xFFD9D9, "");
    lv_obj_set_pos(s_badge_label, 10, 3);
    lv_obj_set_size(s_badge_label, 230, 18);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    s_weather_icon = lv_image_create(s_footer);
    /* 图标 + 温度作为一组右对齐到页脚右侧（右边距 12，右缘 924）：
     * 图标 40px 配 48px 数字 —— 图标略小于数字字高，视觉重量才相当；
     * 组合 = icon(766..806) + 6px 间距 + temp 右对齐(812..924)。 */
    lv_obj_set_pos(s_weather_icon, 766, 13);
    lv_obj_set_size(s_weather_icon, 40, 40);
    lv_image_set_src(s_weather_icon, &reicon_weather_thermometer);
    lv_obj_set_style_image_recolor(s_weather_icon, lv_color_hex(COLOR_CREAM), 0);
    lv_obj_set_style_image_recolor_opa(s_weather_icon, LV_OPA_COVER, 0);

    s_temp = make_label(s_footer, f_temp, COLOR_CREAM, "--°C");
    lv_obj_set_pos(s_temp, 812, 0);
    lv_obj_set_size(s_temp, 112, 56);
    lv_obj_set_style_text_align(s_temp, LV_TEXT_ALIGN_RIGHT, 0);

    /* 5. 兜底场景（无网络时按本地时间判断昼夜） */
    standby_ui_set_scene(WEATHER_SCENE_CLEAR, hour_is_night());

    /* 6. 1s 刷新 + 触摸 activity 监听 */
    s_tick = lv_timer_create(tick_cb, 1000, NULL);
    install_activity_hook();
    standby_ui_update_time();

    s_last_activity_ms = lv_tick_get();
    s_visible = true;   /* 创建即显示待机层（与 app_main 启动顺序一致） */

    ESP_LOGI(TAG, "standby UI created (%dx%d, idle %d s)",
             TK_CANVAS_W, TK_CANVAS_H,
             CONFIG_SMART_HOME_STANDBY_IDLE_SEC);
}

/* ── 显示 / 隐藏 ── */
void standby_ui_show(void)
{
    if (!s_cover || s_visible) return;
    lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    s_last_click_ms = 0;
    wall_resume();
    ESP_LOGI(TAG, "standby shown");
}

void standby_ui_hide(void)
{
    if (!s_cover || !s_visible) return;
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    if (s_frame_lock) xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    if (s_pending_frame != UINT32_MAX) {
        wallpaper_player_frame_release(s_pending_frame);
        s_pending_frame = UINT32_MAX;
    }
    if (s_displayed_frame != UINT32_MAX) {
        s_retired_frames_mask |= (1u << s_displayed_frame);
        s_displayed_frame = UINT32_MAX;
    }
    if (s_frame_lock) xSemaphoreGive(s_frame_lock);
#endif
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    s_last_activity_ms = lv_tick_get();
    wall_pause();
    ESP_LOGI(TAG, "standby hidden");
}

bool standby_ui_is_visible(void) { return s_visible; }

/* ── 外部状态注入（任意任务，只写快照） ── */
void standby_ui_update_weather(const weather_state_t *state)
{
    snapshot_put_weather(state);
}

#else  /* !CONFIG_SMART_HOME_STANDBY_ENABLED —— 隔离测试：整模块空实现 */

void standby_ui_create(lv_obj_t *parent) { (void)parent; }
void standby_ui_show(void) {}
void standby_ui_hide(void) {}
bool standby_ui_is_visible(void) { return false; }
void standby_ui_notify_activity(void) {}
void standby_ui_update_time(void) {}
bool standby_ui_time_valid(void) { return false; }
void standby_ui_update_weather(const weather_state_t *state) { (void)state; }
void standby_ui_set_scene(weather_scene_t scene, bool is_night) { (void)scene; (void)is_night; }
void standby_ui_set_scene_callback(standby_scene_cb_t cb, void *user) { (void)cb; (void)user; }
void standby_ui_attach_wallpaper(void) {}

#endif /* CONFIG_SMART_HOME_STANDBY_ENABLED */
