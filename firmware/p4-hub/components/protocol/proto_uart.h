/**
 * @file proto_uart.h
 * @brief UART 传输层 — 帧封装/解帧（ESP-IDF 驱动）
 *
 * 阶段 2B: P4 侧独立实现，使用 ESP-IDF uart 驱动。
 */
#ifndef PROTO_UART_H
#define PROTO_UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  统计信息
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint32_t rx_frames;         /* 收到的完整帧 */
    uint32_t rx_bytes;          /* 收到的字节数 */
    uint32_t rx_crc_err;        /* CRC 错误 */
    uint32_t rx_too_long;       /* 帧太长 */
    uint32_t rx_ver_mismatch;   /* 版本不匹配 */
    uint32_t tx_frames;         /* 发送的帧 */
    uint32_t tx_bytes;          /* 发送的字节数 */
} proto_stats_t;

/* ════════════════════════════════════════════════════
 *  RX 回调类型
 *  ════════════════════════════════════════════════════ */

typedef void (*proto_rx_cb_t)(const proto_frame_t *frame);

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

/** 初始化 UART 传输层 */
void ProtoUart_Init(void);

/** 注册 RX 回调（收到完整帧时调用，来自 RX 任务上下文） */
void ProtoUart_RegisterRxCb(proto_rx_cb_t cb);

/** 发送一帧（线程安全） */
void ProtoUart_SendFrame(uint8_t type, uint16_t seq,
                         const uint8_t *payload, uint16_t len);

/** 获取统计信息 */
void ProtoUart_GetStats(proto_stats_t *out);

/** 轮询收包（由 RX 任务调用，也可在主任务中调用以快速处理） */
void ProtoUart_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_UART_H */