/**
 * @file app_beep.c
 * @brief 蜂鸣器应用层实现 — 分级告警 + 多音调
 *
 * 来源：前序工程 APP/beep/app_beep.c（原用 HAL_Delay(200) 翻转 GPIO）
 * 本工程修改：
 *   - 阶段 1（T-08）：改由 BSP_TIM 驱动，主循环阻塞时音调不再失真；阈值仍为 gas/air 百分比，⚠️ 但 MQ 的 ppm 换算有缺陷（验收报告 §2.2）
 */
//
// Created by A on 2026/5/27.
// 阶段 1（T-08）：改用 TIM4 蜂鸣器 PWM，消除 GPIO 阻塞抖动
//
#include "app_beep.h"
#include "bsp_beep.h"
#include "app_data.h"
#include "app_proto.h"
#include "act_manager.h"

#define BEEP_GAS_ALARM_PERCENT  60U
#define BEEP_AIR_ALARM_PERCENT  60U

/* 持续告警时的重发节流：上升沿立即上报，之后每 30s 重发一次，
 * 避免把单槽 TX 刷满而挤掉 1Hz 的 SENSOR_REPORT。 */
#define ALARM_RESEND_MS         30000U

/* ── 报警音调（频率递增，越急越高）── */
#define BEEP_FREQ_LO    800U    /* 低频报警      */
#define BEEP_FREQ_HI    2400U   /* 高频报警      */
#define BEEP_FREQ_OK    1000U   /* 正常提示      */

#define BEEP_PERIOD_MS  300U    /* 交替间隔      */

void APP_BEEP_Init(void)
{
    BEEP_Init();   /* → TIM4 初始化，默认静音 */
}

void APP_BEEP_Alarm(void)
{
    /*
     * T-08：用 PWM 变频替代 GPIO 翻转 + HAL_Delay 阻塞。
     * 原实现：BEEP_On()→HAL_Delay(200)→BEEP_Off()→HAL_Delay(200)
     * 问题：HAL_Delay 阻塞主循环 400ms，其他 Task 停滞
     * 修复：TIM4 ISR 独立驱动波形，主循环零阻塞
     */
    act_manager_set_beeper(1, BEEP_FREQ_HI, HAL_GetTick());
}

void APP_BEEP_Task(void)
{
    static uint32_t last_toggle_tick = 0;
    static uint8_t  freq_lo = 0;
    uint32_t now = HAL_GetTick();

    /* 告警判定必须同时满足 valid 与 quality == VALID。
     * 阶段 1 验收整改（报告 §2.2 第 2 项）：原实现只看 valid，而 MQ 侧原先
     * 无条件置 valid=1，导致预热/老化期那无意义的 Rs/R0 = 1.00 也能触发蜂鸣。
     * 现在 valid 已由 quality 推导（见 sensor_valid_from_quality），这里再显式
     * 检查 quality，是为了让"告警只吃可信数据"这条约定在消费端也自证一次。 */
    const uint8_t gas_ok = g_sensor_data.gas_valid &&
                           (g_sensor_data.gas_quality == (uint8_t)SENSOR_QUAL_VALID);
    const uint8_t air_ok = g_sensor_data.air_valid &&
                           (g_sensor_data.air_quality == (uint8_t)SENSOR_QUAL_VALID);

    uint8_t alarm = (gas_ok && g_sensor_data.gas >= BEEP_GAS_ALARM_PERCENT) ||
                    (air_ok && g_sensor_data.air >= BEEP_AIR_ALARM_PERCENT);

    /* ── 告警上升沿上报 P4（ALARM_EVENT）──
     * 只用于"通知面板显示"；蜂鸣与安全动作仍全部本地完成（ADR-009）。
     * 阈值与判定都吃 quality==VALID 的数据，与上面的本地告警条件完全一致。 */
    {
        static uint8_t  reported     = 0;
        static uint32_t last_sent_ms = 0;

        if (alarm && (!reported || (now - last_sent_ms) >= ALARM_RESEND_MS)) {
            uint8_t gas_trip = (uint8_t)(gas_ok &&
                                g_sensor_data.gas >= BEEP_GAS_ALARM_PERCENT);
            AppProto_NotifyAlarm(
                gas_trip ? PROTO_ALARM_GAS_MQ4 : PROTO_ALARM_AIR_MQ135,
                gas_trip ? PROTO_LEVEL_URGENT : PROTO_LEVEL_WARN,
                gas_trip ? (float)g_sensor_data.gas : (float)g_sensor_data.air,
                (float)(gas_trip ? BEEP_GAS_ALARM_PERCENT
                                 : BEEP_AIR_ALARM_PERCENT));
            reported     = 1;
            last_sent_ms = now;
        } else if (!alarm) {
            reported = 0;
        }
    }

    if (!alarm) {
        act_manager_set_beeper(0, 0, now);
        freq_lo = 0;
        return;
    }

    if ((now - last_toggle_tick) >= BEEP_PERIOD_MS) {
        last_toggle_tick = now;

        /* 交替高低频 —— "滴嘟滴嘟" 效果 */
        if (freq_lo) {
            act_manager_set_beeper(1, BEEP_FREQ_LO, now);
            freq_lo = 0;
        } else {
            act_manager_set_beeper(1, BEEP_FREQ_HI, now);
            freq_lo = 1;
        }
    }
}
