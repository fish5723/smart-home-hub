/**
 * @file proto_uart.h
 * @brief 协议 UART 传输层 — DMA + IDLE + 环形缓冲（USART1）
 *
 * 职责:
 *   - ISR 只记录 DMA 位置，不解析帧
 *   - 收包状态机在 proto_uart_poll() 中运行（由调度器以高优先级轮询）
 *   - 发送按需走 DMA，失败回退阻塞
 *   - 统计计数（rx_frames / crc_errors / drops / resyncs）
 *
 * 设计约束:
 *   - 收发不依赖主循环时序（ACK 不排在 OLED 刷新之后）
 *   - USART1 只跑二进制帧，绝不允许 printf 混入
 */
#ifndef PROTO_UART_H
#define PROTO_UART_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  初始化
 *  ════════════════════════════════════════════════════ */

void ProtoUart_Init(void);

/**
 * @brief USART1 IDLE 中断入口（由 stm32f1xx_it.c 的 USART1_IRQHandler 调用）
 *
 * 仅执行 F1 的 IDLE 标志清除序列（读 SR + 读 DR）。数据的可用性
 * 不依赖本中断——Poll 每轮用 CNDTR 模差直接对比写指针与消费指针。
 * 绝不在此解析帧（ISR 职责约定见 proto_uart.c 头部）。
 */
void ProtoUart_NotifyIdle(void);

/* ════════════════════════════════════════════════════
 *  收包（高优先级轮询，每主循环调用一次）
 *  ════════════════════════════════════════════════════ */

typedef void (*proto_rx_callback_t)(const proto_frame_t *frame);

void ProtoUart_SetRxCallback(proto_rx_callback_t cb);
void ProtoUart_Poll(void);

/* ════════════════════════════════════════════════════
 *  发包
 *  ════════════════════════════════════════════════════ */

int  ProtoUart_SendFrame(uint8_t type, uint16_t seq,
                         const uint8_t *payload, uint16_t len);
int  ProtoUart_SendRaw(const uint8_t *buf, uint16_t len);

/* ════════════════════════════════════════════════════
 *  统计（可经 STATS_GET 上报）
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint32_t rx_frames;       /* 成功收帧数 */
    uint32_t crc_errors;      /* CRC 校验失败 */
    uint32_t len_errors;      /* 长度非法 */
    uint32_t unknown_types;   /* 未知类型 */
    uint32_t drops;           /* 缓冲满丢弃 */
    uint32_t resyncs;         /* 重同步次数 */
    uint32_t tx_bytes;        /* 发送字节数 */
} proto_stats_t;

void                ProtoUart_GetStats(proto_stats_t *out);
void                ProtoUart_ResetStats(void);
uint32_t            ProtoUart_GetRxDropped(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_UART_H */