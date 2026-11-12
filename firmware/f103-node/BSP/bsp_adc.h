/**
 * @file bsp_adc.h
 * @brief ADC 采集统一层 — 4 路模拟量的通道表 + 多次采样 + 中值滤波
 *
 * 阶段 1 重构：前序工程把同一份代码复制了 4 遍
 * （BSP/SOIL/soil.c、BSP/MQ-135/bsp_air.c、BSP/MQ-4/bsp_mq4.c、BSP/RAIN/rain.c），
 * 只有 ADC_CHANNEL_x 一个数字不同。本文件把它们合并为一张通道表 + 一次实现，
 * 并补上原文缺失的滤波与电压换算。
 *
 * 引脚（实测自 smart_home.ioc，见 docs/04-硬件接线与BOM.md §2）：
 *   PA0 = ADC1_IN0 = 土壤湿度
 *   PA1 = ADC1_IN1 = MQ-4   可燃气体
 *   PA2 = ADC1_IN2 = MQ-135 空气质量
 *   PA3 = ADC1_IN3 = 雨滴
 *
 * ⚠️ MQ 系列模块为 5V 供电，其模拟输出在高浓度下可能接近 5V，
 *    而 F103 的 ADC 满量程是 3.3V 且多数引脚非 5V 容忍 —— 必须经分压后接入。
 *    分压比由硬件决定，软件侧用 BSP_ADC_RawToMv() 得到引脚电压后
 *    再乘回分压比才是传感器真实输出。详见硬件文档 §3.1。
 *
 * 来源：本工程新建（合并前序工程 4 份重复驱动）
 */
#ifndef BSP_ADC_H
#define BSP_ADC_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ADC 通道编号（对应上面的引脚表，顺序即 ADC_CHANNEL_x 的 x） */
typedef enum {
    BSP_ADC_CH_SOIL  = 0,   /**< PA0 土壤湿度   (ADC_CHANNEL_0) */
    BSP_ADC_CH_MQ4   = 1,   /**< PA1 MQ-4      (ADC_CHANNEL_1) */
    BSP_ADC_CH_MQ135 = 2,   /**< PA2 MQ-135    (ADC_CHANNEL_2) */
    BSP_ADC_CH_RAIN  = 3,   /**< PA3 雨滴      (ADC_CHANNEL_3) */
    BSP_ADC_CH_COUNT
} bsp_adc_ch_t;

/** 单次采样的硬件转换时间参考（239.5 采样周期 + 12.5 转换周期 @12MHz ADCCLK ≈ 21µs） */
#define BSP_ADC_SAMPLE_US        21

/**
 * @brief 单次采样（不做滤波）— 保留原 API 语义
 * @param ch   通道
 * @param raw  输出 0~4095
 * @return 1 成功，0 失败（通道非法 / 转换超时）
 */
uint8_t BSP_ADC_ReadRaw(bsp_adc_ch_t ch, uint16_t *raw);

/**
 * @brief 多次采样 + 极值剔除 + 中值平均（推荐用于所有业务读取）
 *
 * 采样 BSP_ADC_OVERSAMPLE 次，排序后去掉最低/最高各 BSP_ADC_TRIM 个，
 * 对剩余样本取平均。用于抑制开关器件（继电器/水泵）动作时的 ADC 毛刺，
 * 以及 MQ 加热丝带来的周期性波动。
 *
 * 耗时 ≈ BSP_ADC_OVERSAMPLE × 21µs ≈ 190µs（1Hz 轮询下完全可接受；
 * 若将来要提高到 100Hz 以上，需改 DMA 连续采样）。
 *
 * @param ch   通道
 * @param raw  输出 0~4095
 * @return 1 成功，0 失败
 */
uint8_t BSP_ADC_ReadFiltered(bsp_adc_ch_t ch, uint16_t *raw);

/**
 * @brief ADC 原始值 → 引脚电压(mV)，按 3.3V 满量程换算
 * @note 这是【引脚电压】，不是传感器输出。MQ 系列需再乘回分压比：
 *       传感器输出 = BSP_ADC_RawToMv(raw) × (R1+R2)/R2
 */
uint16_t BSP_ADC_RawToMv(uint16_t raw);

/** 供上位使用：通道名（日志/调试） */
const char *BSP_ADC_ChannelName(bsp_adc_ch_t ch);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ADC_H */
