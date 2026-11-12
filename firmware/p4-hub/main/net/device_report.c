/**
 * @file device_report.c
 * @brief 设备属性 / OTA 结果 MQTT 上报实现（见 device_report.h）
 *
 * 说明：
 *   - 属性快照用 retain 发布，新订阅者能立即拿到最新状态；
 *   - OTA 结果由 net_ota 回调触发，发布 smart_home/<node>/ota/result；
 *   - 未连接 broker 时静默跳过，连接恢复后立即补发一次。
 */
#include "device_report.h"

#include "sdkconfig.h"

#ifdef CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_ENABLED

#include "topic.h"
#include "mqtt_service.h"
#include "app_proto.h"
#include "net_ota.h"
#include "sd_card.h"
#include "wifi.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "DEV_RPT";

#ifndef CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_PERIOD_S
#define CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_PERIOD_S 60
#endif

static volatile bool s_force;
static TaskHandle_t s_task;

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:     return "esp32";
    case CHIP_ESP32S2:   return "esp32s2";
    case CHIP_ESP32S3:   return "esp32s3";
    case CHIP_ESP32C3:   return "esp32c3";
    case CHIP_ESP32C2:   return "esp32c2";
    case CHIP_ESP32C6:   return "esp32c6";
    case CHIP_ESP32H2:   return "esp32h2";
    case CHIP_ESP32P4:   return "esp32p4";
    case CHIP_ESP32C61:  return "esp32c61";
    case CHIP_ESP32C5:   return "esp32c5";
    case CHIP_ESP32H21:  return "esp32h21";
    case CHIP_ESP32H4:   return "esp32h4";
    case CHIP_ESP32S31:  return "esp32s31";
    default:             return "unknown";
    }
}

static const char *link_name(link_state_t st)
{
    switch (st) {
    case LINK_UP:       return "UP";
    case LINK_DEGRADED: return "DEGRADED";
    default:            return "DOWN";
    }
}

static const char *ota_status_name(net_ota_status_t st)
{
    switch (st) {
    case NET_OTA_IDLE:        return "IDLE";
    case NET_OTA_CHECKING:    return "CHECKING";
    case NET_OTA_UP_TO_DATE:  return "UP_TO_DATE";
    case NET_OTA_DOWNLOADING: return "DOWNLOADING";
    case NET_OTA_SUCCESS:     return "SUCCESS";
    case NET_OTA_FAILED:      return "FAILED";
    case NET_OTA_OFFLINE:     return "OFFLINE";
    default:                  return "UNKNOWN";
    }
}

static char *build_payload(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    int64_t uptime_us = esp_timer_get_time();
    cJSON_AddNumberToObject(root, "ts_us", (double)uptime_us);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(uptime_us / 1000000LL));

    /* ── 硬件 ── */
    cJSON *dev = cJSON_CreateObject();
    if (dev) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        cJSON_AddStringToObject(dev, "model", "JC-ESP32P4-M3-DEV");
        cJSON_AddStringToObject(dev, "chip", chip_model_name(chip.model));
        cJSON_AddNumberToObject(dev, "cores", chip.cores);
        cJSON_AddNumberToObject(dev, "revision", chip.revision);

        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
            (void)esp_read_mac(mac, ESP_MAC_BASE);
        }
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(dev, "mac", mac_str);

        uint32_t flash_bytes = 0;
        if (esp_flash_get_size(esp_flash_default_chip, &flash_bytes) == ESP_OK) {
            cJSON_AddNumberToObject(dev, "flash_mb",
                                    (double)(flash_bytes / (1024U * 1024U)));
        }
        size_t psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        cJSON_AddNumberToObject(dev, "psram_mb",
                                (double)(psram_bytes / (1024U * 1024U)));
        cJSON_AddItemToObject(root, "dev", dev);
    }

    /* ── 固件 ── */
    cJSON *fw = cJSON_CreateObject();
    if (fw) {
        const esp_app_desc_t *desc = esp_app_get_description();
        const esp_partition_t *part = esp_ota_get_running_partition();
        cJSON_AddStringToObject(fw, "project",
                                (desc && desc->project_name[0])
                                ? desc->project_name : "p4-hub");
        cJSON_AddStringToObject(fw, "version",
                                (desc && desc->version[0]) ? desc->version : "");
        cJSON_AddStringToObject(fw, "idf_ver",
                                (desc && desc->idf_ver[0]) ? desc->idf_ver : "");
        cJSON_AddStringToObject(fw, "partition",
                                part ? part->label : "");
        cJSON_AddItemToObject(root, "fw", fw);
    }

    /* ── Wi-Fi ── */
    cJSON *wifi = cJSON_CreateObject();
    if (wifi) {
        bool connected = false;
        char ip[16] = {0};
#if CONFIG_SMART_HOME_WIFI_ENABLED
        connected = wifi_is_connected();
        if (connected) {
            (void)wifi_get_ip_info(ip, NULL, NULL, sizeof(ip));
        }
#endif
        cJSON_AddBoolToObject(wifi, "connected", connected);
        cJSON_AddStringToObject(wifi, "ip", ip);

        int rssi = 0;
        bool has_rssi = false;
#if CONFIG_SMART_HOME_WIFI_ENABLED
        if (connected) {
            wifi_ap_record_t ap = {0};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
                has_rssi = true;
            }
        }
#endif
        if (has_rssi) cJSON_AddNumberToObject(wifi, "rssi", rssi);
        cJSON_AddItemToObject(root, "wifi", wifi);
    }

    /* ── F103 ── */
    cJSON *f103 = cJSON_CreateObject();
    if (f103) {
        app_proto_peer_info_t peer;
        bool valid = AppProto_GetPeerInfo(&peer);
        cJSON_AddStringToObject(f103, "link",
                                link_name(AppProto_GetLinkState()));
        cJSON_AddBoolToObject(f103, "valid", valid);
        if (valid) {
            cJSON_AddNumberToObject(f103, "proto_ver", peer.proto_ver);
            cJSON_AddNumberToObject(f103, "caps", peer.caps);
            cJSON_AddStringToObject(f103, "name", peer.name);
        }
        cJSON_AddItemToObject(root, "f103", f103);
    }

    /* ── SD 卡 ── */
    cJSON *sd = cJSON_CreateObject();
    if (sd) {
        bool mounted = sd_card_is_mounted();
        int total_mb = 0, free_mb = 0;
        if (mounted) {
            (void)sd_card_get_capacity(&total_mb, &free_mb);
        }
        cJSON_AddBoolToObject(sd, "mounted", mounted);
        cJSON_AddNumberToObject(sd, "total_mb", total_mb);
        cJSON_AddNumberToObject(sd, "free_mb", free_mb);
        cJSON_AddItemToObject(root, "sd", sd);
    }

    /* ── OTA 状态 ── */
    cJSON *ota = cJSON_CreateObject();
    if (ota) {
        const net_ota_result_t *res = net_ota_get_last_result();
        cJSON_AddStringToObject(ota, "status",
                                ota_status_name(res ? res->status
                                                    : NET_OTA_IDLE));
        cJSON_AddStringToObject(ota, "current", res ? res->current : "");
        cJSON_AddStringToObject(ota, "target", res ? res->target : "");
        cJSON_AddStringToObject(ota, "sha256", res ? res->sha256 : "");
        cJSON_AddStringToObject(ota, "error", res ? res->error : "");
        cJSON_AddItemToObject(root, "ota", ota);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static void publish_device_info(void)
{
    if (!mqtt_service_connected()) return;

    char *payload = build_payload();
    if (!payload) {
        ESP_LOGW(TAG, "payload build failed (no mem)");
        return;
    }

    char topic[96];
    if (topic_device_info(topic, sizeof(topic)) < 0) {
        cJSON_free(payload);
        return;
    }

    if (mqtt_service_publish(topic, payload, 1, true)) {
        ESP_LOGI(TAG, "device info published (%u bytes)", (unsigned)strlen(payload));
    } else {
        ESP_LOGW(TAG, "device info publish failed");
    }
    cJSON_free(payload);
}

static void publish_ota_result(const net_ota_result_t *res)
{
    if (!res || !mqtt_service_connected()) return;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    int64_t uptime_us = esp_timer_get_time();
    cJSON_AddStringToObject(root, "status", ota_status_name(res->status));
    cJSON_AddStringToObject(root, "current", res->current);
    cJSON_AddStringToObject(root, "target", res->target);
    cJSON_AddStringToObject(root, "sha256", res->sha256);
    cJSON_AddStringToObject(root, "error", res->error);
    cJSON_AddNumberToObject(root, "ts_us", (double)res->ts_us);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(uptime_us / 1000000LL));

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    char topic[96];
    if (topic_ota_result(topic, sizeof(topic)) >= 0) {
        if (mqtt_service_publish(topic, payload, 1, true)) {
            ESP_LOGI(TAG, "OTA result published: %s (target=%s err=%s)",
                     ota_status_name(res->status), res->target, res->error);
        }
    }
    cJSON_free(payload);

    /* 结果同时刷新属性快照，便于平台只订阅 device 也能看到最新状态 */
    s_force = true;
}

static void device_report_task(void *arg)
{
    (void)arg;
    bool was_connected = false;
    int64_t next_us = 0;

    for (;;) {
        bool connected = mqtt_service_connected();
        int64_t now = esp_timer_get_time();

        if (connected && (!was_connected || s_force || now >= next_us)) {
            s_force = false;
            publish_device_info();
            next_us = now +
                      (int64_t)CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_PERIOD_S * 1000000LL;
        }
        was_connected = connected;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t device_report_init(void)
{
    if (s_task) return ESP_OK;

    net_ota_set_result_cb(publish_ota_result);
    s_force = true;

    if (xTaskCreate(device_report_task, "dev_report", 4096, NULL, 4,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "device report ready (period %ds)",
             CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_PERIOD_S);
    return ESP_OK;
}

void device_report_request(void)
{
    s_force = true;
}

#else /* CONFIG_SMART_HOME_MQTT_DEVICE_REPORT_ENABLED */

esp_err_t device_report_init(void)
{
    return ESP_ERR_NOT_FOUND;
}

void device_report_request(void)
{
}

#endif
