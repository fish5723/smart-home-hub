/**
 * @file app_uart.c
 * @brief 串口汇总输出 — 每秒一帧结构化数据（T-16）
 *
 * 来源：前序工程（smart_home）的 APP/uart/app_uart.c（原为单向 ASCII k=v 行）
 * 本工程修改：
 *   - 阶段 1：输出口从 USART1 改为 USART3（printf 走 retarget），USART1 留给协议
 *   - 阶段 1（T-16）：新增 raw/mv、Rs/R0、ppm 与 quality/valid 标记
 *   - 阶段 1 验收整改：① 浮点改 fmt_fixed 定点格式化（移除 -u _printf_float 依赖）
 *     ② 状态列区分 INVALID / STALE / quality 三者（§3.4 的时间戳落地使用）
 *     ③ 补充 PIR 的三路事件计数（T-13 要求的"中断 vs 轮询"对比数据）
 *
 * 注意：本帧是**人读的调试/观测出口**，不是协议。阶段 2 的二进制协议会在
 * USART1 上另起一套（见 docs/03-通信协议规范.md），两者互不干扰。
 */
#include "app_uart.h"
#include "app_data.h"
#include "fmt_fixed.h"
#include <stdio.h>

#define UART_PERIOD_MS 1000U

/**
 * @brief 合成一路数据的状态文本
 *
 * 三态区分的意义（阶段 1 验收整改 §3.4）：
 *   INVALID —— 这一帧数据本身不可信（valid=0：预热中/超量程/故障）
 *   STALE   —— 数据本身可信，但已经太久没更新（> SENSOR_STALE_MS）
 *   quality —— 正常，附上质量等级（OK / WARMUP / OVER / FAULT）
 * 只看 valid 是分不出"坏数据"和"老数据"的，而两者的处理方式完全不同。
 */
static const char *status_str(uint8_t valid, uint8_t quality,
                              uint32_t tick, uint32_t now)
{
    if (!valid) return "INVALID";
    if (sensor_is_stale(tick, now)) return "STALE";
    return sensor_quality_str(quality);
}

void APP_Uart_Task(void)
{
    static uint32_t last_tick = 0;
    if (HAL_GetTick() - last_tick < UART_PERIOD_MS)
        return;
    last_tick = HAL_GetTick();

    const uint32_t now = last_tick;

    /* 浮点值先转定点字符串 —— 避免依赖 newlib 浮点 printf（省约 4.7KB Flash） */
    char s_temp[12], s_humi[12];
    char s_gas_r0[12], s_gas_ppm[12];
    char s_air_r0[12], s_air_ppm[12];
    char s_press[16];
    char s_bmp_t[12];

    fmt_fixed(s_temp,    sizeof(s_temp),    g_sensor_data.temperature, 1);
    fmt_fixed(s_humi,    sizeof(s_humi),    g_sensor_data.humidity,    1);
    fmt_fixed(s_gas_r0,  sizeof(s_gas_r0),  g_sensor_data.gas_rs_r0,   2);
    fmt_fixed(s_gas_ppm, sizeof(s_gas_ppm), g_sensor_data.gas_ppm,     0);
    fmt_fixed(s_air_r0,  sizeof(s_air_r0),  g_sensor_data.air_rs_r0,   2);
    fmt_fixed(s_air_ppm, sizeof(s_air_ppm), g_sensor_data.air_ppm,     0);
    fmt_fixed(s_press,   sizeof(s_press),   g_sensor_data.pressure,    0);
    fmt_fixed(s_bmp_t,   sizeof(s_bmp_t),   g_sensor_data.bmp280_temp, 1);

    /* T-16: 结构化数据 — 全部传感器值 + 状态 + MQ 标定 + PIR 计数 */
    printf("=== DATA ===\r\n"
           "  T=%sC H=%s%% [%s]\r\n"
           "  SOIL : %umV/%u%% [%s]\r\n"
           "  MQ-4 : %umV  Rs/R0=%s  %sppm [%s]\r\n"
           "  MQ135: %umV  Rs/R0=%s  %sppm [%s]\r\n"
           "  RAIN : %umV/%u%%  raining=%d [%s]\r\n"
           "  HUMAN: %d  irq=%lu evt=%lu poll=%lu [%s]\r\n"
           "  BMP280: %sPa %sC [%s]\r\n"
           "  PUMP: %s\r\n",
           s_temp, s_humi,
           status_str(g_sensor_data.dht11_valid, g_sensor_data.dht11_quality,
                      g_sensor_data.dht11_tick, now),
           g_sensor_data.soil_mv, g_sensor_data.soil,
           status_str(g_sensor_data.soil_valid, g_sensor_data.soil_quality,
                      g_sensor_data.soil_tick, now),
           g_sensor_data.gas_mv, s_gas_r0, s_gas_ppm,
           status_str(g_sensor_data.gas_valid, g_sensor_data.gas_quality,
                      g_sensor_data.gas_tick, now),
           g_sensor_data.air_mv, s_air_r0, s_air_ppm,
           status_str(g_sensor_data.air_valid, g_sensor_data.air_quality,
                      g_sensor_data.air_tick, now),
           g_sensor_data.rain_mv, g_sensor_data.rain,
           g_sensor_data.raining,
           status_str(g_sensor_data.rain_valid, g_sensor_data.rain_quality,
                      g_sensor_data.rain_tick, now),
           g_sensor_data.human,
           (unsigned long)g_sensor_data.human_irq_count,
           (unsigned long)g_sensor_data.human_event_count,
           (unsigned long)g_sensor_data.human_poll_events,
           status_str(1U, g_sensor_data.human_quality,
                      g_sensor_data.human_tick, now),
           s_press, s_bmp_t,
           status_str(g_sensor_data.bmp280_valid, g_sensor_data.bmp280_quality,
                      g_sensor_data.bmp280_tick, now),
           g_sensor_data.pump_alarm ? "ALARM" : (g_sensor_data.pump_on ? "ON" : "OFF"));
}
