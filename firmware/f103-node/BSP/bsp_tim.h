/**
 * @file bsp_tim.h
 * @brief TIM 通用定时器驱动 — 蜂鸣器 PWM（T-08）
 *
 * 阶段 1（T-08）：
 *   - TIM4 提供独立于主循环的蜂鸣器波形
 *   - 主循环阻塞/卡顿不影响音调
 *   - 支持多频率（报警不同音调）
 *
 * 实现方式：TIM4 更新中断 + ISR 内 GPIO 翻转 PB12
 *   - 无需引脚重映射，不占用硬件 PWM 通道
 *   - PSC=71, ARR 可变 → 分辨率 1µs, 频率范围 7.6Hz~500kHz
 *
 * 来源：本工程新建
 */
#ifndef BSP_TIM_H
#define BSP_TIM_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TIM4 用于蜂鸣器（PB12 翻转）
 *
 * 须在 system clock 就绪后调用。
 * 初始化后蜂鸣器默认静音（TIM 不启动）。
 */
void BSP_TIM_Buzzer_Init(void);

/**
 * @brief 启动蜂鸣器（指定频率）
 * @param freq_hz  频率（Hz），典型值 500~4000
 *                 0 = 静音
 */
void BSP_TIM_Buzzer_Start(uint16_t freq_hz);

/**
 * @brief 停止蜂鸣器
 */
void BSP_TIM_Buzzer_Stop(void);

/**
 * @brief 运行时修改蜂鸣器频率（无需先停再启）
 * @param freq_hz  频率（Hz），0 = 等效停止
 */
void BSP_TIM_Buzzer_SetFreq(uint16_t freq_hz);

#ifdef __cplusplus
}
#endif

#endif /* BSP_TIM_H */