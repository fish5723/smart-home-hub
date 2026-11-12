/**
 * @file app_proto.h — PC 桩: 与固件 components/protocol/app_proto.h 同接口
 * 假数据源(演示模式): 温度 28.5 / 湿度 62 / 灯开 / 锁上 / 泵关 / 人感刚动。
 * 真机构建不走本文件(include 顺序: stubs 目录只在模拟器 CMake 里)。
 */
#ifndef APP_PROTO_H_STUB
#define APP_PROTO_H_STUB

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"          /* 真件: 常量/设备 ID 单一来源 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_UP       = 0,
    LINK_DEGRADED = 1,
    LINK_DOWN     = 2,
} link_state_t;

typedef struct {
    uint8_t  dev_id;
    uint8_t  quality;
    float    value;
    int64_t  last_update_us;
} sensor_shadow_entry_t;

#define SHADOW_MAX_SENSORS  16

typedef struct {
    uint8_t  dev_id;
    uint8_t  state;
    int64_t  last_update_us;
} actuator_shadow_entry_t;

typedef struct {
    uint8_t dev_id;
    uint8_t result;
    uint8_t state;
    int64_t last_update_us;
} actuator_feedback_t;

typedef struct {
    uint8_t alarm_id;
    uint8_t level;
    float   value;
    float   threshold;
    bool    active;
    int64_t last_update_us;
} alarm_shadow_t;

typedef struct {
    link_state_t state;
    uint32_t up_ms, degraded_ms, down_ms;
    uint32_t state_changes, missed_heartbeats;
} link_stats_t;

link_state_t AppProto_GetLinkState(void);
int  AppProto_GetSensorShadow(sensor_shadow_entry_t *out, int max_entries);
int  AppProto_GetActuatorShadow(actuator_shadow_entry_t *out, int max_entries);
bool AppProto_GetLastActuatorFeedback(actuator_feedback_t *out);
bool AppProto_GetActiveAlarm(alarm_shadow_t *out);
void AppProto_GetAlarmShadow(alarm_shadow_t *out);
float AppProto_GetSensorValue(uint8_t dev_id);
void AppProto_SendActuatorCmd(uint8_t dev_id, uint8_t action, uint16_t ttl_ms);
void AppProto_MarkUiClick(void);
void AppProto_QueryActuatorStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_PROTO_H_STUB */
