/**
 * @file bsp_beep.c
 * @brief 蜂鸣器实现 — TIM 驱动，多音调
 *
 * 来源：前序工程 BSP/BEEP/beep.c（原为 GPIO 直接翻转）
 * 本工程修改：
 *   - 阶段 1（T-08）：改为 TIM 更新中断产生方波；保留 BEEP_On/Off/Toggle 兼容旧调用
 */
//
// Created by A on 2026/5/27.
// 阶段 1（T-08）：重构为 TIM4 驱动，消除主循环抖动
//
#include "bsp_beep.h"
#include "bsp_tim.h"

/* ── 兼容旧接口（内部标记为 deprecated）── */

void BEEP_Init(void)
{
    BSP_TIM_Buzzer_Init();
}

void BEEP_On(void)
{
    BSP_TIM_Buzzer_Start(1000);   /* 默认 1kHz */
}

void BEEP_Off(void)
{
    BSP_TIM_Buzzer_Stop();
}

void BEEP_Toggle(void)
{
    static uint8_t running = 0;
    if (running) {
        BEEP_Off();
        running = 0;
    } else {
        BEEP_On();
        running = 1;
    }
}

/* ── T-08 新增 PWM 接口 ── */

void BEEP_PWM_Start(uint16_t freq_hz)
{
    BSP_TIM_Buzzer_Start(freq_hz);
}

void BEEP_PWM_Stop(void)
{
    BSP_TIM_Buzzer_Stop();
}

void BEEP_PWM_SetFreq(uint16_t freq_hz)
{
    BSP_TIM_Buzzer_SetFreq(freq_hz);
}