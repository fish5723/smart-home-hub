/**
 * @file ha_entities.h
 * @brief HA 实体映射表（阶段 5）
 *
 * 「加设备只改表」：新增一个对外实体只需在此表追加一行，
 * 不改 mqtt_service / ha_discovery 逻辑（延续 ADR-005 解耦思路）。
 *
 * 表的 dev_id 复用 protocol.h 的 PROTO_DEV_xxx / PROTO_ALARM_xxx，
 * 不另造设备模型。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HA 组件类型（决定 discovery 主题段与 JSON 键） */
typedef enum {
    HA_KIND_SWITCH = 0,   /**< 可读写（命令下发） */
    HA_KIND_SENSOR,       /**< 只读数值 */
    HA_KIND_BINARY,       /**< 只读布尔（ON/OFF） */
} ha_entity_kind_t;

/** 值来源 */
typedef enum {
    HA_SRC_ACTUATOR = 0,  /**< 执行器影子（AppProto_GetActuatorShadow） */
    HA_SRC_SENSOR,        /**< 传感器影子（AppProto_GetSensorShadow） */
    HA_SRC_ALARM,         /**< 告警影子（AppProto_GetAlarmShadow） */
} ha_value_src_t;

typedef struct {
    ha_entity_kind_t kind;
    const char      *component;    /**< "switch" / "sensor" / "binary_sensor" */
    ha_value_src_t   src;
    uint8_t          dev_id;       /**< PROTO_DEV_xxx 或 PROTO_ALARM_xxx */
    const char      *obj_id;       /**< HA object_id（主题内唯一） */
    const char      *name;         /**< 实体显示名 */
    const char      *device_class; /**< 可 NULL */
    const char      *unit;         /**< 可 NULL（unit_of_measurement） */
    const char      *state_class;  /**< 可 NULL（state_class） */
    const char      *icon;         /**< 可 NULL */
    bool             enabled;
} ha_entity_def_t;

/** 实体总数（含 enabled=false 条目） */
int ha_entities_count(void);
/** 按索引取实体；越界返回 NULL */
const ha_entity_def_t *ha_entities_get(int index);
/** 按 obj_id 查实体；未找到返回 NULL */
const ha_entity_def_t *ha_entities_by_obj_id(const char *obj_id);

#ifdef __cplusplus
}
#endif
