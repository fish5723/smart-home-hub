/**
 * @file mqtt_service.c
 * @brief MQTT 服务实现（见 mqtt_service.h）
 */
#include "mqtt_service.h"
#include "topic.h"
#include "ha_entities.h"
#include "ha_discovery.h"
#include "remote_node.h"
#include "storage.h"
#include "app_proto.h"
#include "protocol.h"
#include "wifi.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"

static const char *TAG = "MQTT";

#ifndef CONFIG_SMART_HOME_MQTT_NODE_ID
#define CONFIG_SMART_HOME_MQTT_NODE_ID "p4hub"
#endif
#ifndef CONFIG_SMART_HOME_MQTT_BROKER_URI
#define CONFIG_SMART_HOME_MQTT_BROKER_URI ""
#endif
#ifndef CONFIG_SMART_HOME_MQTT_USERNAME
#define CONFIG_SMART_HOME_MQTT_USERNAME ""
#endif
#ifndef CONFIG_SMART_HOME_MQTT_PASSWORD
#define CONFIG_SMART_HOME_MQTT_PASSWORD ""
#endif
#ifndef CONFIG_SMART_HOME_MQTT_KEEPALIVE_S
#define CONFIG_SMART_HOME_MQTT_KEEPALIVE_S 30
#endif
/* Kconfig bool 关闭时不写入 sdkconfig.h；此处回退 0（=默认关闭），
 * 确保"用户关掉云端控制"一定生效 */
#ifndef CONFIG_SMART_HOME_MQTT_ALLOW_CLOUD_CMD
#define CONFIG_SMART_HOME_MQTT_ALLOW_CLOUD_CMD 0
#endif

/* NVS 键（namespace "storage"；绝不把真实值写进 sdkconfig.defaults，ADR-016） */
#define NVS_KEY_URI  "mqtt_uri"
#define NVS_KEY_USER "mqtt_user"
#define NVS_KEY_PASS "mqtt_pass"
#define NVS_KEY_NODE "mqtt_node"

#define PUB_TICK_MS     25       /* 执行器回读轮询粒度：状态变化 ≤25ms 内上报 */
#define PUB_PERIOD_MS   500      /* 全量增量轮询周期 */
#define KEEPFRESH_MS    40000    /* 强制全量重发周期（须小于 discovery expire_after=60s） */
#define STATE_DEADBAND  0.05f    /* 数值死区，避免无意义抖动发布 */
/* 同一传感器的最小发布间隔。压力/气体等数值每秒都在跳，逐次上报会在
 * esp_hosted 的 SDIO mempool 里堆满小包（实测 mempool OOM → 丢包 → 掉线）。
 * 执行器回读不受此限（云指令端到端仍走 25ms 节拍）。 */
#define SENSOR_MIN_GAP_MS 2000
/* 全量重发时每条之间留出的间隔：esp_hosted SDIO TX/RX 共用 mempool，
 * 瞬时灌入 20+ 条会把池子打空（mempool OOM (TX)），丢包导致 TCP 写阻塞。 */
#define PUB_BURST_GAP_MS 20

/* 与 ha_entities 表规模一致；超出部分不发布 */
#define MAX_ENTITIES 24

/** 云端指令队列元素 */
typedef struct {
    uint8_t dev_id;
    uint8_t action;
} cloud_cmd_t;

#define CLOUD_CMD_Q_LEN 4

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static volatile bool s_want_republish;
static TaskHandle_t  s_task;
static QueueHandle_t s_cmd_q;

static char s_will_topic[96];
static char s_cmd_wildcard[96];
static const char s_will_msg[] = "offline";

static float s_last_v[MAX_ENTITIES];
static bool  s_last_valid[MAX_ENTITIES];
static int64_t s_last_pub_us[MAX_ENTITIES];   /* 上次成功发布时刻（传感器节流用） */

/* discovery 只在首次连接或 HA 重启时重发；普通重连不再全量重发，
 * 避免每次断线重连都灌 18 条配置把小包队列打满。 */
static volatile bool s_discovery_needed = true;

/* 云指令时延打点：入队时刻 → 该设备状态实际上报时刻（仅用于日志） */
static volatile int64_t s_pending_us;
static volatile uint8_t s_pending_dev;
static volatile bool    s_pending;

/* ── 获取某实体的当前值/质量 ──
 * 返回 false = 影子中没有该设备（例如 F103 尚未上报过） */
static bool entity_value(const ha_entity_def_t *e, float *v, uint8_t *q)
{
    if (e->src == HA_SRC_ACTUATOR) {
        actuator_shadow_entry_t acts[8];
        int n = AppProto_GetActuatorShadow(acts, 8);
        for (int i = 0; i < n; i++) {
            if (acts[i].dev_id == e->dev_id) {
                *v = acts[i].state ? 1.0f : 0.0f;
                *q = (AppProto_GetLinkState() == LINK_UP) ? PROTO_QUAL_VALID : PROTO_QUAL_STALE;
                return true;
            }
        }
        return false;
    }

    if (e->src == HA_SRC_SENSOR) {
        sensor_shadow_entry_t sensors[SHADOW_MAX_SENSORS];
        int n = AppProto_GetSensorShadow(sensors, SHADOW_MAX_SENSORS);
        for (int i = 0; i < n; i++) {
            if (sensors[i].dev_id == e->dev_id) {
                *v = sensors[i].value;
                *q = sensors[i].quality;
                return true;
            }
        }
        return false;
    }

    /* 告警：影子只跟踪"最近一次告警"，未匹配的告警实体恒为 0 */
    alarm_shadow_t al;
    AppProto_GetAlarmShadow(&al);
    *v = (al.alarm_id == e->dev_id && al.active) ? 1.0f : 0.0f;
    *q = PROTO_QUAL_VALID;
    return true;
}

static bool publish_state_one(int idx, bool force)
{
    const ha_entity_def_t *e = ha_entities_get(idx);
    if (!e || !e->enabled) return false;

    /* 传感器节流：同一实体最小间隔 SENSOR_MIN_GAP_MS，避免数值抖动
     * 造成每秒多条小包灌满 SDIO mempool。强制全量刷新不受限制。 */
    int64_t now_us = esp_timer_get_time();
    if (!force && e->src == HA_SRC_SENSOR &&
        s_last_pub_us[idx] != 0 &&
        now_us - s_last_pub_us[idx] < (int64_t)SENSOR_MIN_GAP_MS * 1000) {
        return false;
    }

    float v;
    uint8_t q;
    if (!entity_value(e, &v, &q)) return false;     /* 影子无该设备 → 不发布 */
    if (q != PROTO_QUAL_VALID) return false;        /* 数据不可信 → 停发，由 expire_after 兜底 */
    if (!force && s_last_valid[idx] && fabsf(v - s_last_v[idx]) < STATE_DEADBAND) return false;

    char topic[160];
    topic_state(topic, sizeof(topic), e->obj_id);

    bool ok;
    if (e->kind == HA_KIND_SWITCH) {
        ok = mqtt_service_publish(topic, v > 0.5f ? "ON" : "OFF", 1, true);
    } else {
        char pl[48];
        snprintf(pl, sizeof(pl), "{\"v\":%.3f}", (double)v);
        ok = mqtt_service_publish(topic, pl, 1, true);
    }

    if (ok) {
        s_last_v[idx] = v;
        s_last_valid[idx] = true;
        s_last_pub_us[idx] = now_us;
        ESP_LOGD(TAG, "state %s = %.3f (force=%d)", e->obj_id, (double)v, (int)force);

        /* 云指令打点收尾：该设备的状态已实际上报 */
        if (s_pending && e->dev_id == s_pending_dev) {
            s_pending = false;
            ESP_LOGI("MQTT_LAT", "[LAT] cloud cmd -> state pub = %lld ms",
                     (long long)((esp_timer_get_time() - s_pending_us) / 1000));
        }
    }
    return ok;
}

static void publish_all_state(bool force)
{
    int n = ha_entities_count();
    if (n > MAX_ENTITIES) n = MAX_ENTITIES;
    for (int i = 0; i < n; i++) {
        publish_state_one(i, force);
        if (force) vTaskDelay(pdMS_TO_TICKS(PUB_BURST_GAP_MS));
    }
}

/* 只轮询执行器：回读变化在 PUB_TICK_MS 内上报，云指令端到端不再等 500ms 节拍 */
static void publish_actuators(void)
{
    int n = ha_entities_count();
    if (n > MAX_ENTITIES) n = MAX_ENTITIES;
    for (int i = 0; i < n; i++) {
        const ha_entity_def_t *e = ha_entities_get(i);
        if (e && e->src == HA_SRC_ACTUATOR) publish_state_one(i, false);
    }
}

static void publish_availability(void)
{
    char t[96];
    topic_availability(t, sizeof(t));
    mqtt_service_publish(t, "online", 1, true);
}

/* ── 云命令处理（本地优先仲裁在协议层完成）── */
static void handle_cmd(const char *obj_id, const char *payload, int len)
{
    const ha_entity_def_t *e = ha_entities_by_obj_id(obj_id);
    if (!e || e->kind != HA_KIND_SWITCH) {
        ESP_LOGW(TAG, "[CMD] ignore unknown entity '%s'", obj_id);
        return;
    }
#if !CONFIG_SMART_HOME_MQTT_ALLOW_CLOUD_CMD
    ESP_LOGW(TAG, "[CMD] cloud control disabled by Kconfig — '%s' ignored", obj_id);
    (void)payload; (void)len;
#else
    char up[8];
    int n = len < (int)sizeof(up) - 1 ? len : (int)sizeof(up) - 1;
    for (int i = 0; i < n; i++) {
        char c = payload[i];
        up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    up[n] = '\0';

    uint8_t action;
    if (strcmp(up, "ON") == 0 || strcmp(up, "1") == 0) {
        action = PROTO_ACTION_ON;
    } else if (strcmp(up, "OFF") == 0 || strcmp(up, "0") == 0) {
        action = PROTO_ACTION_OFF;
    } else {
        ESP_LOGW(TAG, "[CMD] bad payload '%s'", up);
        return;
    }

    ESP_LOGI(TAG, "[CMD] cloud -> %s %s", e->obj_id, action == PROTO_ACTION_ON ? "ON" : "OFF");
    s_pending_us = esp_timer_get_time();
    s_pending_dev = e->dev_id;
    s_pending = true;

    /* 不在 MQTT 事件任务里直接下发：ProtoTx 单发送槽无锁，写操作必须集中在
     * 主任务上下文（见 mqtt_service_take_cloud_cmd 注释）。此处只投递。 */
    cloud_cmd_t item = { .dev_id = e->dev_id, .action = action };
    if (!s_cmd_q || xQueueSend(s_cmd_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[CMD] queue full — '%s' dropped", e->obj_id);
    }
#endif
}

bool mqtt_service_take_cloud_cmd(uint8_t *dev_id, uint8_t *action)
{
    if (!s_cmd_q || !dev_id || !action) return false;
    cloud_cmd_t item;
    if (xQueueReceive(s_cmd_q, &item, 0) != pdTRUE) return false;
    *dev_id = item.dev_id;
    *action = item.action;
    return true;
}

static void mqtt_on_data(esp_mqtt_event_handle_t ev)
{
    char topic[192];
    int tl = ev->topic_len < (int)sizeof(topic) - 1 ? ev->topic_len : (int)sizeof(topic) - 1;
    memcpy(topic, ev->topic, tl);
    topic[tl] = '\0';

    if (strcmp(topic, TOPIC_HA_STATUS) == 0) {
        if (ev->data_len == 6 && memcmp(ev->data, "online", 6) == 0) {
            ESP_LOGI(TAG, "HA restarted (status=online) → re-publish discovery");
            s_discovery_needed = true;
            s_want_republish = true;
        }
        return;
    }

    char prefix[96];
    topic_cmd_prefix(prefix, sizeof(prefix));
    const size_t pl = strlen(prefix);
    if (strncmp(topic, prefix, pl) == 0) {
        handle_cmd(topic + pl, ev->data, ev->data_len);
        return;
    }

    /* 远端节点（阶段 6 的 C3）的 availability / state，交给 remote_node 维护 */
    remote_node_on_message(topic, ev->data, ev->data_len);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected");
        esp_mqtt_client_subscribe(s_client, s_cmd_wildcard, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_HA_STATUS, 1);
        remote_node_subscribe(s_client);   /* smart_home/<node>/availability + state/+ */
        s_want_republish = true;   /* 连接建立 → 重发 discovery + 全量状态 */
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "broker disconnected (auto-reconnect…)");
        break;
    case MQTT_EVENT_DATA:
        mqtt_on_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT transport error");
        break;
    default:
        break;
    }
}

static void mqtt_pub_task(void *arg)
{
    (void)arg;
    int elapsed = 0;       /* 距上次全量增量 */
    int since_force = 0;   /* 距上次强制全量 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PUB_TICK_MS));
        if (!s_connected) {
            elapsed = 0;
            since_force = 0;
            continue;
        }
        if (s_want_republish) {
            s_want_republish = false;
            publish_availability();
            if (s_discovery_needed) {
                s_discovery_needed = false;
                int n = ha_discovery_publish_all();
                ESP_LOGI(TAG, "discovery published (%d entities)", n);
            }
            publish_all_state(true);
            elapsed = 0;
            since_force = 0;
            continue;
        }

        /* 执行器回读：25ms 粒度，云指令的端到端不再被 500ms 节拍拖长 */
        publish_actuators();

        /* 指令被仲裁丢弃 / 状态未变 → 打点不成立，2s 后丢弃 */
        if (s_pending && esp_timer_get_time() - s_pending_us > 2000000) {
            s_pending = false;
            ESP_LOGI("MQTT_LAT", "[LAT] cloud cmd produced no state change");
        }

        elapsed += PUB_TICK_MS;
        since_force += PUB_TICK_MS;
        if (elapsed >= PUB_PERIOD_MS) {
            elapsed = 0;
            if (since_force >= KEEPFRESH_MS) {
                since_force = 0;
                publish_all_state(true);
            } else {
                publish_all_state(false);
            }
        }
    }
}

bool mqtt_service_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) return false;
    int id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (id < 0) {
        ESP_LOGW(TAG, "publish failed: %s", topic);
        return false;
    }
    return true;
}

bool mqtt_service_connected(void)
{
    return s_connected;
}

static void mqtt_wait_ip_task(void *arg);

esp_err_t mqtt_service_start(void)
{
    char uri[128] = {0};
    char user[64] = {0};
    char pass[96] = {0};
    char node[33] = {0};
    size_t n;

    n = sizeof(uri);
    if (storage_get_str(NVS_KEY_URI, uri, &n) != ESP_OK) uri[0] = '\0';
    if (uri[0] == '\0') strlcpy(uri, CONFIG_SMART_HOME_MQTT_BROKER_URI, sizeof(uri));
    if (uri[0] == '\0') {
        ESP_LOGI(TAG, "no broker configured (NVS '%s' empty) — MQTT disabled", NVS_KEY_URI);
        return ESP_ERR_NOT_FOUND;
    }

    n = sizeof(user);
    if (storage_get_str(NVS_KEY_USER, user, &n) != ESP_OK) user[0] = '\0';
    if (user[0] == '\0') strlcpy(user, CONFIG_SMART_HOME_MQTT_USERNAME, sizeof(user));

    n = sizeof(pass);
    if (storage_get_str(NVS_KEY_PASS, pass, &n) != ESP_OK) pass[0] = '\0';
    if (pass[0] == '\0') strlcpy(pass, CONFIG_SMART_HOME_MQTT_PASSWORD, sizeof(pass));

    n = sizeof(node);
    if (storage_get_str(NVS_KEY_NODE, node, &n) != ESP_OK || node[0] == '\0') {
        strlcpy(node, CONFIG_SMART_HOME_MQTT_NODE_ID, sizeof(node));
    }
    topic_set_node_id(node);

    topic_availability(s_will_topic, sizeof(s_will_topic));
    topic_cmd_wildcard(s_cmd_wildcard, sizeof(s_cmd_wildcard));

    s_cmd_q = xQueueCreate(CLOUD_CMD_Q_LEN, sizeof(cloud_cmd_t));
    if (!s_cmd_q) {
        ESP_LOGE(TAG, "cloud cmd queue create failed");
        return ESP_ERR_NO_MEM;
    }

    /* client_id 用 MAC 后 3 字节保证唯一（重复 client_id 会互相踢线） */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    char cid[52];
    snprintf(cid, sizeof(cid), "p4hub-%s-%02x%02x%02x", node, mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    cfg.credentials.client_id = cid;
    if (user[0]) cfg.credentials.username = user;
    if (pass[0]) cfg.credentials.authentication.password = pass;

    /* mqtts:// 时用 crt bundle 校验服务器证书（明文 mqtt:// 不受影响）。
     * bundle 除默认根证书外，还含项目 certs/ca.pem 里的本地自签 CA
     * （CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH="certs"，见 tools/gen_mqtt_certs.py） */
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

    cfg.session.keepalive = CONFIG_SMART_HOME_MQTT_KEEPALIVE_S;
    cfg.session.disable_clean_session = false;
    cfg.session.last_will.topic = s_will_topic;
    cfg.session.last_will.msg = s_will_msg;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;

    cfg.network.reconnect_timeout_ms = 2000;
    /* 网络读超时必须大于 keepalive，否则空闲连接会被客户端按读超时主动断开
     * （默认 10s：keepalive 调到 60s 后会周期性误判断线）。 */
    cfg.network.timeout_ms = (CONFIG_SMART_HOME_MQTT_KEEPALIVE_S + 30) * 1000;
    cfg.buffer.size = 2048;      /* discovery JSON ~600B，默认 1024 偏小 */
    cfg.buffer.out_size = 2048;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register event failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    /* 等拿到 IP 再启动客户端（见 mqtt_wait_ip_task），避免首连必然失败 */
    if (xTaskCreate(mqtt_wait_ip_task, "mqtt_ipwait", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "ip-wait task create failed — starting immediately");
        if (esp_mqtt_client_start(s_client) == ESP_OK) {
            xTaskCreate(mqtt_pub_task, "mqtt_pub", 4096, NULL, 4, &s_task);
        }
    }

    ESP_LOGI(TAG, "MQTT service ready (uri=%s, node=%s), waiting for IP", uri, node);
    return ESP_OK;
}

/* 等 Wi-Fi 真正拿到 IP 再启动 MQTT 客户端。
 * 启动时序里 wifi_init() 之后立刻调 mqtt_service_start()，此时 DHCP 往往还没
 * 完成（实测 4.3s 连、5.2s 才 Got IP），首连必然 "Host is unreachable"，
 * esp-mqtt 退避后要 ~20s 才重试，正好和壁纸启动/OTA 检查撞在一起。
 * 最多等 MQTT_IP_WAIT_MS；等不到也照常启动（离线场景由客户端自己重连）。 */
#define MQTT_IP_WAIT_MS 30000

static void mqtt_wait_ip_task(void *arg)
{
    (void)arg;
    int waited = 0;
    while (waited < MQTT_IP_WAIT_MS) {
        char ip[16] = {0};
        /* wifi_get_ip_info 在 DHCP 未完成时会返回 true 且内容为 "0.0.0.0"，
         * 只判 ip[0] 会过早放行，必须显式排除 0.0.0.0 */
        if (wifi_get_ip_info(ip, NULL, NULL, sizeof(ip)) && ip[0] &&
            strcmp(ip, "0.0.0.0") != 0) {
            ESP_LOGI(TAG, "IP ready (%s) — starting MQTT client", ip);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (waited >= MQTT_IP_WAIT_MS) {
        ESP_LOGW(TAG, "no IP after %d ms — starting MQTT anyway", MQTT_IP_WAIT_MS);
    }

    if (esp_mqtt_client_start(s_client) == ESP_OK) {
        xTaskCreate(mqtt_pub_task, "mqtt_pub", 4096, NULL, 4, &s_task);
    } else {
        ESP_LOGE(TAG, "client start failed");
    }
    vTaskDelete(NULL);
}

void mqtt_service_stop(void)
{
    if (!s_client) return;
    if (s_connected) {
        char t[96];
        topic_availability(t, sizeof(t));
        esp_mqtt_client_publish(s_client, t, "offline", 0, 1, 1);
    }
    esp_mqtt_client_stop(s_client);
    s_connected = false;
    ESP_LOGI(TAG, "MQTT service stopped");
}
