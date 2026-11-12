/**
 * @file proto_frame.h
 * @brief 协议帧纯逻辑层 — 字节流收包状态机 + 发包组装（不依赖 HAL / 可 PC 单测）
 *
 * 从 proto_uart.c 抽出的解析状态机（docs/12 §5 预警的分层要求）：
 *   - 输入是抽象字节流，不关心字节来自 DMA 环形缓冲还是 PC 测试文件
 *   - 无 printf、无 HAL 调用，firmware 与 tests/ 共用同一份代码
 *
 * 状态机见 docs/03-通信协议规范.md §7：
 *   IDLE → SOF2 → HEADER(6B) → PAYLOAD(LEN) → CRC(2B) → DISPATCH
 *   LEN 非法 / CRC 失败 → 统计 + 回 IDLE 重同步（IDLE 天然跳过非 0xAA 字节）
 *   帧内字节间隔超时由传输层驱动 proto_parser_abort()（规范 §7.4）
 */
#ifndef PROTO_FRAME_H
#define PROTO_FRAME_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  收包状态机
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint32_t rx_frames;     /* CRC 通过并投递的帧数 */
    uint32_t len_errors;    /* LEN 超限 / 版本不符 */
    uint32_t crc_errors;    /* CRC 校验失败 */
    uint32_t resyncs;       /* 重同步次数（含帧内超时） */
} proto_parser_stats_t;

typedef void (*proto_frame_cb_t)(const proto_frame_t *frame, void *user);

typedef struct {
    /* 状态机内部状态 */
    uint8_t   state;
    uint8_t   header[6];        /* VER+TYPE+SEQ(2)+LEN(2) */
    uint8_t   header_idx;
    uint16_t  payload_len;
    uint16_t  payload_idx;
    uint8_t   payload[PROTO_MAX_PAYLOAD];
    uint8_t   crc_buf[2];
    uint8_t   crc_idx;
    /* 投递与统计 */
    proto_frame_cb_t      on_frame;
    void                 *user;
    proto_parser_stats_t  stats;
} proto_parser_t;

void proto_parser_init(proto_parser_t *p, proto_frame_cb_t cb, void *user);

/** 逐字节喂入（传输层从环形缓冲取字节后调用） */
void proto_parser_feed(proto_parser_t *p, uint8_t b);

/** 批量喂入 */
void proto_parser_feed_buf(proto_parser_t *p, const uint8_t *buf, uint16_t len);

/** 帧内超时 / 外部强制：状态机不在 IDLE 时安全丢弃半帧并计一次重同步 */
void proto_parser_abort(proto_parser_t *p);

/** 状态机是否正在收半帧（传输层据此判定帧内超时） */
uint8_t proto_parser_in_frame(const proto_parser_t *p);

void proto_parser_reset_stats(proto_parser_t *p);

/* ════════════════════════════════════════════════════
 *  发包组装（纯逻辑，发送方式由传输层决定）
 *  ════════════════════════════════════════════════════ */

/**
 * 组装完整帧到 out（容量须 >= PROTO_MAX_FRAME）。
 * @return 帧总字节数；参数非法返回 0
 */
uint16_t proto_frame_build(uint8_t type, uint16_t seq,
                           const uint8_t *payload, uint16_t len,
                           uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_FRAME_H */
