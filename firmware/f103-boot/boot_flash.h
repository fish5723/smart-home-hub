/**
 * @file boot_flash.h
 * @brief bootloader 内部 Flash 编程（页擦除/半字写入，例程放 RAM）
 */
#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void boot_flash_unlock(void);
void boot_flash_lock(void);

/** 擦除 app 区 1KB 页；addr 必须在 app 槽内且 1KB 对齐。 */
int boot_flash_erase_page(uint32_t addr);

/** 写一个半字；addr 必须偶数且在 app 槽内。 */
int boot_flash_program_halfword(uint32_t addr, uint16_t value);

/** 按字节写一段（内部按半字拆分，奇数长度用 0xFF 补齐）。
 *  返回写入的字节数，失败返回 -1。 */
int boot_flash_program_bytes(uint32_t addr, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_FLASH_H */
