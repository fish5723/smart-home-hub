/**
 * @file vision_event.c
 * @brief PIR 事件驱动的人脸检测（P4）
 *
 * 约束与取舍：
 *  - 本板无摄像头电源开关，传感器在 camera_init 时已开流；因此「关摄像头」
 *    只能停预览，省不了模组功耗。这里保持预览常开（不改现有观感），
 *    事件时把同一张 LVGL 图像临时挂到全屏覆盖层上做放大显示。
 *  - 检测耗时 ~0.5 s（3 帧 × ESPDet 416 单帧 ~177 ms），放在独立任务里，
 *    不阻塞主循环的协议轮询。
 *  - 所有 LVGL 对象操作都切到 LVGL 任务（esp_lv_adapter_lock + lv_async_call）。
 */
#include "vision_event.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_proto.h"
#include "camera.h"
#include "camera_manager.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face_detect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd.h"
#include "protocol.h"
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
#include "wallpaper_player.h"
#endif

static const char *TAG = "VISION_EVT";

/* ── 事件参数 ── */
#define PIR_POLL_US          (50 * 1000)
#define COOLDOWN_US         (30LL * 1000 * 1000)
#define SCAN_FRAMES         3
#define HOLD_MS             5000
#define SLIDE_IN_MS         320
#define SLIDE_OUT_MS        260
#define CAPTURE_TIMEOUT_MS  3000

/* ── 跨任务状态 ── */
static SemaphoreHandle_t s_trig;
static volatile bool     s_busy;
static int64_t           s_last_start_us;
static bool              s_prev_present;
static bool              s_first_poll = true;

/* ── LVGL 对象（只在 LVGL 任务里访问） ── */
static lv_obj_t *s_cam_img;
static lv_obj_t *s_overlay;
static lv_obj_t *s_badge;

/* 两份独立的文本缓冲：扫描中 / 结果，避免异步回调读到被覆盖的内容 */
static char s_text_scan[32]  = "CAM ON / SCAN";
static char s_text_result[48] = "NO FACE";

/* UI 是否存在：headless 低功耗测试（LCD/LVGL 关闭）时为 false。
 * 没有 UI 时检测仍照常跑，只是不操作覆盖层/角标。 */
static bool s_ui_enabled;

/* ═══════════════════════════════════════════════════════
   LVGL 侧（全部在 LVGL 任务上下文）
   ═══════════════════════════════════════════════════════ */

static void anim_set_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void badge_set_cb(void *arg)
{
    if (s_badge && arg) {
        lv_label_set_text(s_badge, (const char *)arg);
    }
}

static void overlay_show_cb(void *arg)
{
    (void)arg;
    if (!s_overlay || !s_cam_img) {
        return;
    }

    /* 图像本身就由 PPA 缩放到 1024×600，这里只换父节点，不做任何缩放 */
    lv_obj_set_parent(s_cam_img, s_overlay);
    lv_obj_set_pos(s_cam_img, 0, 0);
    lv_obj_remove_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);

    /* 图像晚于角标挂进覆盖层，必须把角标提到最前，否则被画面盖住 */
    lv_obj_move_foreground(s_badge);

    lv_label_set_text(s_badge, s_text_scan);
    lv_obj_set_y(s_overlay, LCD_HEIGHT);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_exec_cb(&a, anim_set_y);
    lv_anim_set_values(&a, LCD_HEIGHT, 0);
    lv_anim_set_duration(&a, SLIDE_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "overlay in");
}

static void overlay_hide_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    /* 图像还给主屏并隐藏：待机层不再长期叠一块预览 */
    if (s_cam_img) {
        lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(s_cam_img, lv_screen_active());
        lv_obj_set_pos(s_cam_img, 0, 0);
    }
    ESP_LOGI(TAG, "overlay out");
}

static void overlay_hide_cb(void *arg)
{
    (void)arg;
    if (!s_overlay) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_exec_cb(&a, anim_set_y);
    lv_anim_set_values(&a, 0, LCD_HEIGHT);
    lv_anim_set_duration(&a, SLIDE_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, overlay_hide_done);
    lv_anim_start(&a);
}

/* 从任意任务安全地往 LVGL 任务投递 */
static void ui_async(void (*cb)(void *), void *arg)
{
    /* headless：没有 LVGL，覆盖层/角标全部跳过，检测结果只走串口日志 */
    if (!s_ui_enabled) return;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGW(TAG, "lvgl lock failed");
        return;
    }
    if (lv_async_call(cb, arg) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call rejected");
    }
    esp_lv_adapter_unlock();
}

void vision_event_attach_ui(lv_obj_t *cam_img)
{
    s_ui_enabled = true;
    s_cam_img = cam_img;

    if (s_overlay == NULL) {
        s_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_overlay, LCD_WIDTH, LCD_HEIGHT);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_set_style_radius(s_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_overlay, 0, 0);
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

        s_badge = lv_label_create(s_overlay);
        lv_label_set_text(s_badge, "CAM ON");
        lv_obj_set_style_text_font(s_badge, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_badge, lv_color_white(), 0);
        lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(s_badge, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(s_badge, 10, 0);
        lv_obj_set_style_radius(s_badge, 8, 0);
        lv_obj_set_pos(s_badge, 16, 14);

        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "overlay attached (camera image hidden by default)");
    }

    /* 平时不显示预览：待机层保持干净，也避免 30fps 的 1MB 拷贝白耗 CPU */
    lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
}

/* ═══════════════════════════════════════════════════════
   检测任务
   ═══════════════════════════════════════════════════════ */

static void vision_task(void *arg)
{
    (void)arg;

    /* 128B 对齐：camera_capture_frame 内部对 out_buf 做 cache msync，
     * 起始地址/长度必须按 cache line（0x80）对齐，否则每次抓帧都报错 */
    uint8_t *buf = heap_caps_aligned_calloc(128, 1, CAMERA_FRAME_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "frame buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }
    if (face_detect_init() != ESP_OK) {
        ESP_LOGE(TAG, "face_detect_init failed");
        free(buf);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "task ready (free PSRAM %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    while (1) {
        if (xSemaphoreTake(s_trig, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 平时预览停止；事件时临时开预览 + 暂停壁纸，避免两路 30fps 同时抢 CPU */
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
        bool wall_was_playing = wallpaper_player_is_playing() &&
                                !wallpaper_player_is_paused();
        if (wall_was_playing) {
            wallpaper_player_pause();
        }
#endif
        if (camera_manager_preview_start() != ESP_OK) {
            ESP_LOGW(TAG, "preview start failed — camera missing");
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
            if (wall_was_playing) {
                wallpaper_player_resume();
            }
#endif
            snprintf(s_text_result, sizeof(s_text_result), "CAM ERR");
            ESP_LOGW(TAG, "result: %s (preview unavailable)", s_text_result);
            s_busy = false;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(40));   /* 等第一帧，避免先闪上一帧旧画面 */

        /* 滑入动画期间不刷新画面：整屏重绘 ~45 ms，会把动画拖成幻灯片 */
        camera_manager_set_refresh_enabled(false);
        ui_async(overlay_show_cb, NULL);
        vTaskDelay(pdMS_TO_TICKS(SLIDE_IN_MS + 80));
        camera_manager_set_refresh_enabled(true);

        int   best_count = 0;
        float best_score = 0.0f;
        int   ok_frames  = 0;

        for (int i = 0; i < SCAN_FRAMES; i++) {
            if (camera_manager_capture(buf, CAPTURE_TIMEOUT_MS) != ESP_OK) {
                /* 首次超时通常意味着排线掉了/接触不良；继续抓只会在坏状态下
                 * 拖时间。camera_manager 已置 lost 标记，下次预览前会重建。 */
                ESP_LOGW(TAG, "frame %d capture failed — abort scan", i + 1);
                break;
            }
            face_detect_result_t r;
            if (face_detect_frame_rgb565((const uint16_t *)buf, CAMERA_H_RES,
                                         CAMERA_V_RES, &r) != ESP_OK) {
                continue;
            }
            ok_frames++;
            if (r.best_score > best_score) {
                best_score = r.best_score;
            }
            if (r.count > best_count) {
                best_count = r.count;
            }
            ESP_LOGD(TAG, "frame %d: count=%d best=%.2f %lldus",
                     i + 1, r.count, r.best_score, (long long)r.detect_us);
        }

        if (ok_frames == 0) {
            snprintf(s_text_result, sizeof(s_text_result), "CAM ERR");
        } else if (best_count > 0) {
            snprintf(s_text_result, sizeof(s_text_result), "FACE %d  %.2f",
                     best_count, best_score);
        } else {
            snprintf(s_text_result, sizeof(s_text_result), "NO FACE");
        }
        ESP_LOGI(TAG, "result: %s (%d/%d frames)", s_text_result,
                 ok_frames, SCAN_FRAMES);
        /* 人脸确认结果写成本地伪传感器：规则“回家开灯”只认 FACE>=1，
         * PIR 单独有人不会开灯；无脸/坏帧一律 0。 */
        AppProto_SetLocalSensorValue(PROTO_DEV_FACE,
                                     (ok_frames > 0 && best_count > 0) ? 1.0f : 0.0f);
        ui_async(badge_set_cb, s_text_result);

        vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
        /* 滑出动画期间同样暂停刷新 */
        camera_manager_set_refresh_enabled(false);
        ui_async(overlay_hide_cb, NULL);

        /* 等滑出动画结束再收尾：停预览、恢复壁纸 */
        vTaskDelay(pdMS_TO_TICKS(SLIDE_OUT_MS + 200));
        camera_manager_preview_stop();
        camera_manager_set_refresh_enabled(true);
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
        if (wall_was_playing) {
            wallpaper_player_resume();
        }
#endif
        /* 事件结束复位确认位：规则回到“未确认”，下一次 PIR 边沿可重新触发 */
        AppProto_SetLocalSensorValue(PROTO_DEV_FACE, 0.0f);
        s_busy = false;
    }
}

/* ═══════════════════════════════════════════════════════
   初始化 + 主循环轮询
   ═══════════════════════════════════════════════════════ */

esp_err_t vision_event_init(void)
{
    if (s_trig != NULL) {
        return ESP_OK;
    }
    s_trig = xSemaphoreCreateBinary();
    if (s_trig == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* 人脸确认初值 0：没有事件时不能把“回家开灯”规则误触发 */
    AppProto_SetLocalSensorValue(PROTO_DEV_FACE, 0.0f);
    if (xTaskCreate(vision_task, "vision_evt", 12288, NULL, 5, NULL) != pdPASS) {
        vSemaphoreDelete(s_trig);
        s_trig = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "event task started (cooldown %lld s)",
             (long long)(COOLDOWN_US / 1000000));
    return ESP_OK;
}

static void trigger_event(int64_t now, const char *reason)
{
    if (s_busy) {
        ESP_LOGI(TAG, "%s ignored (busy)", reason);
        return;
    }
    if (s_last_start_us > 0 && now - s_last_start_us < COOLDOWN_US) {
        ESP_LOGI(TAG, "%s ignored (cooldown %lld s left)", reason,
                 (long long)((COOLDOWN_US - (now - s_last_start_us)) / 1000000));
        return;
    }
    s_busy = true;
    s_last_start_us = now;
    xSemaphoreGive(s_trig);
    ESP_LOGI(TAG, "%s → vision event", reason);
}

void vision_event_tick(void)
{
    static int64_t last_poll_us;

    int64_t now = esp_timer_get_time();
    if (now - last_poll_us < PIR_POLL_US) {
        return;
    }
    last_poll_us = now;

#if CONFIG_SMART_HOME_VISION_EVENT_SELFTEST
    /* 联调开关：链路未接 F103 时验证一次完整流程（默认关闭） */
    static int64_t selftest_at_us;
    static bool    selftest_done;
    if (!selftest_done) {
        if (selftest_at_us == 0) {
            selftest_at_us = now + 10LL * 1000 * 1000;
        } else if (now >= selftest_at_us) {
            selftest_done = true;
            trigger_event(now, "selftest");
        }
    }
#endif

    float human = AppProto_GetSensorValue(PROTO_DEV_HUMAN);
    if (isnan(human)) {
        /* 影子过期（链路断开/无数据）：复位边沿状态，避免恢复瞬间误触发 */
        s_first_poll = true;
        s_prev_present = false;
        return;
    }

    bool present = human >= 0.5f;
    if (s_first_poll) {
        s_first_poll = false;
        s_prev_present = present;
        return;
    }

    if (present && !s_prev_present) {
        trigger_event(now, "PIR edge");
    }
    s_prev_present = present;
}
