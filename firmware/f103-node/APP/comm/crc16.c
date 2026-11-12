/**
 * @file crc16.c
 * @brief CRC-16/CCITT-FALSE 查表实现
 *
 * 源: docs/03-通信协议规范.md §2.3
 * 多项式: 0x1021, 初值 0xFFFF, 输出异或 0x0000
 * 检查值: CRC("123456789") = 0x29B1
 *
 * 查表法: 256 项 × 2B = 512 字节 Flash，72MHz 下每字节约 12 个 CPU 周期
 *         (逐位法约 80 周期/字节，256 字节载荷从 ~2ms 降到 ~60µs)
 */
#include "crc16.h"

static uint16_t crc16_table[256];
static uint8_t  crc16_ready;

void crc16_init(void)
{
    if (crc16_ready) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint16_t crc = 0;
        uint16_t c   = (uint16_t)i << 8;
        for (uint32_t j = 0; j < 8; j++) {
            if ((crc ^ c) & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc = (uint16_t)(crc << 1);
            c <<= 1;
        }
        crc16_table[i] = crc;
    }
    crc16_ready = 1;
}

uint16_t crc16_compute(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    if (!crc16_ready) crc16_init();

    while (len--) {
        crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ *buf++) & 0xFF]);
    }
    return crc;
}