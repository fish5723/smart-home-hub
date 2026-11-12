/**
 * @file app_air.h
 * @brief MQ-135 空气质量应用层接口（1Hz）
 *
 * 来源：前序工程 APP/mq-135/app_air.h
 * 本工程修改：
 *   - 阶段 1（T-02/T-03）：接入 MQ 标定模块输出 Rs/R0 与 ppm，新增 quality；⚠️ 旧 percent（adc*100/4095）在标定完成前仍在用（验收报告 §2.3）
 */
//
// Created by A on 2026/5/27.
//

#ifndef SMART_HOME_APP_AIR_H
#define SMART_HOME_APP_AIR_H
#include "main.h"

/* 注：原 app_air_data_t / g_air_data 影子结构已于阶段 1 验收整改中移除（§3.4）——
 * 它是只写不读的第二份状态，唯一消费者（OLED）已直接读 g_sensor_data。 */

void APP_AIR_Task(void);
#endif //SMART_HOME_APP_AIR_H
