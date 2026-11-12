/**
 * @file crc32.c
 * @brief CRC-32/ISO-HDLC 位运算实现（无查表，省 Flash，适合 bootloader）
 */
#include "crc32.h"

uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc ^= 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)buf[i];
        for (uint8_t bit = 0; bit < 8U; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }

    return crc ^ 0xFFFFFFFFUL;
}

uint32_t crc32_compute(const uint8_t *buf, uint32_t len)
{
    return crc32_update(0U, buf, len);
}
