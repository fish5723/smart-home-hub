/**
 * @file human.c
 * @brief HC-SR501 PIR 人体检测 BSP 驱动
 *
 * 阶段 1（T-13）：改为 EXTI 中断驱动 + 轮询回退
 *   - EXTI 捕获上升/下降沿，设置 flag 供 APP 层读取
 *   - BSP_HUMAN_Read() 保留轮询接口，可做一致性对比
 *
 * 硬件：PB13，CubeMX 配置为 GPIO_MODE_INPUT + GPIO_PULLDOWN
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "bsp_human.h"
#include "bsp_pins.h"   /* T-15: 引脚集中 */

/* 编译期自检（T-15 整改）：
 * 本文件的 ISR 使用 EXTI15_10_IRQn，只覆盖 EXTI 线 10~15。
 * 用 C11 的 _Static_assert 而不是 #if —— 因为 __builtin_ctz 与 HAL 的
 * GPIO_PIN_n（带强制类型转换）都无法参与预处理求值，但都能作为编译器常量表达式。
 * 这样"把 HUMAN_PIN 改到别的线却忘了改中断向量"会直接编译失败，
 * 而不是运行时静默不触发中断。 */
_Static_assert((BSP_PIN_POS(HUMAN_PIN) >= 10U) && (BSP_PIN_POS(HUMAN_PIN) <= 15U),
               "HUMAN_PIN 必须在 EXTI 线 10~15 上（EXTI15_10_IRQn）；改引脚需同步改中断向量");

/* EXTI 触发的状态标志 */
static volatile uint8_t  s_human_triggered = 0;
static volatile uint8_t  s_human_edge_state = 0;  /* 触发时的引脚电平 */
static volatile uint32_t s_human_irq_count  = 0;  /* 原始边沿计数（含抖动，T-13）*/

/* ════════════════════════════════════════════════════
 *  T-13: EXTI 中断初始化
 * ════════════════════════════════════════════════════ */
void BSP_HUMAN_Init(void)
{
    /* 使能 AFIO 时钟（F1 系列用 AFIO 配置 EXTI 引脚映射）*/
    __HAL_RCC_AFIO_CLK_ENABLE();

    /* 映射 HUMAN_PIN 到对应 EXTI 线
     * T-15 整改：原先 13 被硬编码在 5 处（EXTICR 索引/移位/IMR/RTSR/FTSR），
     * 改引脚极易漏改 —— 现在全部由 HUMAN_EXTI_LINE 推导。 */
    const uint32_t line  = HUMAN_EXTI_LINE;
    const uint32_t idx   = line / 4U;                 /* AFIO->EXTICR[0..3] */
    const uint32_t shift = (line % 4U) * 4U;

    AFIO->EXTICR[idx] &= ~(0x0FU << shift);
    AFIO->EXTICR[idx] |=  (0x01U << shift);           /* 0b0001 = GPIOB */

    /* 配置 EXTI 线：双沿触发 */
    EXTI->IMR  |= (1U << line);   /* 不屏蔽中断          */
    EXTI->RTSR |= (1U << line);   /* 上升沿（人出现）     */
    EXTI->FTSR |= (1U << line);   /* 下降沿（人消失）     */

    /* NVIC 配置 */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 15, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* ════════════════════════════════════════════════════
 *  EXTI 中断服务函数
 *
 *  ISR 职责严格限定为三件事（T-13）：
 *    1. 清中断标志
 *    2. 原始边沿计数 +1
 *    3. 闩锁"已触发"标志与当时的电平
 *  去抖、状态判定、"有人经过"计数全部放在 APP 层任务上下文做 ——
 *  ISR 里不做任何可能阻塞或依赖时序判断的事情。
 * ════════════════════════════════════════════════════ */
void EXTI15_10_IRQHandler(void)
{
    if (__HAL_GPIO_EXTI_GET_IT(HUMAN_PIN) != 0x00u)
    {
        __HAL_GPIO_EXTI_CLEAR_IT(HUMAN_PIN);

        s_human_irq_count++;
        s_human_triggered  = 1;
        s_human_edge_state = (HAL_GPIO_ReadPin(HUMAN_PORT, HUMAN_PIN) == GPIO_PIN_SET) ? 1 : 0;
    }
}

/* ════════════════════════════════════════════════════
 *  APP 层接口
 *  ════════════════════════════════════════════════════ */

uint8_t BSP_HUMAN_Read(void)
{
    return (HAL_GPIO_ReadPin(HUMAN_PORT, HUMAN_PIN) == GPIO_PIN_SET) ? 1 : 0;
}

uint8_t BSP_HUMAN_GetTrigger(void)
{
    uint8_t triggered = s_human_triggered;
    s_human_triggered = 0;
    return triggered;
}

uint8_t BSP_HUMAN_GetEdgeState(void)
{
    return s_human_edge_state;
}

uint32_t BSP_HUMAN_GetIrqCount(void)
{
    return s_human_irq_count;
}