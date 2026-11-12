/**
 * @file bsp_tim.c
 * @brief TIM4 蜂鸣器驱动 — 更新中断 + GPIO 翻转（T-08）— HAL 版本
 *
 * 使用 CubeMX 生成的 htim4 句柄，所有操作通过 HAL 库完成。
 *
 * TIM4 时钟 72MHz（APB1 倍频），PSC=71 → 1MHz 计数（1µs 分辨率）
 * 每次 UEV 翻转 PB12 → 一个完整周期 = 2 次中断
 *
 * 来源：本工程新建 → 阶段 1 重构为 HAL
 */
#include "bsp_tim.h"
#include "tim.h"      /* htim4 句柄（CubeMX 生成）       */

/* ════════════════════════════════════════════════════
 *  硬件常量
 *  ════════════════════════════════════════════════════ */
#define TIM4_PSC        71U
#define TIM4_CLK_HZ     1000000UL   /* 72MHz / (71+1) = 1MHz        */
#include "bsp_pins.h"   /* T-15: 引脚集中 */
#include <stdio.h>      /* printf（初始化失败时告警） */

/* 控制标志 */
static volatile uint8_t buzzer_running = 0;

/* ════════════════════════════════════════════════════
 *  TIM4 初始化（HAL 版本）
 *  ════════════════════════════════════════════════════ */

void BSP_TIM_Buzzer_Init(void)
{
    /* 完整的定时器配置 —— 阶段 1 验收整改（§2.3 T-08）。
     * 原实现把 HAL_TIM_Base_Init 放在 Start() 里，导致"只 Init 不 Start"时
     * TIM4 仍停留在 CubeMX 生成的配置（PSC=0 / ARR=65535，即 72MHz 计数），
     * 与 bsp 的预期（1MHz）不一致 —— 现在 Init 即完成全部配置，
     * 消除"配置散落在两个函数里"的隐患。
     *
     * 说明（为什么蜂鸣器不是硬件 PWM）：PB12 在 STM32F103 上没有定时器输出通道，
     * 因此只能用 TIM4 的更新中断翻转 GPIO 来产生方波，占空比恒 50%。
     * 对无源蜂鸣器而言 50% 正是最佳驱动，见 bsp_pins.h 的说明。 */
    htim4.Instance               = TIM4;
    htim4.Init.Prescaler         = TIM4_PSC;        /* 72MHz → 1MHz 计数 */
    htim4.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim4.Init.Period            = 500U - 1U;       /* 占位：约 1kHz 方波 */
    htim4.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim4) != HAL_OK) {
        printf("[TIM] TIM4 base init FAILED\r\n");
    }

    /* NVIC：TIM4 中断优先级 */
    HAL_NVIC_SetPriority(TIM4_IRQn, 14, 0);
    HAL_NVIC_EnableIRQ(TIM4_IRQn);

    /* PB12 确保高电平（静音）— CubeMX 已初始化但这里兜底 */
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);

    buzzer_running = 0;
}

/* ════════════════════════════════════════════════════
 *  启动 / 停止 / 变频（HAL 版本）
 *  ════════════════════════════════════════════════════ */

void BSP_TIM_Buzzer_Start(uint16_t freq_hz)
{
    if (freq_hz == 0) {
        BSP_TIM_Buzzer_Stop();
        return;
    }

    /* 若已在运行，先停 */
    if (buzzer_running) {
        HAL_TIM_Base_Stop_IT(&htim4);
    }

    /*
     * ARR+1 = TIM4_CLK_HZ / (2 * freq_hz)
     * 因为每两次 UEV 才完成一个 GPIO 翻转周期
     */
    uint32_t arr = TIM4_CLK_HZ / (2UL * freq_hz);
    if (arr < 2)    arr = 2;
    if (arr > 65536) arr = 65536;
    arr -= 1;

    /* 初始化已在 BSP_TIM_Buzzer_Init() 完成，这里只改自动重装载值 —— 
     * 避免每次变频都重跑一遍 HAL_TIM_Base_Init（阶段 1 验收整改 §2.3 T-08） */
    __HAL_TIM_SET_AUTORELOAD(&htim4, arr);
    __HAL_TIM_SET_COUNTER(&htim4, 0);
    SET_BIT(htim4.Instance->EGR, TIM_EGR_UG);   /* 影子寄存器立即生效 */

    /* 启动更新中断 + 计数器 */
    HAL_TIM_Base_Start_IT(&htim4);

    buzzer_running = 1;
}

void BSP_TIM_Buzzer_Stop(void)
{
    HAL_TIM_Base_Stop_IT(&htim4);
    __HAL_TIM_SET_COUNTER(&htim4, 0);
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET); /* 静音 */
    buzzer_running = 0;
}

void BSP_TIM_Buzzer_SetFreq(uint16_t freq_hz)
{
    if (freq_hz == 0) {
        BSP_TIM_Buzzer_Stop();
        return;
    }

    if (!buzzer_running) {
        BSP_TIM_Buzzer_Start(freq_hz);
        return;
    }

    uint32_t arr = TIM4_CLK_HZ / (2UL * freq_hz);
    if (arr < 2)    arr = 2;
    if (arr > 65536) arr = 65536;
    arr -= 1;

    __HAL_TIM_SET_AUTORELOAD(&htim4, arr);
    __HAL_TIM_SET_COUNTER(&htim4, 0);
    /* 生成更新事件 → 影子寄存器立即生效 */
    SET_BIT(htim4.Instance->EGR, TIM_EGR_UG);
}

/* ════════════════════════════════════════════════════
 *  TIM4 中断服务函数（HAL 版本）
 *  ════════════════════════════════════════════════════ */

void TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim4);
}

/**
 * @brief HAL 定时器周期溢出回调
 *
 * 每次 TIM4 更新事件触发时翻转 PB12，产生方波。
 * 频率 = TIM4_CLK_HZ / ((PSC+1) * (ARR+1) * 2)
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4) {
        HAL_GPIO_TogglePin(BUZZER_PORT, BUZZER_PIN);
    }
}