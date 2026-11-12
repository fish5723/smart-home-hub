/**
 * @file app_mq4.h
 * @brief MQ-4 可燃气体应用层接口（1Hz）
 *
 * 来源：前序工程 APP/mq-4/app_mq4.h
 * 本工程修改：
 *   - 阶段 1（T-02/T-03）：接入 MQ 标定模块输出 Rs/R0 与 ppm，新增 quality；⚠️ ppm 换算存在取值域缺陷，当前恒为 100.0（验收报告 §2.2）
 */
//
// Created by A on 2026/5/27.
//

#ifndef SMART_HOME_APP_MQ4_H
#define SMART_HOME_APP_MQ4_H
#include "main.h"

/* 注：原 app_mq4_data_t / g_mq4_data 影子结构已于阶段 1 验收整改中移除（§3.4）——
 * 它是只写不读的第二份状态，唯一消费者（OLED）已直接读 g_sensor_data。 */

void APP_MQ4_Task(void);
#endif //SMART_HOME_APP_MQ4_H
