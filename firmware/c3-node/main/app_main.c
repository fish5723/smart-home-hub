/**
 * @file app_main.c
 * @brief 阶段 6 最小验证固件入口
 *
 * 验证目标（三条，全部通过才写阶段 6 spec）：
 *   1. NimBLE GATT Server 可被手机发现/连接/订阅，Notify 正常到达
 *   2. 同时 Wi-Fi 常连 + 每秒 MQTT 发布，10 分钟不出现互斥导致的断连
 *   3. 连接态下 BLE 与 Wi-Fi 双向流量都在（后续再加 Light-sleep 保持测试）
 *
 * 注意：载荷是链路测试序号，不是真实传感器数据，看日志时别当成测量值。
 */
#include <stdio.h>

#include "ble_svc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_link.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soc/rtc.h"

static const char *TAG = "MAIN";

#define STATS_PERIOD_S 10U
#define FRAME_TYPE_SENSOR_REPORT 0x10  /* docs/03 §3.2 */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 诊断：慢时钟到底来自外部 32.768kHz 还是内部 RC。
     * src 为 soc_rtc_slow_clk_src_t，hz≈32768 才算晶振在工作。 */
    ESP_LOGI(TAG, "rtc slow clk: src=%d hz=%lu xtal_en=%d",
             (int)rtc_clk_slow_src_get(),
             (unsigned long)rtc_clk_slow_freq_get_hz(),
             (int)rtc_clk_32k_enabled());

    /* 与 docs/03 §2.3 一致的检查值：CRC16("123456789") 必须等于 0x29B1 */
    if (frame_selftest()) {
        ESP_LOGI(TAG, "frame crc selftest PASS (0x%04X)", FRAME_CRC_CHECK);
    } else {
        ESP_LOGE(TAG, "frame crc selftest FAIL");
    }

    /* 验收 8：light sleep 由 CONFIG_C3_PM_LIGHT_SLEEP 控制，便于 A/B 对照 */
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
#if CONFIG_C3_PM_LIGHT_SLEEP
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };
    ret = esp_pm_configure(&pm);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "pm: freq 80~160MHz, light_sleep=%d",
                 (int)pm.light_sleep_enable);
    } else {
        ESP_LOGE(TAG, "pm configure failed: %s", esp_err_to_name(ret));
    }

#if CONFIG_C3_BLE_ENABLE
    ret = ble_svc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败: %s", esp_err_to_name(ret));
    }
#endif

    ret = net_link_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "网络初始化失败: %s", esp_err_to_name(ret));
    }

    uint16_t seq = 0;
    uint32_t last_stats_s = 0;
    uint8_t last_prov = 0xFF;
    uint8_t last_reason = 0xFF;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        seq++;

        const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);

        net_link_stats_t net;
        net_link_get_stats(&net);

        ble_svc_stats_t ble;
        ble_svc_get_stats(&ble);

        /* 配网/连接状态变化才推 FFF6，避免每秒刷一条 */
        uint8_t reason = 0;
        const uint8_t prov = net_link_get_state(&reason);
        if (prov != last_prov || reason != last_reason) {
            last_prov = prov;
            last_reason = reason;
            ble_svc_prov_notify(prov, reason);
        }

        const uint8_t flags = (uint8_t)((net.mqtt_up ? 0x01U : 0U) |
                                        (ble.connected ? 0x02U : 0U));

        /* BLE 与 MQTT 用同一份字节：阶段二帧里只放 rssi + flags，seq 由帧头携带 */
        const uint8_t fpayload[2] = { (uint8_t)net.rssi, flags };
        uint8_t frame[16];
        const uint16_t flen = frame_build(frame, sizeof(frame), FRAME_TYPE_SENSOR_REPORT,
                                          seq, fpayload, sizeof(fpayload));
        if (flen > 0) {
            ble_svc_notify_frame(frame, flen);
            net_link_publish(seq, up_s, frame, flen);
        }

        if (up_s - last_stats_s >= STATS_PERIOD_S) {
            last_stats_s = up_s;
            ESP_LOGI(TAG,
                     "STAT up=%lus | BLE conn=%lu disc=%lu sub=%u mtu=%u nfail=%lu | "
                     "prov=%u reason=%u | WiFi disc=%lu rssi=%d | MQTT up=%u disc=%lu ok=%lu | "
                     "heap=%lu min=%lu",
                     (unsigned long)up_s,
                     (unsigned long)ble.conn_count,
                     (unsigned long)ble.disc_count,
                     (unsigned)ble.subscribed,
                     (unsigned)ble.mtu,
                     (unsigned long)ble.notify_fail,
                     (unsigned)prov,
                     (unsigned)reason,
                     (unsigned long)net.wifi_disconnects,
                     (int)net.rssi,
                     (unsigned)net.mqtt_up,
                     (unsigned long)net.mqtt_disconnects,
                     (unsigned long)net.mqtt_connects,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());

#if CONFIG_PM_PROFILING
            /* PM 统计：各功耗状态累计耗时 + light sleep 成功/被拒次数。
             * 这是判断"到底进没进 light sleep"的硬证据，比看电流直接。 */
            esp_pm_dump_locks(stdout);
#endif
        }
    }
}
