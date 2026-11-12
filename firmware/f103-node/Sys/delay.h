/**
 * @file delay.h
 * @brief 延时接口（delay_init / delay_us / delay_micros）
 *
 * 来源：前序工程 USER/Delay/delay.h
 * 本工程修改：
 *   - 阶段 1：目录从 USER/Delay 移到 Sys/
 *   - 阶段 1 验收整改（§3.3）：新增 delay_micros()，供任务耗时统计使用
 */
//
// Created by A on 2026/5/4.
//

#ifndef DELAY_H
#define DELAY_H
#include "stm32f1xx_hal.h"
#include "core_cm3.h"
// 初始化延时函数（需在系统时钟配置后调用）
void delay_init(void);

// 微秒级延时（阻塞）
void delay_us(uint32_t nus);

/**
 * @brief 读取单调递增的微秒计时（基于 DWT CYCCNT）
 *
 * 用途：任务耗时统计（阶段 1 验收整改 §3.3）。
 * ⚠️ CYCCNT 为 32 位，72MHz 下约 59.6 秒回绕一次 —— 只适合做**区间差值**
 *    （先取 t0、再取 t1、相减），不能当作长时间基准使用。
 */
uint32_t delay_micros(void);

// 毫秒级延时（阻塞，直接调用 HAL_Delay）
#define delay_ms(ms) HAL_Delay(ms)
#endif //DELAY_H
