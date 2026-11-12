/**
 * @file frame.h
 * @brief 阶段二帧格式（docs/03 §2）在 C3 上的复用实现
 *
 *  偏移 0   SOF     2B  0xAA 0x55
 *  偏移 2   VER     1B  0x02
 *  偏移 3   TYPE    1B  见 docs/03 §3
 *  偏移 4   SEQ     2B  小端
 *  偏移 6   LEN     2B  小端
 *  偏移 8   PAYLOAD LEN
 *  末尾     CRC16   2B  覆盖 VER..PAYLOAD
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FRAME_SOF0          0xAA
#define FRAME_SOF1          0x55
#define FRAME_VER           0x02
#define FRAME_HEADER_SIZE   8
#define FRAME_CRC_SIZE      2
#define FRAME_MAX_PAYLOAD   256
#define FRAME_CRC_CHECK     0x29B1  /* CRC16("123456789") 的检查值 */

/* CRC-16/CCITT-FALSE：多项式 0x1021，初值 0xFFFF，无反转、无异或输出 */
uint16_t frame_crc16(const uint8_t *data, uint16_t len);

/**
 * 组帧。成功返回帧总长度（8 + len + 2），失败返回 0。
 * out 需至少 8 + len + 2 字节。
 */
uint16_t frame_build(uint8_t *out, size_t cap, uint8_t type, uint16_t seq,
                     const uint8_t *payload, uint16_t len);

/** 用文档给定的检查值自检 CRC 实现，返回 1 通过 / 0 失败 */
int frame_selftest(void);
