/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi.h"
#include "wifi_credentials.h"
#include "storage.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

/* ─── 日志标签 ─── */
static const char *TAG = "WIFI";

/* ─── 事件标志 ─── */
#define WIFI_CONNECTED_BIT       BIT0
#define WIFI_FAIL_BIT            BIT1

/* ─── 重连配置 ─── */
#define WIFI_MAX_RETRY           5
#define WIFI_RETRY_MS_BASE       1000    /* 重连间隔基数，每次 ×2 退避 */

/* ─── NVS 存储键名 ─── */
#define NVS_KEY_WIFI_SSID        "wifi_ssid"
#define NVS_KEY_WIFI_PASSWORD    "wifi_pwd"

/* ─── 静态变量 ─── */

static wifi_state_t s_state = WIFI_STATE_DISCONNECTED;
static EventGroupHandle_t s_event_group = NULL;
static esp_netif_t *s_netif_sta = NULL;
static wifi_state_cb_t s_cb = NULL;
static void *s_cb_user_data = NULL;

/* 重连计数器 */
static int s_retry_count = 0;
static bool s_auto_reconnect = true;

/* 异步扫描状态（事件任务写，UI 任务读，用互斥锁保护结果数组） */
static wifi_ap_info_t s_scan_results[WIFI_SCAN_MAX_AP];
static int s_scan_count = 0;
static volatile bool s_scan_in_progress = false;
static SemaphoreHandle_t s_scan_lock = NULL;

/* ─── 前向声明 ─── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void set_state(wifi_state_t new_state);
static void start_reconnect_timer(void);

/* ─── 状态管理 ─── */
static void set_state(wifi_state_t new_state)
{
    if (s_state == new_state) return;
    wifi_state_t old = s_state;
    s_state = new_state;
    ESP_LOGI(TAG, "State: %d -> %d", old, new_state);
    if (s_cb) {
        s_cb(new_state, s_cb_user_data);
    }
}

/* ─── WiFi 事件处理器 ─── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Connected to AP");
        set_state(WIFI_STATE_CONNECTING);
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason: %d", disc->reason);
        set_state(WIFI_STATE_DISCONNECTED);

        if (s_auto_reconnect && s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            start_reconnect_timer();
        } else if (s_auto_reconnect) {
            ESP_LOGE(TAG, "Max retries reached, giving up");
            set_state(WIFI_STATE_ERROR);
            s_retry_count = 0;
        }
        break;
    }

    case WIFI_EVENT_STA_STOP:
        ESP_LOGI(TAG, "STA stopped");
        set_state(WIFI_STATE_DISCONNECTED);
        break;

    case WIFI_EVENT_SCAN_DONE: {
        uint16_t total = 0;
        esp_wifi_scan_get_ap_num(&total);
        ESP_LOGI(TAG, "Scan done, %u AP found", (unsigned)total);

        /* 记录缓冲区较大，放堆上避免占用事件任务栈 */
        wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(
            WIFI_SCAN_MAX_AP * sizeof(wifi_ap_record_t));
        if (!records) {
            ESP_LOGE(TAG, "Scan result alloc failed");
            s_scan_in_progress = false;
            break;
        }

        uint16_t num = WIFI_SCAN_MAX_AP;    /* 入参=容量，出参=实际条数 */
        if (esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) {
            num = 0;
        }

        /* 过滤空 SSID，逐个拷入本地数组 */
        wifi_ap_info_t sorted[WIFI_SCAN_MAX_AP];
        int count = 0;
        for (uint16_t i = 0; i < num && count < WIFI_SCAN_MAX_AP; i++) {
            if (records[i].ssid[0] == '\0') continue;
            memcpy(sorted[count].ssid, records[i].ssid, sizeof(sorted[count].ssid) - 1);
            sorted[count].ssid[sizeof(sorted[count].ssid) - 1] = '\0';   /* 保证 NUL 结尾 */
            sorted[count].rssi = records[i].rssi;
            sorted[count].authmode = (uint8_t)records[i].authmode;
            count++;
        }
        free(records);

        /* 插入排序：RSSI 从强到弱 */
        for (int i = 1; i < count; i++) {
            wifi_ap_info_t key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j].rssi < key.rssi) {
                sorted[j + 1] = sorted[j];
                j--;
            }
            sorted[j + 1] = key;
        }

        /* 写入共享结果，读数方需持锁拷贝 */
        if (s_scan_lock) xSemaphoreTake(s_scan_lock, portMAX_DELAY);
        memcpy(s_scan_results, sorted, (size_t)count * sizeof(wifi_ap_info_t));
        s_scan_count = count;
        if (s_scan_lock) xSemaphoreGive(s_scan_lock);

        s_scan_in_progress = false;
        break;
    }

    default:
        break;
    }
}

/* ─── IP 事件处理器 ─── */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        set_state(WIFI_STATE_CONNECTED);
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        break;
    }

    case IP_EVENT_STA_LOST_IP:
        ESP_LOGW(TAG, "Lost IP");
        set_state(WIFI_STATE_CONNECTING);
        break;

    default:
        break;
    }
}

/* ─── 重连定时器回调（在 esp_timer 回调中不能调用 WiFi API，转投任务） ─── */
static void reconnect_task(void *pvParameters)
{
    int delay_ms = WIFI_RETRY_MS_BASE * (1 << (s_retry_count - 1));
    if (delay_ms > 30000) delay_ms = 30000;
    ESP_LOGI(TAG, "Reconnecting in %d ms (attempt %d/%d)...",
             delay_ms, s_retry_count, WIFI_MAX_RETRY);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}

static void start_reconnect_timer(void)
{
    xTaskCreate(reconnect_task, "wifi_reconn", 3072, NULL, 5, NULL);
}

/* ─── 初始化 ─── */
esp_err_t wifi_init(void)
{
    /* 创建事件组 */
    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
        if (!s_event_group) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 创建扫描结果互斥锁 */
    if (!s_scan_lock) {
        s_scan_lock = xSemaphoreCreateMutex();
        if (!s_scan_lock) {
            ESP_LOGE(TAG, "Failed to create scan lock");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 初始化 TCP/IP 网络接口 */
    ESP_LOGI(TAG, "Initializing esp_netif...");
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 创建默认事件循环 */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 创建 STA 网络接口 */
    s_netif_sta = esp_netif_create_default_wifi_sta();
    if (!s_netif_sta) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return ESP_FAIL;
    }

    /* WiFi 初始化配置 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    /*
     * esp_wifi_init() 会被 esp_wifi_remote 注入的 esp_wifi.h 自动拦截，
     * 透明地通过 SDIO Slot 1 转发到 ESP32-C6 (esp_hosted slave)。
     * 后续所有标准 esp_wifi_*() API 调用均同理。
     */
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "WiFi remote initialized (C6 over SDIO Slot 1)");

    /* 注册事件处理器 */
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                     &ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP lost handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi initialized successfully");
    return ESP_OK;
}

/* ─── 连接 STA ─── */
esp_err_t wifi_connect_sta(const char *ssid, const char *password, bool auto_save)
{
    wifi_config_t wifi_config = {0};
    bool secured = false;
    if (!wifi_prepare_credentials(ssid, password, wifi_config.sta.ssid,
                                  wifi_config.sta.password, &secured)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config.sta.threshold.authmode = secured ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* 保存到 NVS（如果需要） */
    if (auto_save) {
        esp_err_t ret = storage_set_str(NVS_KEY_WIFI_PASSWORD, password ? password : "");
        if (ret != ESP_OK) return ret;
        ret = storage_set_str(NVS_KEY_WIFI_SSID, ssid);
        if (ret != ESP_OK) return ret;
        ESP_LOGI(TAG, "Credentials saved to NVS");
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 关闭省电模式：ESP-Hosted(C6) 的 modem sleep 会让 MQTT keepalive 与
     * HTTP 请求出现秒级延迟，OTA 下载时表现为服务器侧超时。
     * 面板常供电，续航不是约束，因此固定 WIFI_PS_NONE。 */
    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(ret));
    }

    set_state(WIFI_STATE_CONNECTING);
    s_retry_count = 0;

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/* ─── 从 NVS 连接，若无可回退到 sdkconfig 默认值 ─── */
esp_err_t wifi_connect_from_nvs(void)
{
    char ssid[64] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pwd_len = sizeof(password);

    esp_err_t ret = storage_get_str(NVS_KEY_WIFI_SSID, ssid, &ssid_len);
    if (ret == ESP_OK) {
        storage_get_str(NVS_KEY_WIFI_PASSWORD, password, &pwd_len);
        ESP_LOGI(TAG, "Found saved credentials for: %s", ssid);
        return wifi_connect_sta(ssid, password, false);
    }

    /* NVS 没有凭据，回退到 sdkconfig 默认值 */
# ifdef CONFIG_SMART_HOME_WIFI_SSID
    const char *def_ssid = CONFIG_SMART_HOME_WIFI_SSID;
    const char *def_pwd  = CONFIG_SMART_HOME_WIFI_PASSWORD;
    if (def_ssid && strlen(def_ssid) > 0) {
        ESP_LOGI(TAG, "Using sdkconfig defaults for: %s", def_ssid);
        return wifi_connect_sta(def_ssid, def_pwd ? def_pwd : "", false);
    }
# endif

    return ESP_ERR_NOT_FOUND;
}

/* ─── 断开 ─── */
esp_err_t wifi_disconnect(void)
{
    s_auto_reconnect = false;
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(ret));
    }
    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
    }
    set_state(WIFI_STATE_DISCONNECTED);
    s_auto_reconnect = true;
    return ESP_OK;
}

/* ─── 查询状态 ─── */
wifi_state_t wifi_get_state(void)
{
    return s_state;
}

bool wifi_is_connected(void)
{
    return (s_state == WIFI_STATE_CONNECTED);
}

bool wifi_get_ip_info(char *ip, char *gw, char *netmask, size_t buf_size)
{
    if (!s_netif_sta) return false;

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_netif_sta, &ip_info);
    if (ret != ESP_OK) return false;

    if (ip) {
        esp_ip4addr_ntoa(&ip_info.ip, ip, buf_size);
    }
    if (gw) {
        esp_ip4addr_ntoa(&ip_info.gw, gw, buf_size);
    }
    if (netmask) {
        esp_ip4addr_ntoa(&ip_info.netmask, netmask, buf_size);
    }
    return true;
}

/* ─── 回调注册 ─── */
void wifi_register_callback(wifi_state_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_cb_user_data = user_data;
}

void wifi_unregister_callback(void)
{
    s_cb = NULL;
    s_cb_user_data = NULL;
}

/* ─── 异步扫描 ─── */
esp_err_t wifi_scan_start(void)
{
    /* 上次扫描尚未完成，直接拒绝 */
    if (s_scan_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_in_progress = true;

    /* 非阻塞扫描，结果在 WIFI_EVENT_SCAN_DONE 中取 */
    esp_err_t ret = esp_wifi_scan_start(NULL, false);
    if (ret != ESP_OK) {
        s_scan_in_progress = false;
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool wifi_scan_in_progress(void)
{
    return s_scan_in_progress;
}

int wifi_scan_result_count(void)
{
    int count = 0;
    if (s_scan_lock) {
        xSemaphoreTake(s_scan_lock, portMAX_DELAY);
        count = s_scan_count;
        xSemaphoreGive(s_scan_lock);
    }
    return count;
}

bool wifi_scan_result_get(int index, wifi_ap_info_t *out)
{
    if (!out || index < 0 || !s_scan_lock) {
        return false;
    }

    bool ok = false;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (index < s_scan_count) {
        *out = s_scan_results[index];   /* 拷贝出，不暴露内部指针 */
        ok = true;
    }
    xSemaphoreGive(s_scan_lock);
    return ok;
}
