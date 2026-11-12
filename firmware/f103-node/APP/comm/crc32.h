/**
 * @file crc32.h
 * @brief CRC-32/ISO-HDLC（IEEE 802.3，多项式 0xEDB88320，初值 0xFFFFFFFF）
 *
 * 用于 F103 IAP 镜像完整性校验；app 与 bootloader 共用同一实现。
 * 纯逻辑，不依赖 HAL。
 */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 增量更新；首次传 crc=0，后续传上次返回值。 */
uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len);

/** 一次性计算。 */
uint32_t crc32_compute(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC32_H */
