/**
 * @file app_soil.h
 * @brief 土壤湿度应用层接口（1Hz）
 *
 * 来源：前序工程 APP/soil/app_soil.h
 * 本工程修改：
 *   - 阶段 1：新增 raw/mv 与 quality；⚠️ 探头断开时仍置 valid=1 且 percent=0（误判"极干"），验收报告 §2.2
 */
//
// Created by A on 2026/5/27.
//

#ifndef APP_SOIL_H
#define APP_SOIL_H
#include "main.h"

#ifdef __cplusplus
extern "C"{
#endif

/* 注：原 app_soil_data_t / g_soil_data 影子结构已于阶段 1 验收整改中移除（§3.4）——
 * 它是只写不读的第二份状态，唯一消费者（OLED）已直接读 g_sensor_data。 */

void APP_SOIL_Task(void);

#ifdef __cplusplus
}
#endif
#endif
