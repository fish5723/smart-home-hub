/**
 * @file bsp_beep.h
 * @brief 蜂鸣器接口 — 基于 TIM4 的多音调驱动
 *
 * 来源：前序工程 BSP/BEEP（原为纯 GPIO 翻转，无音调能力）
 * 本工程修改：
 *   - 阶段 1（T-08）：改为 TIM 驱动，支持频率设定以区分告警级别
 */
//
// Created by A on 2026/5/27.
// 阶段 1（T-08）：重构为 TIM4 软件翻转，消除 GPIO 阻塞抖动
//

#ifndef SMART_HOME_BEEP_H
#define SMART_HOME_BEEP_H
#include "main.h"

/**
 * @brief 蜂鸣器初始化（T-08：重写为 TIM4 驱动）
 *
 * 调用 BSP_TIM_Buzzer_Init()，启动后默认静音。
 *         └─── 原实现为纯 GPIO 翻转 ───┘
 */
void BEEP_Init(void);

/**
 * @brief 蜂鸣器常开（无源蜂鸣器：等同于启动 1kHz 固定音调）
 * @deprecated 推荐使用 BEEP_PWM_Start(freq) 替代
 */
void BEEP_On(void);

/**
 * @brief 蜂鸣器关闭
 */
void BEEP_Off(void);

/**
 * @brief 翻转（兼容旧接口）
 * @deprecated 推荐使用 BEEP_PWM_Start(freq) 替代
 */
void BEEP_Toggle(void);

/* ═══════════════════════════ T-08 新增 ═══════════════════════════ */

/**
 * @brief 启动 PWM 蜂鸣器（指定频率）
 * @param freq_hz  频率（Hz），0=静音
 */
void BEEP_PWM_Start(uint16_t freq_hz);

/**
 * @brief 停止蜂鸣器
 */
void BEEP_PWM_Stop(void);

/**
 * @brief 运行时改频率
 * @param freq_hz  频率（Hz），0=静音
 */
void BEEP_PWM_SetFreq(uint16_t freq_hz);

#endif /* SMART_HOME_BEEP_H */