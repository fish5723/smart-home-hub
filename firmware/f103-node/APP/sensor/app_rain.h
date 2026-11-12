/**
 * @file app_rain.h
 * @brief 雨滴传感器应用层接口（1Hz）
 *
 * 来源：前序工程 APP/rain/app_rain.h
 * 本工程修改：
 *   - 阶段 1：新增 raw/mv 与 quality；raining 阈值判定保留
 */
#ifndef SMART_HOME_APP_RAIN_H
#define SMART_HOME_APP_RAIN_H

#include "main.h"

/* 注：原 app_rain_data_t / g_rain_data 影子结构已于阶段 1 验收整改中移除（§3.4）——
 * 它是只写不读的第二份状态，唯一消费者（OLED）已直接读 g_sensor_data。 */

void APP_RAIN_Task(void);

#endif
