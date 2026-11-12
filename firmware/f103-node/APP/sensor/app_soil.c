/**
 * @file app_soil.c
 * @brief 土壤湿度应用层任务 — 1Hz 采集 + 百分比换算
 *
 * 阶段 1（T-01）：新增 raw / mv 记录，printf 输出引脚电压
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "app_soil.h"
#include "bsp_adc.h"
#include "app_data.h"
#include "stdio.h"
#include "debug_config.h"

/* 干湿阈值（由硬件标定） */
#define SOIL_DRY_VALUE  4077    /* 空气中干燥值 */
#define SOIL_WET_VALUE  1264    /* 完全浸入水中值 */

/* 执行周期 */
#define SOIL_PERIOD_MS  1000U

/* 时间维中值窗口（2026-10-24）：5 个 1Hz 读数取中值 */
#define SOIL_WIN        5
/* 相邻两次平滑值相差 ≥ 该百分点时打一行诊断日志 */
#define SOIL_JUMP_PCT   20U

static uint8_t s_win[SOIL_WIN];
static uint8_t s_win_n, s_win_idx;
static uint8_t s_has_last, s_last_med;

/** 小窗口插入排序取中值（n≤5，无需 qsort） */
static uint8_t median_u8(const uint8_t *src, uint8_t n)
{
    uint8_t a[SOIL_WIN];
    for (uint8_t i = 0; i < n; i++) a[i] = src[i];
    for (uint8_t i = 1; i < n; i++)
    {
        uint8_t key = a[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    return a[n / 2];
}

/* 注：原 g_soil_data 影子结构已移除（阶段 1 验收整改 §3.4）——
 * 它是只写不读的第二份状态，百分比现在用局部变量。 */

void APP_SOIL_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  fault_cnt = 0;      /* 连续失败计数（成功时清零） */

    if (HAL_GetTick() - last_tick < SOIL_PERIOD_MS)
        return;
    last_tick = HAL_GetTick();

    uint16_t adc;
    if (!BSP_ADC_ReadFiltered(BSP_ADC_CH_SOIL, &adc))
    {
        g_sensor_data.soil_valid = 0;
        if (++fault_cnt >= SENSOR_FAULT_COUNT)
            g_sensor_data.soil_quality = (uint8_t)SENSOR_QUAL_FAULT;
        return;
    }
    fault_cnt = 0;                      /* 读取成功 → 连续失败计数清零 */

    /* ── ADC 有效性判定（T-03）──
     * 阶段 1 验收整改（报告 §2.2 第 4 项）：
     *   原实现无条件 soil_valid = 1。而电阻式土壤探头断线（或悬空）时输出接近
     *   电源轨 → 被判为"极干 0%"，灌溉只看 valid 就会在无水环境下持续开泵。
     *   现改为 valid 只表示"这一帧数据可用于业务判定"，超量程一律不可信。
     *
     * ⚠️ 物理上的诚实说明：探头"插在干燥空气中"与"断线"在电气上难以区分，
     *    两者都会读到接近轨的值。因此这里选择"宁可不下发灌溉"——
     *    基于不可信数据开水泵的风险，远大于漏灌一次。 */
    g_sensor_data.soil_quality = sensor_quality_from_adc(adc);
    g_sensor_data.soil_valid   = (g_sensor_data.soil_quality == (uint8_t)SENSOR_QUAL_VALID) ? 1U : 0U;

    /* ── 记录原始值与引脚电压 ── */
    g_sensor_data.soil_raw = adc;
    g_sensor_data.soil_mv  = BSP_ADC_RawToMv(adc);

    /* ── 百分比换算（局部变量，不再维护第二份全局状态 —— §3.4）── */
    uint8_t percent;
    if (adc >= SOIL_DRY_VALUE)
        percent = 0;
    else if (adc <= SOIL_WET_VALUE)
        percent = 100;
    else
    {
        int32_t p = ((int32_t)SOIL_DRY_VALUE - adc) * 100
                      / (SOIL_DRY_VALUE - SOIL_WET_VALUE);
        if      (p < 0)   p = 0;
        else if (p > 100) p = 100;
        percent = (uint8_t)p;
    }

    /* ── 时间维 5 点中值（2026-10-24 新增）──
     * 背景：实测在湿度过渡区（探头半湿/接触不稳）单秒读数会在 74~100% 之间跳变
     *       （adc 摆动约 600 counts），而静置于稳定湿度时只有 ±1%（实测 60s：49~50%）。
     *       单帧滤波（bsp_adc 的 9 次采样）只能抑制片内噪声，拦不住这种秒级摆动。
     * 做法：对最近 5 个【有效】百分比取中值 → 抗孤立毛刺，但不引入趋势滞后。
     * 诚实边界：这是显示层的稳定化，不改变"探头接触不好"这一物理事实；
     *          跳变超过 SOIL_JUMP_PCT 时打一行日志，便于下次定位是探头还是电路。 */
    if (g_sensor_data.soil_quality == (uint8_t)SENSOR_QUAL_VALID)
    {
        s_win[s_win_idx] = percent;
        s_win_idx = (uint8_t)((s_win_idx + 1U) % SOIL_WIN);
        if (s_win_n < SOIL_WIN)
            s_win_n++;

        uint8_t smoothed = median_u8(s_win, s_win_n);
        if (s_has_last &&
            (uint8_t)(smoothed > s_last_med ? smoothed - s_last_med
                                            : s_last_med - smoothed) >= SOIL_JUMP_PCT)
        {
            printf("[SOIL] jump %u%% -> %u%%  raw_adc=%u\r\n",
                   s_last_med, smoothed, adc);
        }
        s_has_last = 1;
        s_last_med = smoothed;

        g_sensor_data.soil      = smoothed;
        g_sensor_data.soil_tick = HAL_GetTick();   /* 数据新鲜度（§3.4）*/
    }
    else
    {
        s_win_n = 0;                    /* 超量程/故障：清空窗口，恢复后重新积累 */
        s_win_idx = 0;
        s_has_last = 0;
    }

#if DBG_SENSOR_PERIODIC
    printf("[ADC] SOIL raw=%4u  mv=%4u  %u%% -> %u%%  %s\r\n",
           adc, g_sensor_data.soil_mv, percent, g_sensor_data.soil,
           sensor_quality_str(g_sensor_data.soil_quality));
#endif
}
