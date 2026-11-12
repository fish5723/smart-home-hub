/**
 * @file app_mq4.c
 * @brief MQ-4 可燃气体应用层任务 — 1Hz 采集 + 标定
 *
 * 阶段 1（T-02）：接入 MQ 标定模块，输出 Rs/R0 与 ppm。
 * 旧 percent 字段保留赋值以兼容 OLED 显示。
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "app_mq4.h"
#include "bsp_adc.h"
#include "sensor/app_mq_calib.h"
#include "stdio.h"
#include "app_data.h"
#include "fmt_fixed.h"
#include "debug_config.h"

/*
 * MQ-4 为 5V 供电，其模拟输出必须经分压后才能接入 3.3V ADC 引脚。
 * 阶段 1 验收整改（§2.3）：分压比已集中到 app_mq_calib.h 的 MQ_DIVIDER_MQ4 ——
 * 本文件不再各自定义，避免"两处数值不一致"。实测方法与影响见该处注释。
 */
#define MQ4_DIVIDER_RATIO   MQ_DIVIDER_MQ4

/* 执行周期 */
#define MQ4_PERIOD_MS 1000U

/* 2026-11-03 现场：MQ-4 模拟输入在 0~0.3V 间大幅摆动，无燃气时仍反复跳
 * 优/差/超量程并误报，硬件修复前曾把该通道业务输出停用（quality=FAULT）。
 * 2026-11-11：改用社区口径单点标定（R0=Rs(air)/4.4，ppm=1012.7·ratio^-2.786，
 * 见 app_mq_calib.h），读数具备物理意义，恢复输出；若实机仍抖动，
 * 再临时改回 0。 */
#ifndef APP_MQ4_ENABLE
#define APP_MQ4_ENABLE 1
#endif

/* MQ-4 标定上下文 */
static mq_calib_t s_mq4_calib;
static uint8_t    s_mq4_inited = 0;

void APP_MQ4_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  fault_cnt = 0;      /* 连续失败计数（成功时清零） */
    if (HAL_GetTick() - last_tick < MQ4_PERIOD_MS)
        return;
    last_tick = HAL_GetTick();

    uint16_t adc;
    if (!BSP_ADC_ReadFiltered(BSP_ADC_CH_MQ4, &adc))
    {
        g_sensor_data.gas_valid = 0;
        if (++fault_cnt >= SENSOR_FAULT_COUNT)
            g_sensor_data.gas_quality = (uint8_t)SENSOR_QUAL_FAULT;
        return;
    }
    fault_cnt = 0;                      /* 读取成功 → 连续失败计数清零 */

    /* ── 惰性初始化标定上下文 ── */
    if (!s_mq4_inited) {
        APP_MQ_Calib_Init(&s_mq4_calib, MQ_TYPE_MQ4,
                          MQ4_DIVIDER_RATIO, MQ_CALIB_RL_DEFAULT);
        /* 尝试恢复上次标定的 R0（省掉 24h 老化）。
         * 必须在 Init 之后调用 —— LoadR0 要用 divider_ratio 换算 r0_mv。 */
        (void)APP_MQ_Calib_LoadR0(&s_mq4_calib);
        s_mq4_inited = 1;
    }

    uint16_t pin_mv = BSP_ADC_RawToMv(adc);

    /* ── 驱动标定状态机 ── */
    APP_MQ_Calib_Process(&s_mq4_calib, adc, pin_mv);

    /* ── quality / valid 标记（T-03）──
     * 阶段 1 验收整改（报告 §2.2 第 2 项）：
     *   原实现只有 WARMUP 被判为预热，AGING / R0_READY（正式运行时 24h 的绝大部分）
     *   都报 "OK"，且 valid 无条件置 1 —— 而告警只看 valid，于是预热/老化期那
     *   无意义的 Rs/R0 = 1.00 可以直接触发蜂鸣。
     *   现改为：标定完成（READY）之前一律标 WARMUP，且 valid 由 quality 推导，
     *   不参与告警与控制判定。 */
    if (s_mq4_calib.state != MQ_CALIB_READY)
        g_sensor_data.gas_quality = (uint8_t)SENSOR_QUAL_WARMUP;
    else
        g_sensor_data.gas_quality = sensor_quality_from_adc(adc);

#if !APP_MQ4_ENABLE
    /* 硬件修复前的临时停用：一律 FAULT，不参与等级/告警判定。 */
    g_sensor_data.gas_quality = (uint8_t)SENSOR_QUAL_FAULT;
#endif

    /* ── 更新系统数据中心 ── */
    g_sensor_data.gas_raw   = adc;
    g_sensor_data.gas_mv    = pin_mv;
    g_sensor_data.gas_rs_r0 = s_mq4_calib.rs_r0;
    g_sensor_data.gas_ppm   = s_mq4_calib.concentration;

    /* percent 字段（0~100%，供 OLED 显示）
     * 阶段 1 验收整改：标定完成前**不再**用 adc*100/4095 这个无物理意义的值
     * 伪装成浓度百分比（它会让 OLED 显示一个看着正常、实则错误的数字）。
     * 现在未标定时统一给 0，并由 quality=WARMUP + valid=0 表明"不可用"，
     * OLED 对不可用数据显示 "--"。
     * 量程：与 air 统一到 MQ_PCT_FULL_SCALE_PPM（见 app_mq_calib.h），
     * 使同一百分比在两条通道上代表同一 ppm。 */
    uint8_t gas_percent;
    if (s_mq4_calib.state == MQ_CALIB_READY) {
        uint16_t p = (uint16_t)(s_mq4_calib.concentration * 100U / MQ_PCT_FULL_SCALE_PPM);
        gas_percent = (p > 100U) ? 100U : (uint8_t)p;
    } else {
        gas_percent = 0U;
    }

    g_sensor_data.gas       = gas_percent;
    g_sensor_data.gas_tick  = HAL_GetTick();   /* 数据新鲜度（§3.4）*/
    g_sensor_data.gas_valid = sensor_valid_from_quality(g_sensor_data.gas_quality);

#if DBG_SENSOR_PERIODIC
    char s_r0[12], s_ppm[12];
    fmt_fixed(s_r0,  sizeof(s_r0),  s_mq4_calib.rs_r0,        2);
    fmt_fixed(s_ppm, sizeof(s_ppm), s_mq4_calib.concentration, 1);
    printf("[ADC] MQ4 raw=%4u  mv=%4u  Rs/R0=%s  ppm=%s  %s\r\n",
           adc, pin_mv, s_r0, s_ppm,
           sensor_quality_str(g_sensor_data.gas_quality));
#endif
}
