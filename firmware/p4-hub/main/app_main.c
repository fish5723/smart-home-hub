/*
 * Smart Home Hub — P4 Hub
 * 阶段 0: LCD 点屏 + 触摸 + LVGL 图形界面
 *
 * 硬件: JC-ESP32P4-M3-DEV + JD9165BA 1024×600 MIPI DSI + GT911
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "lcd.h"
#include "touch.h"
#include "lvgl_port.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "storage.h"
#include "crash_log.h"
#include "sd_card.h"
#include "ota/ota_update.h"
#include "ota/f103_iap.h"
#include "ota/net_ota.h"
#include "wifi.h"
#if CONFIG_ESP_HOSTED
#include "eh_host_core.h"
#endif
#if CONFIG_SMART_HOME_CAMERA_ENABLED
#include "camera_manager.h"
#include "camera.h"
#include "jpeg_bench.h"
#include "face_detect.h"
#include "vision_event.h"
#endif
#include "face_batch.h"
#if CONFIG_SMART_HOME_VISION_ENABLED
#include "vision_core.h"
#endif
#include "common/watchdog.h"
#include "app_proto.h"
#include "rule_runtime.h"
#if CONFIG_SMART_HOME_MQTT_ENABLED
#include "mqtt_service.h"
#include "device_report.h"
#endif

static const char *TAG = "p4-hub";

/* 显示/触摸初始化的最大重试次数（H4：失败不得挂起或重启循环） */
#define DISP_INIT_MAX_RETRY   3

/* ── LVGL UI（在 LVGL 任务上下文中创建） ── */
#include "ui_pages.h"

#if CONFIG_SMART_HOME_STANDBY_ENABLED
#include "standby_ui.h"
#include "wallpaper_player.h"
#include "weather_service.h"
#endif

#if CONFIG_SMART_HOME_CAMERA_ENABLED
/* P0 自检：抓 3 帧，打印每帧耗时与缓冲大小。
 * 期望 size == 800*640*2 = 1,024,000（ISP 输出 RGB565）。 */
static void camera_selftest(void)
{
    /* 64B 对齐：camera_capture 内部会对该缓冲做 cache msync */
    void *buf = heap_caps_aligned_calloc(128, 1, CAMERA_FRAME_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[CAMERA] selftest: PSRAM buffer alloc failed");
        return;
    }

    for (int i = 1; i <= 3; i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = camera_manager_capture(buf, 3000);
        int64_t dt_us = esp_timer_get_time() - t0;
        ESP_LOGI(TAG, "[CAMERA] frame %d: %s, %lld us, size=%u",
                 i, esp_err_to_name(err), (long long)dt_us,
                 (unsigned)camera_manager_get_frame_size());
    }

    free(buf);
}

/* P1 验收：JPEG 硬件/软件编码对比（抓一帧 RGB565，质量 80，各 5 次） */
static void camera_jpeg_bench(void)
{
    void *buf = heap_caps_aligned_calloc(128, 1, CAMERA_FRAME_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[JPEG] bench: PSRAM buffer alloc failed");
        return;
    }
    if (camera_manager_capture(buf, 3000) != ESP_OK) {
        ESP_LOGW(TAG, "[JPEG] bench: capture failed");
        free(buf);
        return;
    }

    jpeg_bench_result_t r;
    if (jpeg_bench_run((const uint16_t *)buf, CAMERA_H_RES, CAMERA_V_RES, 80, &r) == ESP_OK) {
        ESP_LOGI(TAG, "[JPEG] HW ok=%d dec=%d %lldus %uB psnr=%.1f | "
                 "SW ok=%d dec=%d %lldus %uB psnr=%.1f",
                 (int)r.hw_ok, (int)r.hw_decoded, (long long)r.hw_us,
                 (unsigned)r.hw_bytes, r.hw_psnr_db,
                 (int)r.sw_ok, (int)r.sw_decoded, (long long)r.sw_us,
                 (unsigned)r.sw_bytes, r.sw_psnr_db);
    }
    free(buf);
}

static void camera_preview_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    camera_manager_lvgl_refresh();
}

/* P3 自检：抓帧 → MSR+MNP 检测。第 1 次含模型加载（惰性），后 3 次为纯推理。 */
static void camera_face_selftest(void)
{
    void *buf = heap_caps_aligned_calloc(128, 1, CAMERA_FRAME_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[FACE] selftest: PSRAM buffer alloc failed");
        return;
    }
    if (face_detect_init() != ESP_OK) {
        ESP_LOGE(TAG, "[FACE] selftest: init failed");
        free(buf);
        return;
    }
    ESP_LOGI(TAG, "[FACE] free PSRAM before load: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (int i = 1; i <= 4; i++) {
        if (camera_manager_capture(buf, 3000) != ESP_OK) {
            ESP_LOGW(TAG, "[FACE] #%d capture failed", i);
            continue;
        }
        face_detect_result_t r;
        if (face_detect_frame_rgb565((const uint16_t *)buf, CAMERA_H_RES,
                                     CAMERA_V_RES, &r) == ESP_OK) {
            ESP_LOGI(TAG, "[FACE] #%d count=%d best=%.2f box=(%d,%d,%d,%d) "
                     "area=%d %lld us", i, r.count, r.best_score,
                     r.x, r.y, r.w, r.h, r.best_area, (long long)r.detect_us);
        }
        if (i == 1) {
            ESP_LOGI(TAG, "[FACE] free PSRAM after first run: %u B",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
    free(buf);
}

#endif

#if CONFIG_SMART_HOME_STANDBY_ENABLED
/* 当前生效的壁纸场景与文件（循环换片用）。这两个变量由 LVGL 任务（场景变化）
 * 和播放任务（一轮播完）共同读写，最坏情况只是读到上一轮的旧值、多换或少换
 * 一次，不存在一致性风险，故不加锁。 */
static weather_scene_t s_cur_scene = WEATHER_SCENE_UNKNOWN;
static bool            s_cur_night;
static char            s_cur_wall_path[192];

/* 场景/档位 + 槽位号 → JWP 路径。槽位 0 是正典 day.jwp/night.jwp，
 * 1..8 对应 day_02..day_09 / night_02..night_09。 */
static void wallpaper_path_for(const char *scene_name, const char *grade,
                               uint32_t idx, char *out, size_t out_size)
{
    if (idx == 0) {
        snprintf(out, out_size, "%s/%s/%s.jwp",
                 CONFIG_SMART_HOME_WALLPAPER_DIR, scene_name, grade);
    } else {
        snprintf(out, out_size, "%s/%s/%s_%02lu.jwp",
                 CONFIG_SMART_HOME_WALLPAPER_DIR, scene_name, grade,
                 (unsigned long)(idx + 1u));
    }
}

/* Pick a random variant for the current weather scene.  The canonical
 * day.jwp/night.jwp files remain valid fallbacks; optional day_02.jwp …
 * day_09.jwp (and night_NN.jwp) are generated from alternate source clips.
 *
 * 先在“实际存在”的候选里等概率挑一个，再尽量避开 avoid（上一段）。
 * 旧实现是“从随机起点往后扫、命中第一个存在的就返回”，当变体稀疏时
 * 会严重偏置：每档只有 day.jwp/day_02/day_03 三个（占 0/1/2 号槽，3~8 号槽
 * 恒空），正典文件会被兜回 7/9 的概率 —— 观感上就退化成“总是那一段”。 */
static bool standby_choose_wallpaper_path(weather_scene_t scene, bool is_night,
                                          const char *avoid, char *out, size_t out_size)
{
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    const char *scene_name = weather_scene_to_string(scene);
    const char *grade = is_night ? "night" : "day";
    char candidate[192];
    uint32_t present[9];
    uint32_t found = 0;

    for (uint32_t i = 0; i < 9u; ++i) {
        wallpaper_path_for(scene_name, grade, i, candidate, sizeof(candidate));
        FILE *f = fopen(candidate, "rb");
        if (f) {
            fclose(f);
            present[found++] = i;
        }
    }
    if (found == 0) {
        return false;
    }

    uint32_t pick = present[esp_random() % found];
    if (avoid && avoid[0] && found > 1u) {
        for (uint32_t attempt = 0; attempt < 4u; ++attempt) {
            char cur[192];
            wallpaper_path_for(scene_name, grade, pick, cur, sizeof(cur));
            if (strcmp(cur, avoid) != 0) {
                break;
            }
            pick = present[esp_random() % found];
        }
    }
    wallpaper_path_for(scene_name, grade, pick, out, out_size);
    return true;
#else
    (void)scene;
    (void)is_night;
    (void)avoid;
    (void)out;
    (void)out_size;
    return false;
#endif
}

/* 播放器一轮播完（JWP 循环回跳）后换一段变体，避免晴天一整天都是同一段。
 * 在播放任务上下文被调用；wallpaper_player_play() 只是投递队列，可安全调用。
 *
 * ⚠️ 目前默认关闭（不注册）：每次换片都要 open 新文件 + fseek 到随机位置，
 * 与 C6 的 SDIO 共用 SDMMC 主机时会把 esp_hosted 的 TX/RX mempool 打空
 * （eh_sdio: mempool OOM）→ 丢包 → MQTT 掉线。改为“一次启动只读一个壁纸”：
 * 文件只 open 一次，之后纯顺序读 + 文件内回跳。天气场景变化时仍会换片。 */
#ifdef ENABLE_WALLPAPER_ROTATION
static void standby_wall_looped(void *user)
{
    (void)user;
#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    if (!sd_card_is_mounted() || s_cur_scene == WEATHER_SCENE_UNKNOWN) {
        return;
    }
    char path[192];
    if (!standby_choose_wallpaper_path(s_cur_scene, s_cur_night, s_cur_wall_path,
                                       path, sizeof(path))) {
        return;
    }
    if (strcmp(path, s_cur_wall_path) == 0) {
        return;   /* 该场景只有一段可用时不折腾（避免无意义地重开文件） */
    }
    ESP_LOGI(TAG, "[WALLPAPER] 一轮播完 → 换片 %s", path);
    strlcpy(s_cur_wall_path, path, sizeof(s_cur_wall_path));
    wallpaper_player_play(path);
#endif
}
#endif /* ENABLE_WALLPAPER_ROTATION */

/* 天气 → 壁纸场景：切换 JWP 文件（在 LVGL 任务上下文被调用） */
static void standby_scene_changed(weather_scene_t scene, bool is_night, void *user)
{
    (void)user;

#if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "[WALLPAPER] SD not mounted — 保留默认渐变背景");
        return;
    }
    char path[192];
    if (!standby_choose_wallpaper_path(scene, is_night, NULL, path, sizeof(path))) {
        ESP_LOGI(TAG, "[WALLPAPER] %s/%s/{day,night}.jwp 缺失 — 保留默认渐变背景",
                 CONFIG_SMART_HOME_WALLPAPER_DIR, weather_scene_to_string(scene));
        return;
    }
    ESP_LOGI(TAG, "[WALLPAPER] scene=%s night=%d → %s",
             weather_scene_to_string(scene), is_night, path);
    s_cur_scene = scene;
    s_cur_night = is_night;
    strlcpy(s_cur_wall_path, path, sizeof(s_cur_wall_path));
    wallpaper_player_play(path);
#else
    (void)scene;
    (void)is_night;
#endif
}

/* 天气更新 → 待机层（在 weather 任务上下文被调用，只写快照） */
static void weather_updated_cb(const weather_state_t *state, void *user)
{
    (void)user;
    standby_ui_update_weather(state);
}
#endif /* CONFIG_SMART_HOME_STANDBY_ENABLED */

#if CONFIG_SMART_HOME_STANDBY_ENABLED && CONFIG_SMART_HOME_WALLPAPER_ENABLED
/* 壁纸调度：开机先等 Wi-Fi + MQTT 稳定（最多 25s），再放行播放，避开首轮突发。
 *
 * 历史：曾因 C6 与 host 的 esp_hosted 版本不一致（2.9.6 / 3.0.9）导致
 * SD 流式读把 SDIO mempool 打空（mempool OOM → MQTT 掉线），当时被迫改成
 * “联网时冻结最后一帧”。C6 升级到 3.0.9（STREAM 模式）后复测：
 * 2 次冷启动 + 10 分钟 30fps 常播零 OOM、零 MQTT 错误，故恢复为常播。 */
static void wallpaper_net_gate_task(void *arg)
{
    (void)arg;
    int stable_ms = 0;
    bool net_ok = false;
    for (int waited_ms = 0; waited_ms < 25000; waited_ms += 250) {
        net_ok = wifi_is_connected();
#if CONFIG_SMART_HOME_MQTT_ENABLED
        net_ok = net_ok && mqtt_service_connected();
#endif
        stable_ms = net_ok ? stable_ms + 250 : 0;
        if (stable_ms >= 5000) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGI(TAG, "[WALLPAPER] network calm — starting standby playback");
    wallpaper_player_set_network_ready(true);
    vTaskDelete(NULL);
}
#endif

static void ui_create(void *user_data)
{
    (void)user_data;
    /* SD BinFont 在 LVGL 任务上下文中解析；失败时页面保留内置 fallback。 */
    if (sd_card_is_mounted()) {
        ui_pages_load_sd_fonts(sd_card_get_mount_point());
    } else {
        ESP_LOGW(TAG, "[FONT] SD not mounted, using built-in fallback fonts");
    }
    ui_pages_create(lv_screen_active());

#if CONFIG_SMART_HOME_STANDBY_ENABLED
    /* 待机层建在六页 UI 之后 → 位于最上层，全屏覆盖并吞掉底层点击。
     * 主 UI 与待机层都不销毁，只做显示/隐藏切换（见 standby_ui.c）。 */
    standby_ui_create(lv_screen_active());
    standby_ui_set_scene_callback(standby_scene_changed, NULL);
    standby_ui_attach_wallpaper();
    /* 一轮播完 → 换一段变体（见 standby_wall_looped）
     * 默认不注册：换片会重开文件并随机寻址，与 SDIO 争用会打挂 Wi-Fi。 */
#ifdef ENABLE_WALLPAPER_ROTATION
    wallpaper_player_set_loop_cb(standby_wall_looped, NULL);
#endif

    /* 无网络时的兜底：SD 上第一个可用的 JWP（含测试样例） */
# if CONFIG_SMART_HOME_WALLPAPER_ENABLED
    if (sd_card_is_mounted()) {
        static const char *boot_paths[] = {
            CONFIG_SMART_HOME_WALLPAPER_DIR "/sunny/day.jwp",
            CONFIG_SMART_HOME_WALLPAPER_DIR "/test/sample.jwp",
        };
        for (size_t i = 0; i < sizeof(boot_paths) / sizeof(boot_paths[0]); i++) {
            FILE *f = fopen(boot_paths[i], "rb");
            if (f) {
                fclose(f);
                ESP_LOGI(TAG, "[WALLPAPER] boot fallback → %s", boot_paths[i]);
                strlcpy(s_cur_wall_path, boot_paths[i], sizeof(s_cur_wall_path));
                wallpaper_player_play(boot_paths[i]);
                break;
            }
        }
    }
# endif /* CONFIG_SMART_HOME_WALLPAPER_ENABLED */
    ESP_LOGI(TAG, "Standby layer created (double-click to wake)");
#endif /* CONFIG_SMART_HOME_STANDBY_ENABLED */

#if CONFIG_SMART_HOME_CAMERA_ENABLED
    /* ── 摄像头预览图像 (位置: x=10, y=300) ── */
    lv_obj_t *cam_img = camera_manager_create_lvgl_image(lv_screen_active(), 0, 0);
    if (cam_img) {
        ESP_LOGI(TAG, "Camera preview image created");
        vision_event_attach_ui(cam_img);
    } else {
        ESP_LOGW(TAG, "Camera preview image creation failed");
    }

    /* ── 摄像头帧刷新定时器 (30fps → ~33ms) ── */
    lv_timer_create(camera_preview_timer_cb, 33, NULL);

# if CONFIG_SMART_HOME_CAMERA_PREVIEW_AUTO
    /* 自动启动预览 */
    esp_err_t ret = camera_manager_preview_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auto preview start failed: %s", esp_err_to_name(ret));
    }
# endif
#endif

    ESP_LOGI(TAG, "LVGL UI created");
}

/* ── app_main ── */
void app_main(void)
{
    /* ⚠️ 使用约定（回滚 nb_console_vprintf 后的已知行为）：
     * IDF 默认 UART console 写为忙等——**P4 的串口工具必须保持开启**，
     * 关闭/切换串口会导致 main 卡死在日志写入上（任务看门狗触发）。
     * 联调期两个串口窗口（P4 + F103）都保持打开即可。
     * 非阻塞方案的完整实现在 git 历史 cdbe42b，将来需要时可恢复。 */

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Smart Home Hub - P4 Hub");
    ESP_LOGI(TAG, "Board: JC-ESP32P4-M3-DEV");
    ESP_LOGI(TAG, "Display: JD9165BA 1024x600 MIPI DSI");
    ESP_LOGI(TAG, "Touch: GT911");

    ESP_LOGI(TAG, "========================================");

    /* ── 时区：SNTP 只同步 UTC epoch，本地时间由应用层负责设置 ──
     * POSIX TZ 符号与 UTC 偏移相反：CST-8 = 中国标准时间 (UTC+8)。
     * 缺这两行时 localtime_r() 按 UTC 解析，待机时钟/昼夜判断会偏 8 小时。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 接线（实测定案，docs/04 §4.5）：
     *   P4 pin10 (GPIO47, RX) ← F103 PA9
     *   P4 pin12 (GPIO46, TX) → F103 PA10
     *   P4 pin6  (GND)        ↔ F103 GND
     * 联调期的 GPIO 自测/针脚探测诊断代码已清理——结论固化在 docs/04；
     * 如需再次定位针脚，git 历史 274a81b 有完整的探测实现可恢复。 */

    /* 0a. 注册主任务到 TWDT（10s 超时，sdkconfig.defaults 固化） */
    wdt_add_current_task();
    wdt_feed();

#if CONFIG_SMART_HOME_STANDBY_ENABLED && CONFIG_SMART_HOME_WALLPAPER_ENABLED
    /* 先关壁纸门控：等 wallpaper_net_gate_task 判定 Wi-Fi/MQTT 首轮突发结束 */
    wallpaper_player_set_network_ready(false);
#endif

    /* 0. NVS Flash 初始化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[OK] NVS flash initialized");

#if CONFIG_SMART_HOME_OTA_ENABLED
    /* OTA 版本/分区日志；若上次 OTA 未通过验证，这里会发现并清理 pending 标记。 */
    ota_update_init();
#endif

    /* 0b. 存储模块初始化 */
    ret = storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] Storage init: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[OK] Storage ready");
    }

    /* 0c. 崩溃日志初始化 */
    ret = crash_log_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[WARN] Crash log init: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[OK] Crash log ready");
    }

    /* 0d. SD 卡初始化
     * Slot 0（SD）与 Slot 1（C6 SDIO）共用一个 SDMMC 控制器。Hosted 的
     * auto-init 会在 app_main 之前复位 C6 并初始化 SDIO；若此时立刻挂 SD，
     * slot0 设置时钟会与 slot1 的启动流程互相踩踏，实测偶发
     * `sd_host_slot_sdmmc_do_transaction: failed to set clk` → SD 与 C6
     * 同时起不来。先等 Hosted 就绪（最多 3s），再做 SD 挂载。 */
#if CONFIG_ESP_HOSTED
    int hosted_ready = eh_host_wait_auto_init_ready(3000);
    ESP_LOGI(TAG, "[SD] hosted auto-init ready=%d", hosted_ready);
#endif
    ESP_LOGI(TAG, "[SD] Initializing...");
    ret = sd_card_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[WARN] SD card init skipped: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[OK] SD card ready");
    }

#if CONFIG_SMART_HOME_OTA_ENABLED
    /* SD 卡 OTA：存在镜像则写入非活动分区并重启；无文件时立即返回。 */
    (void)ota_update_check_sd();
#endif

#if CONFIG_SMART_HOME_FACE_BATCH_STANDALONE
    /* 阶段七独立评测：只保留 NVS/存储/SD，同步跑完测试集后空转。
     * 不初始化 WiFi/协议/摄像头/LCD/UI，避免其它子系统干扰检测耗时。 */
    ESP_LOGI(TAG, "[FACE_BATCH] standalone mode — WiFi/protocol/camera/UI skipped");
    if (face_batch_run_sync() != ESP_OK) {
        ESP_LOGE(TAG, "[FACE_BATCH] run failed");
    }
    ESP_LOGI(TAG, "[FACE_BATCH] done — idling");
    while (1) {
        wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

#if CONFIG_SMART_HOME_WIFI_ENABLED
    /* 0e. WiFi 初始化（SD 卡之后 — Slot 0/1 共享时钟分频器） */
    ESP_LOGI(TAG, "[WIFI] Initializing...");
    ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[WARN] WiFi init skipped: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[OK] WiFi ready");
#if CONFIG_SMART_HOME_WIFI_AUTO_CONNECT
        ret = wifi_connect_from_nvs();
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "[WIFI] No saved credentials, skipping auto-connect");
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[WARN] WiFi auto-connect: %s", esp_err_to_name(ret));
        }
#endif
    }
#endif /* CONFIG_SMART_HOME_WIFI_ENABLED */

#if CONFIG_SMART_HOME_NET_OTA_ENABLED
    /* 0e-2. 网络 OTA：后台任务等待 Wi-Fi 就绪后按策略检查 manifest */
    if (net_ota_init() == ESP_OK) {
        ESP_LOGI(TAG, "[OK] Network OTA ready");
    } else {
        ESP_LOGW(TAG, "[WARN] Network OTA init failed");
    }
#endif

    /* 0f. 视觉任务平台初始化 */
#if CONFIG_SMART_HOME_VISION_ENABLED
    ESP_LOGI(TAG, "[VISION] Initializing...");
    ret = vision_core_init();
    if (ret != 0) {
        ESP_LOGW(TAG, "[WARN] Vision core init: 0x%04x", ret);
        vision_set_disabled(true);
    } else {
        ESP_LOGI(TAG, "[OK] Vision core ready");
    }
#endif /* CONFIG_SMART_HOME_VISION_ENABLED */

    /* ── 阶段 2B: P4↔F103 协议栈初始化（提前到显示之前，避免串口阻塞延迟） ──
     * 接线（实测定案，见 docs/04 §4.5）：
     *   P4 pin10 (GPIO47, RX) ← F103 PA9
     *   P4 pin12 (GPIO46, TX) → F103 PA10
     *   P4 pin6  (GND)        ↔ F103 GND */
    ESP_LOGI(TAG, "[PROTO] Initializing P4↔F103 protocol...");
    AppProto_Init();
    ESP_LOGI(TAG, "[OK] Protocol stack ready");

    /* 阶段 8: F103 IAP 发送器（后台任务 + OTA_RESULT 回调） */
    if (f103_iap_init() == ESP_OK) {
        AppProto_SetOtaResultHandler(f103_iap_on_result);
        ESP_LOGI(TAG, "[OK] F103 IAP sender ready");
    } else {
        ESP_LOGW(TAG, "[WARN] F103 IAP sender init failed");
    }

    ret = rule_runtime_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "[RULES] init failed: %s", esp_err_to_name(ret));

    /* 立即轮询一次：发送 HELLO_REQ 启动协商 */
    AppProto_Poll();

#if CONFIG_SMART_HOME_MQTT_ENABLED
    /* ── 阶段 5: MQTT / Home Assistant 集成（后台任务，不阻塞 LVGL）──
     * 未配置 broker uri 时只打日志、不启动；Wi-Fi 未就绪时客户端自动重连。 */
    ESP_LOGI(TAG, "[MQTT] Initializing...");
    ret = mqtt_service_start();
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "[MQTT] no broker configured, MQTT disabled");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[MQTT] start failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[OK] MQTT service started");
#if CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_ENABLED
        ret = device_report_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[WARN] Device report init: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "[OK] Device report ready");
        }
#endif
    }
#endif

#if CONFIG_SMART_HOME_STANDBY_ENABLED && CONFIG_SMART_HOME_WALLPAPER_ENABLED
    /* 网络就绪门控：在 Wi-Fi/MQTT 首轮突发结束前不启动壁纸的 SD 读取 */
    if (xTaskCreate(wallpaper_net_gate_task, "wp_gate", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "[WARN] wallpaper gate task create failed — playback ungated");
        wallpaper_player_set_network_ready(true);
    }
#endif

#if CONFIG_SMART_HOME_LCD_ENABLED
    /* 1. LCD 初始化
     * ⚠️ 阶段 2 联调事故的最终定性：**屏幕未接时，esp_lcd 的 DCS 命令写入
     * 会忙等挂死**（MIPI-DSI 命令 FIFO 无面板应答永不排空，esp_lcd 内部
     * 无超时机制，backtrace: mipi_dsi_host_ll_gen_is_cmd_fifo_full ←
     * lcd_init）——此前反复出现的看门狗复位全部源于此。
     * esp_lcd 无法从外部加超时，唯一可靠的规避 = 不初始化。
     * 压测/无屏场景用 SMART_HOME_LCD_ENABLED=n（headless 模式）。 */
    ESP_LOGI(TAG, "[LCD] Initializing...");
    bool ui_available = false;
    for (int attempt = 1; attempt <= DISP_INIT_MAX_RETRY; attempt++) {
        ret = lcd_init();
        if (ret == ESP_OK) {
            ui_available = true;
            break;
        }
        ESP_LOGE(TAG, "[LCD] init attempt %d/%d failed: %s",
                 attempt, DISP_INIT_MAX_RETRY, esp_err_to_name(ret));
        wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (ui_available) {
        ESP_LOGI(TAG, "[OK] LCD initialized");
    } else {
        ESP_LOGE(TAG, "[DEGRADED] LCD unavailable after %d attempts — "
                      "running headless, UI disabled. 检查 FPC 排线与连接器。",
                 DISP_INIT_MAX_RETRY);
    }

    /* 2. 触摸初始化（仅在显示可用时才有意义） */
    if (ui_available) {
        ESP_LOGI(TAG, "[TOUCH] Initializing...");
        ret = touch_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[WARN] Touch init failed, UI runs without touch: %s",
                     esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "[OK] Touch ready");
        }
    }
#else
    ESP_LOGI(TAG, "[LCD] disabled (headless mode) — 压测/联调跳过显示链路");
    const bool ui_available = false;
#endif

#if CONFIG_SMART_HOME_CAMERA_ENABLED
    /* 2b. 摄像头初始化 (touch 之后 — 复用 I2C 总线 GPIO7/8) */
    ESP_LOGI(TAG, "[CAMERA] Initializing...");
    ret = camera_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[WARN] Camera init skipped: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[OK] Camera ready");
#if CONFIG_SMART_HOME_CAMERA_SELFTEST
        camera_selftest();
        camera_jpeg_bench();
        camera_face_selftest();
#endif
        vision_event_init();
    }
#endif

#if CONFIG_SMART_HOME_FACE_BATCH_TEST
    /* 阶段七评测入口：SD 卡批量读图 + 端侧检测（独立任务，不阻塞主循环） */
    if (face_batch_start() != ESP_OK) {
        ESP_LOGW(TAG, "[FACE_BATCH] start failed");
    }
#endif

    /* 3. LVGL 初始化（显示 + adapter 任务启动）—— 同样不做无界等待 */
#if CONFIG_SMART_HOME_LCD_ENABLED
    if (ui_available) {
        ESP_LOGI(TAG, "[LVGL] Initializing...");
        ret = lvgl_port_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[DEGRADED] LVGL init failed: %s — UI disabled",
                     esp_err_to_name(ret));
            ui_available = false;
        } else {
            ESP_LOGI(TAG, "[OK] LVGL ready");

            /* 4. 注册触摸输入设备 */
            ret = lvgl_port_register_touch();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "[WARN] Touch register failed, UI without touch: %s",
                         esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "[OK] Touch registered");
            }

            /* 5. 壁纸播放器（不触碰 LVGL，但必须在 ui_create 里 attach 之前就绪） */
#if CONFIG_SMART_HOME_STANDBY_ENABLED && CONFIG_SMART_HOME_WALLPAPER_ENABLED
            ESP_LOGI(TAG, "[WALLPAPER] Initializing...");
            ret = wallpaper_player_init(LCD_WIDTH, LCD_HEIGHT);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "[WARN] Wallpaper player init failed: %s",
                         esp_err_to_name(ret));
            } else {
                wallpaper_player_set_target_fps(CONFIG_SMART_HOME_WALLPAPER_FPS);
                ESP_LOGI(TAG, "[OK] Wallpaper player ready (%d fps target)",
                         CONFIG_SMART_HOME_WALLPAPER_FPS);
            }
#endif

            /* 6. 创建 UI + 待机层。 lv_async_call mutates LVGL's timer
             * list, so serialize the enqueue with the adapter task too. */
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                lv_async_call(ui_create, NULL);
                esp_lv_adapter_unlock();
            } else {
                ESP_LOGE(TAG, "[LVGL] failed to lock adapter for UI init");
            }

            /* 7. 天气服务（后台任务；等 Wi-Fi、授时、拉取天气，绝不阻塞 LVGL） */
#if CONFIG_SMART_HOME_STANDBY_ENABLED && CONFIG_SMART_HOME_WEATHER_ENABLED
            weather_service_register_cb(weather_updated_cb, NULL);
            ret = weather_service_start();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "[WARN] Weather service start failed: %s",
                         esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "[OK] Weather service started");
            }
#endif
        }
    } else {
        ESP_LOGW(TAG, "[SKIP] 无显示，跳过 LVGL 与 UI 初始化");
    }
#else
    ESP_LOGI(TAG, "[SKIP] headless — LVGL/UI 不初始化");
#endif /* CONFIG_SMART_HOME_LCD_ENABLED */

    ESP_LOGI(TAG, "=== System running (ui=%s) ===",
#if CONFIG_SMART_HOME_LCD_ENABLED
             ui_available ? "on" : "off");
#else
             "off");
#endif

    /* 7. 主循环 */
#if CONFIG_SMART_HOME_OTA_ENABLED
    /* 系统初始化完成：确认当前镜像可用，取消 bootloader 回滚。 */
    ota_update_mark_valid();
#endif
    int64_t last_alive_us = esp_timer_get_time();
    int counter = 0;
    while (1) {
        /* 协议收包 + TX 重传 + 链路状态机 + HELLO 协商 */
        AppProto_Poll();
#if CONFIG_SMART_HOME_CAMERA_ENABLED
        /* PIR 上升沿 → 全屏预览 + 人脸检测（P4） */
        vision_event_tick();
#endif
        rule_runtime_tick();

#if CONFIG_SMART_HOME_MQTT_ENABLED
        /* 云端指令：MQTT 事件任务只投队列，这里在主任务上下文统一下发，
         * 避免与 ProtoTx 的单发送槽发生跨任务写竞争。 */
        uint8_t cdev, cact;
        if (mqtt_service_take_cloud_cmd(&cdev, &cact)) {
            AppProto_SendActuatorCmdEx(cdev, cact, 0, PROTO_CMD_SRC_CLOUD);
        }
#endif

        /* 看门狗喂狗 */
        wdt_feed();

        /* 每 5s 心跳日志 */
        int64_t now = esp_timer_get_time();
        if (now - last_alive_us > 5000000) {
            last_alive_us = now;
            counter++;
            ESP_LOGI(TAG, "Alive [%d]  link=%s",
                     counter,
                     AppProto_GetLinkState() == LINK_UP ? "UP" :
                     AppProto_GetLinkState() == LINK_DEGRADED ? "DEGR" : "DOWN");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
