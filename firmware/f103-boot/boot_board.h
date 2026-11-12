/**
 * @file boot_board.h
 * @brief bootloader 板级底座：72MHz 时钟、DWT 延时、IWDG 8s、USART3 日志
 */
#ifndef BOOT_BOARD_H
#define BOOT_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 配置 HSE/PLL 72MHz（失败回退 HSI 8MHz）、DWT、IWDG 8s、USART3 115200。 */
void boot_board_init(void);

/** 当前 SYSCLK（Hz）。 */
uint32_t boot_cpu_hz(void);

/** 基于 DWT 的毫秒延时；循环内喂 IWDG。 */
void boot_delay_ms(uint32_t ms);

/** 启动 IWDG，标称 8s（LSI 40kHz，PR=128，RLR=2500）。 */
void boot_iwdg_init(void);

/** 喂 IWDG。 */
void boot_iwdg_feed(void);

/** USART3 PB10/PB11 115200 阻塞日志（无 printf/newlib）。 */
void boot_log(const char *text);
void boot_log_hex(uint32_t value);
void boot_log_u32(uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_BOARD_H */
