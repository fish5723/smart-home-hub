/**
 * @file bsp_w25q64.h
 * @brief W25Q64 8MB SPI Flash 驱动接口
 *
 * 阶段 1（T-10）修复：
 *   - 补 WEL 位校验、tPUW 上电等待
 *   - 补 Block Erase (64KB) / Chip Erase
 *
 * 当前状态（2026-10-18 盘点）：**全工程尚无调用者** —— 按计划保留，
 * 给阶段 8 的日志环形缓冲用（届时接入）。驱动本身已在阶段 1 验证过读写与读 ID。
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#ifndef SMART_HOME_BSP_W25Q64_H
#define SMART_HOME_BSP_W25Q64_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void BSP_W25Q64_Init(void);
uint32_t BSP_W25Q64_ReadID(void);
uint8_t BSP_W25Q64_Write(uint32_t addr, uint8_t *buf, uint16_t len);
void BSP_W25Q64_Read(uint32_t addr, uint8_t *buf, uint16_t len);
uint8_t BSP_W25Q64_EraseSector(uint32_t addr);   /*  4KB Sector Erase  */
uint8_t BSP_W25Q64_EraseBlock(uint32_t addr);     /* 64KB Block  Erase  (T-10) */
uint8_t BSP_W25Q64_EraseChip(void);               /*     Chip Erase      (T-10) */

#ifdef __cplusplus
}
#endif

#endif