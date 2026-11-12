/**
 * @file proto_tx.h
 * @brief 发送管理层 — ACK 等待 / 超时重传 / 去重判定
 *
 * 与 F103 侧 proto_tx.h 接口兼容，独立实现。
 */
#ifndef PROTO_TX_H
#define PROTO_TX_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

/** 分配下一个发送序号 */
uint16_t ProtoTx_NextSeq(void);

/** 发送需要 ACK 的帧（内部记录 pending） */
void ProtoTx_SendWithAck(uint8_t type, const uint8_t *payload, uint16_t len);

/** 发送不需要 ACK 的帧 */
void ProtoTx_SendNoAck(uint8_t type, const uint8_t *payload, uint16_t len);

/** 发送槽是否占用（有待确认帧）。占用时 SendWithAck 会丢弃新帧。 */
bool ProtoTx_IsBusy(void);

/** 收到 ACK */
void ProtoTx_OnAck(uint16_t seq);

/** 收到 NACK */
void ProtoTx_OnNack(uint16_t seq, uint8_t error_code);

/** 去重判定 */
bool ProtoTx_IsDuplicate(uint16_t seq);

/** 记录序号（去重） */
void ProtoTx_RecordSeq(uint16_t seq);

/** 轮询超时重传 */
void ProtoTx_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_TX_H */