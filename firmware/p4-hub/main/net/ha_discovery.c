/**
 * @file ha_discovery.c
 * @brief Home Assistant MQTT Discovery 实现（见 ha_discovery.h）
 *
 * Discovery JSON 关键设计：
 *  - availability_topic 指向设备级 <base>/availability（LWT 遗嘱）→ 整机离线判定
 *  - expire_after=60：实体在 60s 内无更新则 HA 自动标 unavailable。发布侧每 20s
 *    强制全量重发一次（见 mqtt_service.c KEEPFRESH_MS），故正常在线不会误判；
 *    而 P4↔F103 链路断开 / 传感器 quality 非 VALID 时**主动停发**，到期后 HA 如实
 *    显示 unavailable —— 不粉饰"设备在线但没数据"。
 */
#include "ha_discovery.h"
#include "topic.h"
#include "ha_entities.h"
#include "mqtt_service.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "cJSON.h"

static const char *TAG = "ha_disco";

/* 每条 discovery 之间的间隔。esp_hosted 的 SDIO TX/RX 共用一个 mempool，
 * 18 条配置在连接瞬间一次性灌入会触发 "mempool OOM (TX)" → 数据包被静默
 * 丢弃 → TCP 写阻塞数十秒，最终 broker 判定超时掉线。20ms 间隔使发送
 * 速率降到 ~50 条/秒，足以让 SDIO 排空。 */
#define DISCOVERY_GAP_MS 20

/** 无更新多久后 HA 标记该实体 unavailable（秒） */
#define HA_EXPIRE_AFTER_S 60

static void add_device_block(cJSON *root)
{
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    if (!dev) return;

    char ids[48];
    snprintf(ids, sizeof(ids), "%s-hub", topic_node_id());
    cJSON_AddStringToObject(dev, "identifiers", ids);
    cJSON_AddStringToObject(dev, "name", "Smart Home Hub");
    cJSON_AddStringToObject(dev, "manufacturer", "DIY");
    cJSON_AddStringToObject(dev, "model", "ESP32-P4 Hub");
    cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
}

static bool publish_one(const ha_entity_def_t *e)
{
    char topic[192];
    topic_discovery(topic, sizeof(topic), e->component, e->obj_id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    char uniq[64];
    snprintf(uniq, sizeof(uniq), "%s_%s", topic_node_id(), e->obj_id);
    cJSON_AddStringToObject(root, "name", e->name);
    cJSON_AddStringToObject(root, "unique_id", uniq);
    cJSON_AddStringToObject(root, "object_id", e->obj_id);
    if (e->icon) cJSON_AddStringToObject(root, "icon", e->icon);
    if (e->device_class) cJSON_AddStringToObject(root, "device_class", e->device_class);

    char avail[96], state[160];
    topic_availability(avail, sizeof(avail));
    topic_state(state, sizeof(state), e->obj_id);
    cJSON_AddStringToObject(root, "availability_topic", avail);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON_AddStringToObject(root, "state_topic", state);
    cJSON_AddNumberToObject(root, "qos", 1);
    cJSON_AddBoolToObject(root, "retain", true);
    cJSON_AddNumberToObject(root, "expire_after", HA_EXPIRE_AFTER_S);

    switch (e->kind) {
    case HA_KIND_SWITCH: {
        char cmd[160];
        topic_cmd(cmd, sizeof(cmd), e->obj_id);
        cJSON_AddStringToObject(root, "command_topic", cmd);
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
        break;
    }
    case HA_KIND_SENSOR:
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.v }}");
        if (e->unit) cJSON_AddStringToObject(root, "unit_of_measurement", e->unit);
        if (e->state_class) cJSON_AddStringToObject(root, "state_class", e->state_class);
        break;
    case HA_KIND_BINARY:
        cJSON_AddStringToObject(root, "value_template",
                               "{{ 'ON' if value_json.v | int(0) > 0 else 'OFF' }}");
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
        break;
    default:
        break;
    }

    add_device_block(root);

    char *payload = cJSON_PrintUnformatted(root);
    bool ok = false;
    if (payload) {
        ok = mqtt_service_publish(topic, payload, 1, true);
        cJSON_free(payload);
    } else {
        ESP_LOGW(TAG, "cJSON print failed for %s", e->obj_id);
    }
    cJSON_Delete(root);
    return ok;
}

int ha_discovery_publish_all(void)
{
    int n = ha_entities_count();
    int published = 0;
    for (int i = 0; i < n; i++) {
        const ha_entity_def_t *e = ha_entities_get(i);
        if (!e || !e->enabled) continue;
        if (publish_one(e)) published++;
        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_GAP_MS));
    }
    return published;
}
