/**
 * @file app_beep.h
 * @brief 蜂鸣器应用层接口 — 分级告警
 *
 * 来源：前序工程 APP/beep/app_beep.h
 * 本工程修改：
 *   - 阶段 1（T-08）：告警音改用 TIM 多音调（800/2400Hz 交替），不再阻塞主循环
 */
//
// Created by A on 2026/5/27.
//

#ifndef SMART_HOME_APP_BEEP_H
#define SMART_HOME_APP_BEEP_H
#include "main.h"

void APP_BEEP_Init(void);

void APP_BEEP_Alarm(void);

void APP_BEEP_Task(void);
#endif //SMART_HOME_APP_BEEP_H
