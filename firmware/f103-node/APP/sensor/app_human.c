/**
 * @file app_human.c
 * @brief HC-SR501 PIR 人体检测应用层任务
 *
 * 阶段 1（T-13）：EXTI 中断优先 + 轮询退路
 * 阶段 1 验收整改（报告 §2.3 T-13）：补上原实现缺失的三件事 ——
 *   1) 事件去抖：PIR 在边沿附近会输出多个跳变，设最小事件间隔把它们合并
 *   2) 三个计数器：EXTI 原始边沿 / 去抖后接受的事件 / 纯轮询发现的跳变
 *      —— 这三个数就是验收要求的"中断计数与轮询计数一致性对比"的原始数据
 *      （irq 远大于 evt 说明抖动明显；poll 远小于 evt 说明 100ms 轮询会漏事件）
 *   3) 数据新鲜度时间戳 human_tick
 *   同时去掉了只写不读的全局影子结构 g_human_data（§3.4）。
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "app_human.h"
#include "app_data.h"
#include "bsp_human.h"
#include "stdio.h"

/* 执行周期 */
#define HUMAN_PERIOD_MS     100U

/* 最小事件间隔：HC-SR501 输出在跳变附近可能有多次抖动，
 * 同时人走过时输出会持续数秒 —— 800ms 足以把同一次经过合并成一个事件，
 * 又不会把连续两次经过误合并。 */
#define HUMAN_DEBOUNCE_MS   800U

/* HC-SR501 reports motion pulses rather than continuous occupancy. */
#define HUMAN_OCCUPANCY_HOLD_MS  10000U

/** 人体检测任务（T-13：EXTI 优先 + 轮询退路 + 去抖 + 计数）*/
void APP_HUMAN_Task(void)
{
    static uint32_t last_tick       = 0;
    static uint32_t last_event_tick = 0;
    static uint8_t  last_state      = 0;   /* 上一次稳定占用状态（打印用） */
    static uint8_t  last_polled_state = 0; /* 上一次原始轮询电平 */
    static uint32_t s_events        = 0;   /* 去抖后接受的事件数 */
    static uint32_t s_poll_events   = 0;   /* 纯轮询路径发现的跳变数 */
    static uint8_t  occupancy_state = 0;
    static uint32_t low_since_tick  = 0;
    static uint8_t  initialized     = 0;

    uint32_t now = HAL_GetTick();

    if (now - last_tick < HUMAN_PERIOD_MS)
        return;
    last_tick = now;

    /* ── 两条路径分别取值 ── */
    uint8_t polled = BSP_HUMAN_Read();        /* 引脚当前电平 */
    uint8_t irq    = BSP_HUMAN_GetTrigger();  /* 读取即清除，表示"发生过边沿" */

    /* 纯轮询路径看到的跳变（用于与中断路径对比）*/
    if (polled != last_polled_state) {
        s_poll_events++;
        last_polled_state = polled;
    }

    /* ── 事件去抖 ── */
    uint8_t accepted = 0;
    if (irq) {
        if (last_event_tick == 0U || (now - last_event_tick) >= HUMAN_DEBOUNCE_MS) {
            s_events++;
            last_event_tick = now;
            accepted = 1;
        }
    }

    /* 状态判定：短脉冲保护 —— 中断捕获到了边沿，但 100ms 轮询可能已经
     * 读不到那个电平（脉冲短于轮询周期），此时以"边沿发生时的电平"为准，
     * 避免把一个真实事件丢掉。 */
    uint8_t raw_state = polled;
    if (accepted) {
        raw_state = BSP_HUMAN_GetEdgeState();
    }

    /* 高电平立即确认有人；低电平连续保持 10 秒才确认无人。事件计数
     * 仍保留原始/去抖后的运动事件语义，不受占用保持影响。 */
    if (!initialized) {
        occupancy_state = raw_state ? 1U : 0U;
        initialized = 1U;
        low_since_tick = raw_state ? 0U : now;
    } else if (raw_state) {
        occupancy_state = 1U;
        low_since_tick = 0U;
    } else if (occupancy_state) {
        if (low_since_tick == 0U) {
            low_since_tick = now;
        } else if ((now - low_since_tick) >= HUMAN_OCCUPANCY_HOLD_MS) {
            occupancy_state = 0U;
            low_since_tick = 0U;
        }
    }

    uint8_t state = occupancy_state;

    /* ── 写入数据中心 ── */
    g_sensor_data.human             = state;
    g_sensor_data.human_quality     = (uint8_t)SENSOR_QUAL_VALID;
    g_sensor_data.human_irq_count   = BSP_HUMAN_GetIrqCount();
    g_sensor_data.human_event_count = s_events;
    g_sensor_data.human_poll_events = s_poll_events;
    g_sensor_data.human_tick        = now;

    /* 仅状态变化时打印，避免 10Hz 刷屏 */
    if (state != last_state) {
        printf("HUMAN: %s  (irq=%lu evt=%lu poll=%lu)\r\n",
               state ? "DETECTED" : "GONE",
               (unsigned long)g_sensor_data.human_irq_count,
               (unsigned long)s_events,
               (unsigned long)s_poll_events);
        last_state = state;
    }
}
