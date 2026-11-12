/**
 * @file ha_entities.c
 * @brief HA 实体映射表实现（见 ha_entities.h）
 *
 * 单位说明：sensor 的 unit 取「F103 上报给 P4 的原始单位」（影子值不做换算），
 * 例如 BMP280 压力上报为 Pa、MQ 系列为 ppm、土壤/雨滴为百分比。
 */
#include "ha_entities.h"
#include "protocol.h"
#include <string.h>

static const ha_entity_def_t k_entities[] = {
    /* ── 执行器 → switch（HA 可远程下发，经仲裁后走 P4→F103）── */
    { HA_KIND_SWITCH, "switch", HA_SRC_ACTUATOR, PROTO_DEV_RELAY1,
      "relay1", "Living Light", NULL, NULL, NULL, "mdi:lightbulb", true },
    { HA_KIND_SWITCH, "switch", HA_SRC_ACTUATOR, PROTO_DEV_RELAY2,
      "relay2", "Floor Lamp", NULL, NULL, NULL, "mdi:floor-lamp", true },
    { HA_KIND_SWITCH, "switch", HA_SRC_ACTUATOR, PROTO_DEV_BEEPER,
      "beeper", "Buzzer", NULL, NULL, NULL, "mdi:bell", true },
    /* PROTO_DEV_LOCK(0x03) 不接入：实装门锁模块与设计不符，2026-10-18 已删除 */

    /* ── 传感器 → sensor（只上报）── */
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_DHT11_TEMP,
      "dht11_temp", "Room Temperature", "temperature", "\u00b0C", "measurement", "mdi:thermometer", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_DHT11_HUM,
      "dht11_hum", "Room Humidity", "humidity", "%", "measurement", "mdi:water-percent", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_BMP280_TEMP,
      "bmp280_temp", "Barometer Temperature", "temperature", "\u00b0C", "measurement", "mdi:thermometer", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_BMP280_PRESS,
      "bmp280_press", "Pressure", "pressure", "Pa", "measurement", "mdi:gauge", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_MQ135,
      "mq135", "Air Quality (MQ-135)", NULL, "ppm", "measurement", "mdi:air-filter", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_MQ4,
      "mq4", "Combustible Gas (MQ-4)", NULL, "ppm", "measurement", "mdi:gas-cylinder", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_SOIL,
      "soil", "Soil Moisture", "moisture", "%", "measurement", "mdi:water", true },
    { HA_KIND_SENSOR, "sensor", HA_SRC_SENSOR, PROTO_DEV_RAIN,
      "rain", "Rain Sensor", "moisture", "%", "measurement", "mdi:weather-rainy", true },

    /* ── 人体存在 → binary_sensor（motion）── */
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_SENSOR, PROTO_DEV_HUMAN,
      "human", "Motion", "motion", NULL, NULL, "mdi:motion-sensor", true },

    /* ── 告警 → binary_sensor（来自 ALARM_EVENT 影子）──
     * PROTO_ALARM_LOCK_FAIL(0x04) 不接入（锁不做） */
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_ALARM, PROTO_ALARM_GAS_MQ4,
      "alarm_gas", "Gas Alarm", "gas", NULL, NULL, "mdi:fire-alert", true },
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_ALARM, PROTO_ALARM_AIR_MQ135,
      "alarm_air", "Air Alarm", "problem", NULL, NULL, "mdi:alert", true },
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_ALARM, PROTO_ALARM_PUMP,
      "alarm_pump", "Pump Alarm", "problem", NULL, NULL, "mdi:water-pump", true },
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_ALARM, PROTO_ALARM_SENSOR,
      "alarm_sensor", "Sensor Alarm", "problem", NULL, NULL, "mdi:alert-circle", true },
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_ALARM, PROTO_ALARM_LINK,
      "alarm_link", "Link Alarm", "problem", NULL, NULL, "mdi:link-off", true },
    { HA_KIND_BINARY, "binary_sensor", HA_SRC_ALARM, PROTO_ALARM_STORAGE,
      "alarm_storage", "Storage Alarm", "problem", NULL, NULL, "mdi:sd-alert", true },
};

#define ENTITY_COUNT ((int)(sizeof(k_entities) / sizeof(k_entities[0])))

int ha_entities_count(void)
{
    return ENTITY_COUNT;
}

const ha_entity_def_t *ha_entities_get(int index)
{
    if (index < 0 || index >= ENTITY_COUNT) return NULL;
    return &k_entities[index];
}

const ha_entity_def_t *ha_entities_by_obj_id(const char *obj_id)
{
    if (!obj_id) return NULL;
    for (int i = 0; i < ENTITY_COUNT; i++) {
        if (strcmp(k_entities[i].obj_id, obj_id) == 0) return &k_entities[i];
    }
    return NULL;
}
