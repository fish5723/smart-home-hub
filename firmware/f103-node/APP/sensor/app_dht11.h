/**
 * @file app_dht11.h
 * @brief DHT11 应用层接口（2s）
 *
 * 来源：前序工程 APP/dht11/app_dht11.h
 * 本工程修改：
 *   - 阶段 1：新增 quality 与 g_sched_trace 标记；⚠️ 无失败重试（验收报告 §2.3）
 */

#ifndef APP_DHT11_H
#define APP_DHT11_H
#ifdef __cplusplus
extern "C"{
#endif

#include "main.h"

/* 注：原 app_dht11_data_t / g_dht11_data 影子结构已于阶段 1 验收整改中移除（§3.4）——
 * 它是只写不读的第二份状态，唯一消费者（OLED）已直接读 g_sensor_data。 */

void APP_DHT11_Task(void);

#ifdef __cplusplus
}
#endif

#endif