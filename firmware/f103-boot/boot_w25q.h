/**
 * @file boot_w25q.h
 * @brief bootloader 裸机 SPI1 + W25Q64 访问（读/页写/扇区擦/块擦）
 */
#ifndef BOOT_W25Q_H
#define BOOT_W25Q_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     boot_w25q_init(void);
uint32_t boot_w25q_id(void);
int      boot_w25q_ready(void);

void boot_w25q_read(uint32_t addr, uint8_t *buf, uint32_t len);
int  boot_w25q_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);
int  boot_w25q_erase_sector(uint32_t addr);
int  boot_w25q_erase_block(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_W25Q_H */
