/**
 * @file bsp_adc.c
 * @brief ADC 采集统一层实现 — 通道表 + 多次采样 + 中值滤波
 *
 * 来源：本工程新建（合并前序工程 4 份逐字节相同的重复驱动）
 */
#include "bsp_adc.h"
#include <string.h>

extern ADC_HandleTypeDef hadc1;

/* ── 通道表：唯一事实来源。加一路模拟量只需在这里加一行 ── */
typedef struct {
    uint32_t    channel;   /* ADC_CHANNEL_x */
    const char *name;
} bsp_adc_entry_t;

static const bsp_adc_entry_t s_adc_table[BSP_ADC_CH_COUNT] = {
    [BSP_ADC_CH_SOIL]   = { ADC_CHANNEL_0, "SOIL"   },
    [BSP_ADC_CH_MQ4]    = { ADC_CHANNEL_1, "MQ4"    },
    [BSP_ADC_CH_MQ135]  = { ADC_CHANNEL_2, "MQ135"  },
    [BSP_ADC_CH_RAIN]   = { ADC_CHANNEL_3, "RAIN"   },
};

/* ── 滤波参数 ── */
#define BSP_ADC_OVERSAMPLE  9   /* 采样次数（奇数便于取中值） */
#define BSP_ADC_TRIM        2   /* 两端各剔除的样本数 → 保留 5 个取平均 */
#define BSP_ADC_CONV_TIMEOUT_MS  20

/* 转换等待超时：沿用原实现的 20ms（远大于 21µs 的单次转换时间） */

const char *BSP_ADC_ChannelName(bsp_adc_ch_t ch)
{
    if (ch >= BSP_ADC_CH_COUNT) return "INVALID";
    return s_adc_table[ch].name;
}

uint8_t BSP_ADC_ReadRaw(bsp_adc_ch_t ch, uint16_t *raw)
{
    if (ch >= BSP_ADC_CH_COUNT || raw == NULL) return 0;

    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel     = s_adc_table[ch].channel;
    sConfig.Rank        = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
        return 0;
    }

    if (HAL_ADC_Start(&hadc1) != HAL_OK) {
        return 0;
    }

    if (HAL_ADC_PollForConversion(&hadc1, BSP_ADC_CONV_TIMEOUT_MS) != HAL_OK) {
        HAL_ADC_Stop(&hadc1);
        return 0;
    }

    *raw = (uint16_t)HAL_ADC_GetValue(&hadc1);

    HAL_ADC_Stop(&hadc1);
    return 1;
}

/** 插入排序 —— 样本数固定为 9，插入排序比 qsort 更快且无函数指针开销 */
static void sort_u16(uint16_t *a, uint8_t n)
{
    for (uint8_t i = 1; i < n; i++) {
        uint16_t key = a[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

uint8_t BSP_ADC_ReadFiltered(bsp_adc_ch_t ch, uint16_t *raw)
{
    if (ch >= BSP_ADC_CH_COUNT || raw == NULL) return 0;

    uint16_t samples[BSP_ADC_OVERSAMPLE];

    for (uint8_t i = 0; i < BSP_ADC_OVERSAMPLE; i++) {
        if (!BSP_ADC_ReadRaw(ch, &samples[i])) {
            return 0;      /* 任何一次失败都视为整体失败，不返回可疑值 */
        }
    }

    sort_u16(samples, BSP_ADC_OVERSAMPLE);

    /* 去掉两端极值后取平均：既抑制毛刺，又比纯中值保留更多分辨率 */
    uint32_t sum = 0;
    const uint8_t keep = BSP_ADC_OVERSAMPLE - 2 * BSP_ADC_TRIM;
    for (uint8_t i = BSP_ADC_TRIM; i < BSP_ADC_OVERSAMPLE - BSP_ADC_TRIM; i++) {
        sum += samples[i];
    }
    *raw = (uint16_t)(sum / keep);
    return 1;
}

uint16_t BSP_ADC_RawToMv(uint16_t raw)
{
    /* 3.3V 满量程 / 12 位。先乘后除会溢出 16 位，故用 32 位中间量 */
    return (uint16_t)(((uint32_t)raw * 3300U + 2047U) / 4095U);
}
