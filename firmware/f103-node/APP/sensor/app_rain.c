/**
 * @file app_rain.c
 * @brief 雨滴传感器应用层任务 — 1Hz 采集
 *
 * 阶段 1（T-01）：新增 raw / mv 记录，printf 输出引脚电压
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "app_rain.h"
#include "bsp_adc.h"
#include "app_data.h"
#include "stdio.h"
#include "debug_config.h"

#define RAIN_DRY_VALUE       4095U
#define RAIN_WET_VALUE       1200U
#define RAIN_DETECTED_PERCENT 40U

/* 执行周期 */
#define RAIN_PERIOD_MS 1000U

void APP_RAIN_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  fault_cnt = 0;      /* 连续失败计数（成功时清零） */
    if (HAL_GetTick() - last_tick < RAIN_PERIOD_MS)
        return;
    last_tick = HAL_GetTick();

    uint16_t adc;
    if (!BSP_ADC_ReadFiltered(BSP_ADC_CH_RAIN, &adc))
    {
        g_sensor_data.rain_valid = 0;
        if (++fault_cnt >= SENSOR_FAULT_COUNT)
            g_sensor_data.rain_quality = (uint8_t)SENSOR_QUAL_FAULT;
        return;
    }
    fault_cnt = 0;                      /* 读取成功 → 连续失败计数清零 */
    g_sensor_data.rain_quality = sensor_quality_from_adc(adc);

    /* ── 记录原始值与引脚电压 ── */
    g_sensor_data.rain_raw = adc;
    g_sensor_data.rain_mv  = BSP_ADC_RawToMv(adc);

    /* ── 百分比换算（局部变量，不再维护第二份全局状态 —— §3.4）── */
    uint8_t percent;
    if (adc >= RAIN_DRY_VALUE)
        percent = 0;
    else if (adc <= RAIN_WET_VALUE)
        percent = 100;
    else
        percent = (uint8_t)(((RAIN_DRY_VALUE - adc) * 100U)
                            / (RAIN_DRY_VALUE - RAIN_WET_VALUE));

    uint8_t raining = (percent >= RAIN_DETECTED_PERCENT) ? 1U : 0U;

    g_sensor_data.rain       = percent;
    g_sensor_data.raining    = raining;
    g_sensor_data.rain_tick  = HAL_GetTick();   /* 数据新鲜度（§3.4）*/
    g_sensor_data.rain_valid = sensor_valid_from_quality(g_sensor_data.rain_quality);

#if DBG_SENSOR_PERIODIC
    printf("[ADC] RAIN raw=%4u  mv=%4u  %u%%  %s  %s\r\n",
           adc, g_sensor_data.rain_mv, percent,
           raining ? "RAINING" : "DRY",
           sensor_quality_str(g_sensor_data.rain_quality));
#endif
}
