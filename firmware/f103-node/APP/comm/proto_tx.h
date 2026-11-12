/**
 * @file proto_tx.h
 * @brief 协议发送管理 — ACK 超时 / 重传 / 去重表
 *
 * 约束:
 *   - 裸机单线程，不引入 RTOS 互斥量
 *   - 同一时刻最多 1 个待确认帧（简单可靠，符合当前任务模型）
 *   - 需要 ACK 的类型: SENSOR_REPORT / ALARM_EVENT / ACTUATOR_CMD / CONFIG_* / OTA_*
 *   - 不需要 ACK: PING/PONG / ACK 本身 / 未知类型
 *
 * 来源: docs/03-通信协议规范.md §6
 */
#ifndef PROTO_TX_H
#define PROTO_TX_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  初始化
 *  ════════════════════════════════════════════════════ */

void ProtoTx_Init(void);

/* ════════════════════════════════════════════════════
 *  发送（带/不带 ACK）
 *  ════════════════════════════════════════════════════ */

uint16_t ProtoTx_SendWithAck(uint8_t type, const uint8_t *payload, uint16_t len);

int ProtoTx_SendFireAndForget(uint8_t type, const uint8_t *payload, uint16_t len);

/* ════════════════════════════════════════════════════
 *  轮询（检查超时 → 重传 → 超限通知）
 *  ════════════════════════════════════════════════════ */

void ProtoTx_Poll(void);

/* ════════════════════════════════════════════════════
 *  ACK / NACK 处理（由 RX 回调调用）
 *  ════════════════════════════════════════════════════ */

void ProtoTx_OnAck(uint16_t acked_seq);
void ProtoTx_OnNack(uint16_t nacked_seq, uint8_t err_code);

/* ════════════════════════════════════════════════════
 *  去重表（接收侧）
 *  ════════════════════════════════════════════════════ */

int      ProtoTx_IsDuplicate(uint16_t seq);
void     ProtoTx_RecordSeq(uint16_t seq);

/* ════════════════════════════════════════════════════
 *  统计
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint32_t tx_frames;       /* 已发送帧数 */
    uint32_t tx_ack_ok;       /* ACK 成功次数 */
    uint32_t tx_timeouts;     /* 超时次数（含重传） */
    uint32_t tx_retrans;      /* 重传次数 */
    uint32_t tx_dropped;      /* 超过最大重传次数放弃 */
    uint32_t rx_dedup_hits;   /* 去重命中 */
} proto_tx_stats_t;

void ProtoTx_GetStats(proto_tx_stats_t *out);

/* ════════════════════════════════════════════════════
 *  序列号管理
 *  ════════════════════════════════════════════════════ */

uint16_t ProtoTx_NextSeq(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_TX_H */