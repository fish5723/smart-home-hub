/**
 * @file lvgl_port.c
 * @brief LVGL Port — esp_lvgl_adapter 封装层
 *
 * 硬件: JC-ESP32P4-M3-DEV + JD9165BA 1024×600 MIPI DSI + GT911
 */

#include "lvgl_port.h"
#include "bsp/lcd/lcd.h"
#include "bsp/touch/touch.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_adapter_input.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"

static const char *TAG = "LVGL_PORT";

static lv_display_t *s_disp  = NULL;
static lv_indev_t   *s_indev = NULL;

/* 触摸状态缓存（16ms 节流） */
static lv_indev_data_t s_last_touch = {0};
static uint32_t s_last_read_ms = 0;
static bool s_logged_pressed = false;

/* ── 触摸读取回调（轮询模式，每 16ms 节流） ── */
static void lvgl_port_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint32_t now = lv_tick_get();

    if (now - s_last_read_ms < 16) {
        *data = s_last_touch;
        return;
    }
    s_last_read_ms = now;

    touch_point_t point;
    esp_err_t ret = touch_read(&point);

    if (ret == ESP_OK && point.pressed) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state   = LV_INDEV_STATE_PRESSED;
        if (!s_logged_pressed) {
            ESP_LOGI(TAG, "Touch edge: PRESSED x=%u y=%u", point.x, point.y);
        }
        s_logged_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        if (s_logged_pressed) {
            ESP_LOGI(TAG, "Touch edge: RELEASED");
        }
        s_logged_pressed = false;
    }

    s_last_touch = *data;
}

/* ── LVGL 初始化 ── */
esp_err_t lvgl_port_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL port via esp_lvgl_adapter");

    /* 旋转 */
#ifdef CONFIG_SMART_HOME_LCD_ROTATE_0
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
#elif defined(CONFIG_SMART_HOME_LCD_ROTATE_90)
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_90;
#elif defined(CONFIG_SMART_HOME_LCD_ROTATE_180)
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_180;
#elif defined(CONFIG_SMART_HOME_LCD_ROTATE_270)
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_270;
#else
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
#endif

    /* 防撕裂策略由 sdkconfig 选择。IDF 6.1 提供 MIPI frame-buffer-complete
     * 回调，DOUBLE_DIRECT 可安全释放/切换面板帧缓冲。 */
#ifdef CONFIG_SMART_HOME_LCD_TEAR_AVOID_NONE
    esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
#elif defined(CONFIG_SMART_HOME_LCD_TEAR_AVOID_DOUBLE_DIRECT)
    esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
#elif defined(CONFIG_SMART_HOME_LCD_TEAR_AVOID_DOUBLE_FULL)
    esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL;
#else
    esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI;
#endif

    /* 1. 初始化 adapter */
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    /* LVGL vector/SVG rendering (ThorVG) needs a 32 KB-capable task stack. */
    cfg.task_stack_size = 32768;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "Adapter init failed");

    /* 2. 注册显示 */
    esp_lv_adapter_display_profile_t profile = {
        .interface             = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
        .rotation              = rotation,
        .hor_res               = LCD_WIDTH,
        .ver_res               = LCD_HEIGHT,
        /*
         * A wallpaper frame invalidates most of the 1024x600 surface.  A
         * A full-height draw buffer reduces MIPI transfer submissions to one
         * per wallpaper frame.  It costs two 1024x600 RGB565 draw buffers in
         * PSRAM (about 2.4 MiB) and avoids five/three strip handoffs.
         * LVGL layout and pixels are unchanged.
         */
        .buffer_height         = 600,
        .use_psram             = true,
        /* ESP32-P4 PPA accelerates LVGL blend/fill operations.  The project
         * uses one draw unit, which is required by the adapter's PPA path. */
        .enable_ppa_accel      = true,
        .require_double_buffer = true,
        .mono_layout           = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
    };

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel           = lcd_get_panel(),
        .panel_io        = lcd_get_panel_io(),
        .profile         = profile,
        .tear_avoid_mode = tear,
        .te_sync         = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };

    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "Display registration failed");
    ESP_LOGI(TAG, "Display registered: %dx%d", LCD_WIDTH, LCD_HEIGHT);

    /* 3. 触摸注册通过 lvgl_port_register_touch() 单独调用
     *    原因: 与 WiFi SDIO 共存时，DMA 操作可能破坏 touch handle，
     *         将触摸注册步骤独立便于控制初始化顺序。
     */

    /* 4. 启动 adapter 任务 (此时触摸还未注册，LVGL 不会轮询触摸) */
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "Adapter start failed");
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    /* 显示侧真实刷新帧率统计（壁纸帧率排查用；由 SELFTEST 打印 dispfps） */
    (void)esp_lv_adapter_fps_stats_enable(NULL, true);
#endif
    ESP_LOGI(TAG, "LVGL port init complete");
    return ESP_OK;
}

esp_err_t lvgl_port_register_touch(void)
{
    if (s_indev) {
        ESP_LOGW(TAG, "Touch already registered");
        return ESP_OK;
    }
#if CONFIG_SMART_HOME_TOUCH_ENABLED
    esp_lcd_touch_handle_t th = touch_get_handle();
    if (!th) {
        ESP_LOGE(TAG, "Touch handle not available");
        return ESP_ERR_INVALID_STATE;
    }
    s_indev = lv_indev_create();
    if (!s_indev) {
        ESP_LOGE(TAG, "Failed to create LVGL indev");
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_port_touch_read);
    lv_indev_set_display(s_indev, s_disp);
    ESP_LOGI(TAG, "Touch registered");
#endif
    return ESP_OK;
}
