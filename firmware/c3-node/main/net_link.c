/**
 * @file net_link.c
 * @brief Wi-Fi STA + MQTT + BLE 配网落盘（阶段 6）
 *
 * 凭据来源优先级：NVS（BLE 配网写入）> Kconfig 默认值。
 * 两者都没有时只跑 BLE，等手机通过 0xFFF4/0xFFF5 写进来。
 */
#include "net_link.h"
#include "net_link_credentials.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_crt_bundle.h"

static const char *TAG = "NET";

#define NVS_NS       "netcfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

#define WIFI_RETRY_MAX 3

static esp_mqtt_client_handle_t s_client;
static net_link_stats_t s_stats;
static esp_timer_handle_t s_reconnect_timer;

/* docs/17 命名：smart_home/<node_id>/{availability,state/...} */
static char s_topic_avail[96];
static char s_topic_rssi[96];

static uint8_t s_stack_inited;
static uint8_t s_wifi_started;
static char    s_ssid[NET_LINK_SSID_MAX + 1];
static char    s_pass[NET_LINK_PASS_MAX + 1];
static uint8_t s_state = NET_LINK_IDLE;
static uint8_t s_reason;
static uint8_t s_attempts;

/* 打印断线原因的可读名称；只覆盖调试时真正会遇到的几个 */
static const char *reason_name(uint8_t reason)
{
    switch (reason) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "OTHER";
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* ─────────────────────── 凭据（NVS） ─────────────────────── */

static uint8_t nvs_read_str(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }

    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);

    return (err == ESP_OK) ? 1U : 0U;
}

static esp_err_t load_credentials(void)
{
    uint8_t from_nvs = nvs_read_str(NVS_KEY_SSID, s_ssid, sizeof(s_ssid));

    if (!from_nvs) {
        if (strlen(CONFIG_C3_WIFI_SSID) > NET_LINK_SSID_MAX) return ESP_ERR_INVALID_ARG;
        strlcpy(s_ssid, CONFIG_C3_WIFI_SSID, sizeof(s_ssid));
    }
    if (!nvs_read_str(NVS_KEY_PASS, s_pass, sizeof(s_pass))) {
        if (strlen(CONFIG_C3_WIFI_PASSWORD) > NET_LINK_PASS_MAX) return ESP_ERR_INVALID_ARG;
        strlcpy(s_pass, CONFIG_C3_WIFI_PASSWORD, sizeof(s_pass));
    }

    if (strlen(s_ssid) == 0) {
        ESP_LOGW(TAG, "no SSID in NVS or Kconfig, waiting for BLE provisioning");
        s_state = NET_LINK_IDLE;
    } else {
        ESP_LOGI(TAG, "credential source=%s ssid='%s'", from_nvs ? "NVS" : "Kconfig", s_ssid);
    }
    return ESP_OK;
}

static esp_err_t save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ─────────────────────────── Wi-Fi ─────────────────────────── */

/* 开机先扫一次：把目标 AP 的真实 authmode / 信道 / RSSI 打出来，再连接。
 * 这样 reason=2 这类"连不上"能立刻区分是密码问题还是射频/共存问题。 */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        ESP_LOGI(TAG, "scan found %u AP(s)", (unsigned)num);

        if (num > 20) {
            num = 20;
        }
        wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
        if (recs != NULL) {
            if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
                for (uint16_t i = 0; i < num; i++) {
                    ESP_LOGI(TAG, "AP[%u] ssid='%s' rssi=%d ch=%u auth=%d",
                             (unsigned)i, (const char *)recs[i].ssid,
                             recs[i].rssi, (unsigned)recs[i].primary,
                             (int)recs[i].authmode);
                }
            }
            free(recs);
        }
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        s_stats.wifi_disconnects++;
        s_stats.mqtt_up = 0;
        s_reason = (uint8_t)e->reason;
        ESP_LOGW(TAG, "wifi down reason=%d(%s) (total=%lu, try=%u/%d)",
                 e->reason, reason_name((uint8_t)e->reason),
                 (unsigned long)s_stats.wifi_disconnects,
                 (unsigned)s_attempts, WIFI_RETRY_MAX);

        s_attempts++;
        if (s_attempts >= WIFI_RETRY_MAX) {
            s_state = NET_LINK_FAILED;
            ESP_LOGE(TAG, "连续 %u 次失败，停止重试；改凭据请用 BLE 配网",
                     (unsigned)s_attempts);
            return;
        }

        s_state = NET_LINK_CONNECTING;
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, 3000ULL * 1000ULL);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_state = NET_LINK_CONNECTED;
        s_attempts = 0;
        s_reason = 0;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

/* 把当前凭据写进 Wi-Fi 驱动并连接；凭据变更时先断开再重配 */
static void wifi_apply_config(void)
{
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    /* ESP-IDF 的字段是定长字节数组，恰好 32/64 字节时不要求 NUL 结尾。 */
    memcpy(wc.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(wc.sta.password, s_pass, strlen(s_pass));
    wc.sta.threshold.authmode = (strlen(s_pass) == 0) ? WIFI_AUTH_OPEN
                                                     : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_state = NET_LINK_CONNECTING;
    s_attempts = 0;
    esp_wifi_connect();
}

/* ──────────────────────────── MQTT ──────────────────────────── */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const esp_mqtt_event_handle_t e = data;

    switch (e->event_id) {
    case MQTT_EVENT_CONNECTED:
        s_stats.mqtt_up = 1;
        s_stats.mqtt_connects++;
        ESP_LOGI(TAG, "mqtt connected (total=%lu)",
                 (unsigned long)s_stats.mqtt_connects);
        /* 上线即宣告 available（LWT 会在异常断开时把同一个 topic 置 offline） */
        esp_mqtt_client_publish(s_client, s_topic_avail, "online", 0, 1, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_stats.mqtt_up = 0;
        s_stats.mqtt_disconnects++;
        ESP_LOGW(TAG, "mqtt down (total=%lu)",
                 (unsigned long)s_stats.mqtt_disconnects);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error type=%d", e->error_handle->error_type);
        break;

    default:
        break;
    }
}

/* 网络栈只初始化一次；BLE 配网可能在开机很久之后才触发 */
static esp_err_t stack_start(void)
{
    if (s_stack_inited) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL));

    const esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    snprintf(s_topic_avail, sizeof(s_topic_avail), "smart_home/%s/availability",
             CONFIG_C3_MQTT_NODE_ID);
    snprintf(s_topic_rssi, sizeof(s_topic_rssi), "smart_home/%s/state/rssi",
             CONFIG_C3_MQTT_NODE_ID);

    esp_mqtt_client_config_t mqtt_cfg;
    memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));
    mqtt_cfg.broker.address.uri = CONFIG_C3_MQTT_URI;
    mqtt_cfg.session.keepalive = CONFIG_C3_MQTT_KEEPALIVE_S;
    /* mqtts:// 时用 crt bundle 校验服务器证书（明文 mqtt:// 不受影响）；
     * bundle 含项目 certs/ca.pem 的本地自签 CA（见 tools/gen_mqtt_certs.py） */
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;
    /* LWT：异常掉线时 broker 替我们把 availability 置为 offline（docs/17） */
    mqtt_cfg.session.last_will.topic = s_topic_avail;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt init failed");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    s_stack_inited = 1;
    return ESP_OK;
}

/* 射频参数必须在 esp_wifi_start() 之后设置，否则驱动返回 invalid state 并 abort。
 * C3 SuperMini 板载天线驻波比约 5.8：满功率发射时反射/失真严重，
 * AP 侧连认证帧都收不到（现象为 AUTH_EXPIRE 而 RSSI 很好）。
 * 降功率是社区验证过的第一条自救手段，因此做成可配置项便于对照。 */
static void wifi_tune(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_C3_WIFI_TX_POWER_QDBM));
    int8_t tx_qdbm = 0;
    if (esp_wifi_get_max_tx_power(&tx_qdbm) == ESP_OK) {
        ESP_LOGI(TAG, "max tx power=%d (%.2f dBm)", tx_qdbm, tx_qdbm / 4.0);
    }

    /* 保持默认省电模式，先验证"常连 + 每秒发布"下的共存；
     * 低功耗四模式的电流对比放到后续单独一轮。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

/* ──────────────────────────── 对外 ──────────────────────────── */

esp_err_t net_link_init(void)
{
    esp_err_t err = load_credentials();
    if (err != ESP_OK) return err;
    if (strlen(s_ssid) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!net_link_credentials_valid(s_ssid, s_pass)) {
        ESP_LOGE(TAG, "stored Wi-Fi credentials invalid");
        return ESP_ERR_INVALID_ARG;
    }

    err = stack_start();
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = 1;
    wifi_tune();
    wifi_apply_config();

    ESP_LOGI(TAG, "wifi+mqtt started, broker=%s", CONFIG_C3_MQTT_URI);
    return ESP_OK;
}

esp_err_t net_link_set_credentials(const char *ssid, const char *pass)
{
    if (!net_link_credentials_valid(ssid, pass)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存凭据失败: %s", esp_err_to_name(err));
        return err;
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    ESP_LOGI(TAG, "new credentials from BLE: ssid='%s' pass_len=%u",
             s_ssid, (unsigned)strlen(s_pass));

    err = stack_start();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = 1;
        wifi_tune();
    } else {
        esp_timer_stop(s_reconnect_timer);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_apply_config();
    return ESP_OK;
}

uint8_t net_link_get_state(uint8_t *reason)
{
    if (reason != NULL) {
        *reason = s_reason;
    }
    return s_state;
}

void net_link_publish(uint16_t seq, uint32_t uptime_s,
                      const uint8_t *frame, uint16_t frame_len)
{
    if (s_client == NULL || !s_stats.mqtt_up) {
        return;
    }

    /* frame 最多 FRAME_HEADER_SIZE + 256 + 2 = 266 字节，hex 后 532 字符；
     * 本节点载荷只有 2 字节（帧长 12），留够余量即可。 */
    char hex[2 * 32 + 1];
    if (frame == NULL || frame_len > 32) {
        hex[0] = '\0';
    } else {
        for (uint16_t i = 0; i < frame_len; i++) {
            hex[i * 2]     = "0123456789abcdef"[(frame[i] >> 4) & 0x0F];
            hex[i * 2 + 1] = "0123456789abcdef"[frame[i] & 0x0F];
        }
        hex[frame_len * 2] = '\0';
    }

    char payload[160];
    int len = snprintf(payload, sizeof(payload),
                       "{\"seq\":%u,\"rssi\":%d,\"up\":%lu,\"frame\":\"%s\"}",
                       (unsigned)seq, (int)s_stats.rssi,
                       (unsigned long)uptime_s, hex);
    if (len <= 0 || len >= (int)sizeof(payload)) {
        return;
    }

    /* QoS 0：验证共存不需要重传开销 */
    esp_mqtt_client_publish(s_client, CONFIG_C3_MQTT_PUB_TOPIC,
                            payload, len, 0, 0);

    /* docs/17 标准 topic：state/rssi（QoS1 + retain，供 P4 面板与 HA 订阅） */
    char rssi_pl[32];
    int rl = snprintf(rssi_pl, sizeof(rssi_pl), "{\"v\":%d}", (int)s_stats.rssi);
    if (rl > 0 && rl < (int)sizeof(rssi_pl)) {
        esp_mqtt_client_publish(s_client, s_topic_rssi, rssi_pl, rl, 1, 1);
    }
}

void net_link_get_stats(net_link_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    if (s_wifi_started && s_state == NET_LINK_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_stats.rssi = (int8_t)ap.rssi;
        } else {
            s_stats.rssi = 127;
        }
    } else {
        s_stats.rssi = 127;
    }

    *out = s_stats;
}
