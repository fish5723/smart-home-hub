/**
 * @file crc16.h
 * @brief CRC-16/CCITT-FALSE 查表实现（纯逻辑，不依赖 HAL / 可 PC 单测）
 *
 * 多项式: 0x1021
 * 初值:   0xFFFF
 * 输入反转: 否
 * 输出异或: 0x0000
 * 检查值: CRC("123456789") = 0x29B1
 *
 * 来源: docs/03-通信协议规范.md §2.3
 */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void     crc16_init(void);
uint16_t crc16_compute(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC16_H */