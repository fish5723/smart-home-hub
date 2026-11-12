/**
 * @file app_human.h
 * @brief PIR 人体检测应用层接口（100ms）
 *
 * 来源：前序工程 APP/hc-sr501/app_human.h
 * 本工程修改：
 *   - 阶段 1（T-13）：优先消费 EXTI 中断事件，无事件时回落轮询
 *   - 阶段 1 验收整改：补上去抖 + 三个事件计数器（irq / 去抖后 / 纯轮询），
 *     并移除只写不读的全局影子结构 g_human_data（验收报告 §2.3 T-13 与 §3.4）
 */
//
// Created by A on 2026/5/28.
//

#ifndef SMART_HOME_APP_HUMAN_H
#define SMART_HOME_APP_HUMAN_H
#ifdef __cplusplus
extern "C"{
#endif

#include "main.h"

void APP_HUMAN_Task(void);

#ifdef __cplusplus
}
#endif
#endif //SMART_HOME_APP_HUMAN_H
