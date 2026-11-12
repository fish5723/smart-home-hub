/**
 * @file app_data.h
 * @brief 系统数据中心 — 所有传感器/执行器状态的唯一事实来源
 *
 * 阶段 1 重构：
 *   - T-01：新增 raw / mv 字段，记录 ADC 原始值与引脚电压
 *   - T-02：新增 Rs/R0 / ppm 字段（MQ 标定模块）
 *   - T-03：新增 quality 标记，区分 有效/预热中/超量程/故障
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#ifndef APP_DATA_H
#define APP_DATA_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  传感器数据质量标记（T-03）
 *
 *  VALID     = 数据正常，可参与告警/控制判定
 *  WARMUP    = 传感器预热中（MQ 上电 60s 内）— 不参与告警
 *  OVERRANGE = ADC 值接近 0 或 4095（可能短路/开路）— 标记异常
 *  FAULT     = 连续 N 次读取失败 — 传感器离线或损坏
 *  ════════════════════════════════════════════════════ */
typedef enum {
    SENSOR_QUAL_VALID     = 0,
    SENSOR_QUAL_WARMUP    = 1,
    SENSOR_QUAL_OVERRANGE = 2,
    SENSOR_QUAL_FAULT     = 3,
} sensor_quality_t;

/** 超量程阈值：ADC 读数 < 低阈值 或 > 高阈值 视为超量程 */
#define ADC_OVERRANGE_LO    5U     /**< 接近 GND 短路判定 */
#define ADC_OVERRANGE_HI    4090U  /**< 接近 VCC 开路判定 */

/** 故障判定：连续失败次数阈值 */
#define SENSOR_FAULT_COUNT  5U

/* ════════════════════════════════════════════════════
 *  并发约定（阶段 1 验收整改：报告 §3.4）
 *
 *  `g_sensor_data` 是【主循环单写者】结构：
 *    - 唯一写入者是主循环里的各 APP_*_Task（串行执行，无抢占）
 *    - 现有唯一的硬件中断（PIR EXTI）**不直接写本结构**，只在 bsp_human.c
 *      内更新自己的 static 标志/计数，由 APP_HUMAN_Task 读取后再写入本结构
 *    - 因此当前**不需要加锁**
 *
 *  ⚠️ 若将来新增"在中断里直接写 g_sensor_data"的代码，必须同时引入临界区
 *     （__disable_irq / taskENTER_CRITICAL），否则会出现撕裂读取。
 *     这条约定刻意写下来 —— 它比"看起来没问题"可靠。
 *
 *  valid 与 quality 的关系（T-03 收口）：
 *    quality 是完整的质量描述；valid 是它派生出的布尔量
 *    （见 sensor_valid_from_quality()，仅 VALID 时为 1）。
 *    新代码请直接用 quality；valid 保留是为了兼容旧消费方。
 * ════════════════════════════════════════════════════ */

/** 数据陈旧阈值：超过该时长未更新即视为陈旧 */
#define SENSOR_STALE_MS   5000U

/* ════════════════════════════════════════════════════
 *  传感器数据聚合结构体
 *  ─ 所有模块通过 extern g_sensor_data 读写
 *  ─ 串口输出由此结构体直接格式化
 * ════════════════════════════════════════════════════ */
typedef struct {

    /* —— ADC 通道原始值 & 电压 —— */
    uint16_t soil_raw;          /* PA0 土壤湿度 ADC 原始值 */
    uint16_t soil_mv;           /* PA0 引脚电压 (mV) */
    uint8_t  soil;              /* 土壤湿度百分比 0~100 */
    uint8_t  soil_valid;        /* 旧 valid 位（1=有效），向下兼容 */
    uint8_t  soil_quality;      /* sensor_quality_t — 仅当 VALID 时参与判定 */

    uint16_t gas_raw;           /* PA1 MQ-4    ADC 原始值 */
    uint16_t gas_mv;            /* PA1 引脚电压 (mV) —— 未经分压补偿 */
    float    gas_rs_r0;         /* MQ-4  Rs/R0 比值（T-02 标定后有效） */
    float    gas_ppm;           /* MQ-4  估算浓度 ppm   */
    uint8_t  gas;               /* MQ-4  百分比 (旧字段，保持兼容 OLED) */
    uint8_t  gas_valid;
    uint8_t  gas_quality;

    uint16_t air_raw;           /* PA2 MQ-135  ADC 原始值 */
    uint16_t air_mv;            /* PA2 引脚电压 (mV) —— 未经分压补偿 */
    float    air_rs_r0;         /* MQ-135 Rs/R0 比值（T-02 标定后有效） */
    float    air_ppm;           /* MQ-135 估算浓度 ppm   */
    uint8_t  air;               /* MQ-135 百分比 (旧字段，保持兼容 OLED) */
    uint8_t  air_valid;
    uint8_t  air_quality;

    uint16_t rain_raw;          /* PA3 雨滴    ADC 原始值 */
    uint16_t rain_mv;           /* PA3 引脚电压 (mV) */
    uint8_t  rain;              /* 雨滴百分比 0~100 */
    uint8_t  rain_valid;
    uint8_t  rain_quality;
    uint8_t  raining;           /* 是否正在下雨 (阈值判定) */

    /* —— 其它传感器 —— */
    float temperature;
    float humidity;
    uint8_t dht11_valid;
    uint8_t dht11_quality;

    float pressure;
    float bmp280_temp;          /* BMP280 片上温度 °C（与气压同帧读出） */
    uint8_t bmp280_valid;
    uint8_t bmp280_quality;

    uint8_t human;              /* PIR 人体检测 */
    uint8_t human_quality;
    /* PIR 事件计数（T-13）：用于"中断计数 vs 轮询计数"一致性对比
     *   irq_count   = EXTI 原始边沿数（含抖动）
     *   event_count = 去抖后接受的次数（可作为"有人经过"的近似）
     *   poll_events = 由电平轮询发现的跳变次数（退路路径）
     * 三个数一起看：irq_count 远大于 event_count 说明 PIR 输出抖动明显；
     * poll_events 远小于 event_count 说明 100ms 轮询会漏事件。 */
    uint32_t human_irq_count;
    uint32_t human_event_count;
    uint32_t human_poll_events;

    /* —— 执行器状态 —— */
    uint8_t pump_on;
    uint8_t pump_alarm;         /* T-18: 水泵超时/干烧告警（锁存，需灌溉需求消失才解除） */
    uint8_t lock_unlocked;

    /* ════════════════════════════════════════════════════
     *  数据新鲜度时间戳（阶段 1 验收整改：报告 §3.4）
     *
     *  每个传感器记录"最后一次成功更新"的 HAL_GetTick()。
     *  用途：消费方需要区分"这一帧数据是否可信"（valid/quality）
     *        与"这份数据是多久以前的"（tick）——valid 只说明前者。
     *        阶段 3 的"影子状态 + 陈旧标记"直接依赖它。
     *  0 表示从未更新过（见 sensor_is_stale）。
     * ════════════════════════════════════════════════════ */
    uint32_t soil_tick;
    uint32_t gas_tick;
    uint32_t air_tick;
    uint32_t rain_tick;
    uint32_t dht11_tick;
    uint32_t bmp280_tick;
    uint32_t human_tick;
    uint32_t pump_tick;         /* 灌溉控制的最后一次执行 */

} sensor_data_t;

extern sensor_data_t g_sensor_data;

/** 工具函数：根据 ADC 原始值判断是否超量程 */
static inline uint8_t sensor_quality_from_adc(uint16_t adc_raw)
{
    if (adc_raw <= ADC_OVERRANGE_LO || adc_raw >= ADC_OVERRANGE_HI)
        return (uint8_t)SENSOR_QUAL_OVERRANGE;
    return (uint8_t)SENSOR_QUAL_VALID;
}

/** 工具函数：quality → 人类可读字符串 */
static inline const char *sensor_quality_str(uint8_t q)
{
    static const char *names[] = { "OK", "WARMUP", "OVER", "FAULT" };
    if (q > 3) return "?";
    return names[q];
}

/**
 * @brief 由 quality 推导 valid（T-03）
 *
 * 语义约定（阶段 1 验收整改，报告 §2.2 第 2/4 项）：
 *   `valid` 表示"这一帧数据可以参与告警与控制的业务判定"，
 *   而不仅仅是"本次读取没有报错"。因此**只有 VALID 才置 1**：
 *     - WARMUP   （MQ 预热/老化期）→ 不可用于告警
 *     - OVERRANGE（探头断线/悬空）→ 不可用于灌溉
 *     - FAULT    （连续失败）      → 不可用于任何判定
 *   原实现多处无条件置 valid=1，导致预热期无意义数据可触发蜂鸣、
 *   土壤探头断线可被当"极干"而误灌溉。
 */
static inline uint8_t sensor_valid_from_quality(uint8_t q)
{
    return (q == (uint8_t)SENSOR_QUAL_VALID) ? 1U : 0U;
}

/**
 * @brief 数据是否陈旧（§3.4）
 * @param tick 该传感器的时间戳（0 表示从未更新 → 也算陈旧）
 * @param now  当前 HAL_GetTick()
 */
static inline uint8_t sensor_is_stale(uint32_t tick, uint32_t now)
{
    if (tick == 0U) return 1U;
    return ((now - tick) > SENSOR_STALE_MS) ? 1U : 0U;
}

/**
 * @brief 数据年龄（ms）；从未更新时返回 0xFFFFFFFF 表示未知
 */
static inline uint32_t sensor_age_ms(uint32_t tick, uint32_t now)
{
    if (tick == 0U) return 0xFFFFFFFFU;
    return now - tick;
}

#ifdef __cplusplus
}
#endif

#endif /* APP_DATA_H */