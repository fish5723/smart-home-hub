/**
 * @file app_dht11.c
 * @brief DHT11 温湿度应用层任务 — 2s 周期采集
 *
 * 来源：前序工程（smart_home）的 APP/dht11/app_dht11.c
 * 本工程修改：
 *   - 阶段 1：接入 quality 标记与 fault_cnt；进入读取前写 g_sched_trace
 *   - 阶段 1 验收整改：浮点输出改 fmt_fixed（移除 `-u _printf_float` 依赖）。
 *     顺带修掉一个观感 bug——原实现用 %.1f 打印，但 DHT11 驱动只填整数部分，
 *     所以永远显示 .0，看起来像"精度正常"其实并没有小数位。
 */
#include <stdio.h>
#include "app_dht11.h"
#include "app_data.h"
#include "app_scheduler.h"  /* g_sched_trace — 子阶段追踪 */
#include "bsp_dht11.h"
#include "fmt_fixed.h"

#define DHT11_PERIOD_MS 2000U

void APP_DHT11_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  fault_cnt = 0;      /* 连续失败计数（成功时清零） */

    if(HAL_GetTick() - last_tick < DHT11_PERIOD_MS)
    {
        return;
    }

    last_tick = HAL_GetTick();

    dht11_data_t data;

    /* 进入 DHT11_Read 前标记子阶段 */
    g_sched_trace = SCHED_TRACE_DHT11_READ;

    if(DHT11_Read(&data))
    {
        fault_cnt = 0;                  /* 读取成功 → 连续失败计数清零 */

        /* 数据中心（不再维护第二份全局影子结构 —— §3.4） */
        g_sensor_data.temperature = data.temperature;
        g_sensor_data.humidity = data.humidity;
        g_sensor_data.dht11_tick = HAL_GetTick();   /* 数据新鲜度（§3.4）*/
        g_sensor_data.dht11_quality = (uint8_t)SENSOR_QUAL_VALID;
        g_sensor_data.dht11_valid = sensor_valid_from_quality(g_sensor_data.dht11_quality);

        char s_t[12], s_h[12];
        fmt_fixed(s_t, sizeof(s_t), data.temperature, 1);
        fmt_fixed(s_h, sizeof(s_h), data.humidity,    1);
        printf("DHT11 OK T=%s H=%s%%\r\n", s_t, s_h);
    }
    else
    {
        g_sensor_data.dht11_valid = 0;
        if (++fault_cnt >= SENSOR_FAULT_COUNT)
            g_sensor_data.dht11_quality = (uint8_t)SENSOR_QUAL_FAULT;

        printf("DHT11 ERROR tick=%lu\r\n", (unsigned long)HAL_GetTick());
    }
}