/**
 * @file app_monitor.h
 * @brief OLED 本地显示接口（F103 降级显示通道）
 *
 * 来源：前序工程 APP/app_monitor.h
 * 本工程修改：
 *   - 阶段 1：新增快照式脏标记；页切换 5s
 */
//
// Created by A on 2026/5/27.
//

#ifndef SMART_HOME_APP_MONITOR_H
#define SMART_HOME_APP_MONITOR_H

#include "main.h"
typedef enum
{
    MONITOR_PAGE_ENV = 0,

    MONITOR_PAGE_SYS,

    MONITOR_PAGE_MAX

} monitor_page_t;
void APP_MONITOR_Task(void);
void APP_MONITOR_NextPage(void);
void APP_MONITOR_ShowSysPage(void);
void APP_MONITOR_ShowEnvPage(void);
#endif //SMART_HOME_APP_MONITOR_H
