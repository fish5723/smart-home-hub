/**
 * @file app_air.c
 * @brief MQ-135 空气质量应用层任务 — 1Hz 采集 + 标定
 *
 * 阶段 1（T-02）：接入 MQ 标定模块，输出 Rs/R0 与 ppm。
 * 旧 percent 字段保留赋值以兼容 OLED 显示。
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "app_air.h"
#include "bsp_adc.h"
#include "sensor/app_mq_calib.h"
#include "stdio.h"
#include "app_data.h"
#include "fmt_fixed.h"
#include "debug_config.h"

/*
 * MQ-135 为 5V 供电，分压说明同 MQ-4。
 * 阶段 1 验收整改（§2.3）：分压比已集中到 app_mq_calib.h 的 MQ_DIVIDER_MQ135。
 */
#define MQ135_DIVIDER_RATIO MQ_DIVIDER_MQ135

/* 执行周期 */
#define AIR_PERIOD_MS 1000U

/* MQ-135 标定上下文 */
static mq_calib_t s_air_calib;
static uint8_t    s_air_inited = 0;

void APP_AIR_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  fault_cnt = 0;      /* 连续失败计数（成功时清零） */
    if (HAL_GetTick() - last_tick < AIR_PERIOD_MS)
        return;
    last_tick = HAL_GetTick();

    uint16_t adc;
    if (!BSP_ADC_ReadFiltered(BSP_ADC_CH_MQ135, &adc))
    {
        g_sensor_data.air_valid = 0;
        if (++fault_cnt >= SENSOR_FAULT_COUNT)
            g_sensor_data.air_quality = (uint8_t)SENSOR_QUAL_FAULT;
        return;
    }
    fault_cnt = 0;                      /* 读取成功 → 连续失败计数清零 */

    /* ── 惰性初始化标定上下文 ── */
    if (!s_air_inited) {
        APP_MQ_Calib_Init(&s_air_calib, MQ_TYPE_MQ135,
                          MQ135_DIVIDER_RATIO, MQ_CALIB_RL_DEFAULT);
        /* 尝试恢复上次标定的 R0（省掉 24h 老化）。必须在 Init 之后。 */
        (void)APP_MQ_Calib_LoadR0(&s_air_calib);
        s_air_inited = 1;
    }

    uint16_t pin_mv = BSP_ADC_RawToMv(adc);

    /* ── 驱动标定状态机 ── */
    APP_MQ_Calib_Process(&s_air_calib, adc, pin_mv);

    /* ── quality / valid 标记（T-03）──
     * 阶段 1 验收整改（报告 §2.2 第 2 项）：标定完成前一律 WARMUP 且不可用，
     * 详见 app_mq4.c 的同一处说明。 */
    if (s_air_calib.state != MQ_CALIB_READY)
        g_sensor_data.air_quality = (uint8_t)SENSOR_QUAL_WARMUP;
    else
        g_sensor_data.air_quality = sensor_quality_from_adc(adc);

    /* ── 更新系统数据中心 ── */
    g_sensor_data.air_raw   = adc;
    g_sensor_data.air_mv    = pin_mv;
    g_sensor_data.air_rs_r0 = s_air_calib.rs_r0;
    g_sensor_data.air_ppm   = s_air_calib.concentration;

    /* percent 字段（0~100%，供 OLED 显示）—— 未标定时给 0，不伪装成浓度。
     * 量程：与 gas 统一到 MQ_PCT_FULL_SCALE_PPM（见 app_mq_calib.h）。 */
    uint8_t air_percent;
    if (s_air_calib.state == MQ_CALIB_READY) {
        uint16_t p = (uint16_t)(s_air_calib.concentration * 100U / MQ_PCT_FULL_SCALE_PPM);
        air_percent = (p > 100U) ? 100U : (uint8_t)p;
    } else {
        air_percent = 0U;
    }

    g_sensor_data.air       = air_percent;
    g_sensor_data.air_tick  = HAL_GetTick();   /* 数据新鲜度（§3.4）*/
    g_sensor_data.air_valid = sensor_valid_from_quality(g_sensor_data.air_quality);

#if DBG_SENSOR_PERIODIC
    char s_r0[12], s_ppm[12];
    fmt_fixed(s_r0,  sizeof(s_r0),  s_air_calib.rs_r0,        2);
    fmt_fixed(s_ppm, sizeof(s_ppm), s_air_calib.concentration, 1);
    printf("[ADC] AIR raw=%4u  mv=%4u  Rs/R0=%s  ppm=%s  %s\r\n",
           adc, pin_mv, s_r0, s_ppm,
           sensor_quality_str(g_sensor_data.air_quality));
#endif
}
