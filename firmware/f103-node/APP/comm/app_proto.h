/**
 * @file app_proto.h
 * @brief 协议调度层 — 收包分发 / ACK判定 / 超时轮询 / 链路状态机
 *
 * 位置: ProtoUart（传输）← app_proto（调度）← 各业务模块
 *
 * 本层职责:
 *   1. 注册 ProtoUart RX callback，接收帧
 *   2. 去重 → ACK/NACK 判定 → 业务帧分发
 *   3. 轮询 ProtoTx 的超时/重传
 *   4. 链路状态机: UP → 3s无帧→DEGRADED → 6s→DOWN
 *   5. [阶段2B] HELLO协商 / ACTUATOR执行 / TTL校验 / 传感器上报
 */
#ifndef APP_PROTO_H
#define APP_PROTO_H

#include <stdint.h>
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
 *  链路统计（只读）
 *  ════════════════════════════════════════════════════ */

typedef struct {
    link_state_t state;
    uint32_t     up_ms;          /* LINK_UP 累计时长 */
    uint32_t     degraded_ms;    /* LINK_DEGRADED 累计时长 */
    uint32_t     down_ms;        /* LINK_DOWN 累计时长 */
    uint32_t     state_changes;  /* 状态切换次数 */
    uint32_t     last_rx_ms;     /* 上次收到帧的时间戳 */
    uint32_t     missed_heartbeats;
} link_stats_t;

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

void          AppProto_Init(void);
void          AppProto_Poll(void);
link_state_t  AppProto_GetLinkState(void);
void          AppProto_GetLinkStats(link_stats_t *out);

/* ── 阶段 2B: 业务层 ── */

/** 传感器数据上报任务（由调度器定期调用） */
void AppProto_SendReportTask(void);

/** 上报一次告警事件（ALARM_EVENT 0x12，需要 ACK）。
 *  载荷: alarm_id(1B) + level(1B) + value(f32) + threshold(f32) = 10B。
 *  语义: 仅"通知面板显示"；安全动作（蜂鸣/关断）仍全部本地完成（ADR-009）。
 *  @return 1=已发出; 0=TX 槽被占（已挂起，AppProto_Poll 会自动重试） */
int AppProto_NotifyAlarm(uint8_t alarm_id, uint8_t level,
                         float value, float threshold);

/** 获取执行器当前状态，写入 payload 缓冲区，返回长度 */
uint16_t AppProto_GetActuatorStatus(uint8_t *payload, uint16_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* APP_PROTO_H */