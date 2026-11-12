/**
 * @file boot_meta.h
 * @brief IAP 元数据双副本读写（W25Q64 0x000000 / 0x001000）
 */
#ifndef BOOT_META_H
#define BOOT_META_H

#include "iap_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 读取有效元数据（magic+CRC16 校验，选 seq 最大者）。
 *  @return 1=找到有效副本，0=两份都无效。 */
int boot_meta_load(iap_meta_t *out);

/** 写入新一份元数据（seq 自动 +1，按 seq 奇偶交替写 A/B 扇区）。
 *  旧副本在新副本写完前保持有效。@return 1=成功 */
int boot_meta_write(const iap_meta_t *meta);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_META_H */
