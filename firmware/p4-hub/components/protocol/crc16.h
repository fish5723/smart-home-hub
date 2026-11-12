/**
 * @file crc16.h
 * @brief CRC16-CCITT (多项式 0x1021, 初始值 0xFFFF)
 *
 * 与 F103 侧 crc16.c 使用相同算法和查表，独立实现。
 */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t crc16_compute(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC16_H */