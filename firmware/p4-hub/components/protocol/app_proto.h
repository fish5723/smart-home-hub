/**
 * @file app_proto.h
 * @brief 协议调度层 — P4 侧（HELLO / 设备影子 / 命令下发 / 链路状态机）
 *
 * 阶段 2B: P4 侧独立实现。
 * 与 F103 侧 app_proto.h 对应，但 P4 是主控端（发起命令、收传感器数据）。
 */
#ifndef APP_PROTO_H
#define APP_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  链路状态
 *  ════════════════════════════════════════════════════ */

typedef enum {
    LINK_UP       = 0,
    LINK_DEGRADED = 1,
    LINK_DOWN     = 2,
} link_state_t;

/* ════════════════════════════════════════════════════
 *  设备影子 — 传感器条目
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  dev_id;        /* PROTO_DEV_xxx */
    uint8_t  quality;       /* PROTO_QUAL_xxx */
    float    value;
    int64_t  last_update_us; /* 最后更新时间戳 (esp_timer) */
} sensor_shadow_entry_t;

#define SHADOW_MAX_SENSORS  16

/* ════════════════════════════════════════════════════
 *  设备影子 — 执行器条目
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  dev_id;
    uint8_t  state;          /* 0=OFF, 1=ON */
    int64_t  last_update_us;
} actuator_shadow_entry_t;

typedef struct {
    uint8_t dev_id;
    uint8_t result;
    uint8_t state;
    int64_t last_update_us;
} actuator_feedback_t;

/* ════════════════════════════════════════════════════
 *  告警影子 — ALARM_EVENT (0x12)
 *  F103 的本地安全动作（蜂鸣/关断）已自行完成，本结构仅用于面板显示。
 *  active 由"数据恢复"自动清除（对应设备的新鲜有效数据低于阈值）。
 * ════════════════════════════════════════════════════ */

typedef struct {
    uint8_t alarm_id;        /* PROTO_ALARM_xxx */
    uint8_t level;           /* PROTO_LEVEL_xxx */
    float   value;           /* 触发值 */
    float   threshold;       /* 阈值 */
    bool    active;          /* true = 仍在告警 */
    int64_t last_update_us;
} alarm_shadow_t;

/* ════════════════════════════════════════════════════
 *  链路统计（只读）
 *  ════════════════════════════════════════════════════ */

typedef struct {
    link_state_t state;
    uint32_t     up_ms;
    uint32_t     degraded_ms;
    uint32_t     down_ms;
    uint32_t     state_changes;
    uint32_t     missed_heartbeats;
} link_stats_t;

/* ════════════════════════════════════════════════════
 *  HELLO_RSP 对端信息快照（供设备属性上报）
 * ════════════════════════════════════════════════════ */

typedef struct {
    bool     valid;          /* true = 已收到过 HELLO_RSP */
    uint8_t  proto_ver;      /* 对端协议版本 */
    uint16_t caps;           /* 对端能力位图 */
    char     name[17];       /* 对端设备名（NUL 结尾） */
} app_proto_peer_info_t;

/** 拷贝最近一次 HELLO_RSP 的对端信息；无 HELLO 时 valid=false。 */
bool AppProto_GetPeerInfo(app_proto_peer_info_t *out);

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

void          AppProto_Init(void);
void          AppProto_Poll(void);
link_state_t  AppProto_GetLinkState(void);
void          AppProto_GetLinkStats(link_stats_t *out);

/* ── 设备影子 ── */

/** 获取传感器影子数据（返回条目数） */
int  AppProto_GetSensorShadow(sensor_shadow_entry_t *out, int max_entries);

/** 获取执行器影子数据（返回条目数） */
int  AppProto_GetActuatorShadow(actuator_shadow_entry_t *out, int max_entries);
bool AppProto_GetLastActuatorFeedback(actuator_feedback_t *out);

/** 获取当前告警：返回 true 并填充 out 表示仍在告警状态 */
bool AppProto_GetActiveAlarm(alarm_shadow_t *out);

/** 无条件拷贝最近一次告警影子（含已解除的历史事件，active=false）。
 *  用于面板展示"已恢复"状态；若从未收到过 ALARM_EVENT，则 alarm_id=0。 */
void AppProto_GetAlarmShadow(alarm_shadow_t *out);

/** 获取指定设备的传感器值，未找到返回 NaN */
float AppProto_GetSensorValue(uint8_t dev_id);

/** 写入 P4 本地伪传感器（如人脸确认结果）；不发给 F103，只用于本地规则判定。
 *  quality 固定为 VALID，时间戳随之刷新。 */
void AppProto_SetLocalSensorValue(uint8_t dev_id, float value);

/* ── 命令下发 ── */

/** 命令来源（本地优先仲裁，ADR-004） */
typedef enum {
    PROTO_CMD_SRC_LOCAL = 0,   /**< UI 点击 / 规则引擎等本地来源 */
    PROTO_CMD_SRC_CLOUD = 1,   /**< MQTT 云端下发 */
} proto_cmd_src_t;

/** 云端指令在「本地操作后」该窗口内不生效（毫秒，ADR-004 规定 1s） */
#define PROTO_CLOUD_ARB_WINDOW_MS 1000

/** 发送执行器指令到 F103（带 ACK 确认）——本地来源（薄包装，语义同旧接口） */
void AppProto_SendActuatorCmd(uint8_t dev_id, uint8_t action, uint16_t ttl_ms);

/** 同上，但显式标记来源：CLOUD 源在本地操作 1s 窗口内被丢弃 */
void AppProto_SendActuatorCmdEx(uint8_t dev_id, uint8_t action, uint16_t ttl_ms,
                                proto_cmd_src_t src);

/** 查询某执行器最近一次本地操作时间戳（毫秒；0 = 从未） */
int64_t AppProto_GetLastLocalCmdMs(uint8_t dev_id);

/** 标记一次 UI 点击起点，用于测量"点击 → 执行器吸合"端到端延迟（<200ms 验收）。
 *  需在调用 AppProto_SendActuatorCmd 之前、紧贴点击事件处调用。 */
void AppProto_MarkUiClick(void);

/** 查询执行器状态 */
void AppProto_QueryActuatorStatus(void);

/** HELLO 协商 */
void AppProto_RequestHello(void);

/* ── 阶段 8: F103 IAP ── */

/** OTA_RESULT 回调（由 main 注册 f103_iap_on_result，避免 protocol 组件
 *  反向依赖 main）。回调在协议 RX 任务上下文执行，必须非阻塞。 */
typedef void (*app_proto_ota_result_cb_t)(const proto_frame_t *frame);

void AppProto_SetOtaResultHandler(app_proto_ota_result_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* APP_PROTO_H */
