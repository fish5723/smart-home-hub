/**
 * @file app_bmp280.h
 * @brief BMP280 应用层接口（1Hz）
 *
 * 来源：前序工程 APP/bmp-280/app_bmp280.h
 * 本工程修改：
 *   - 阶段 1：新增 quality；⚠️ fault_cnt 成功后未清零（验收报告 §3.2）
 */
//
// Created by A on 2026/5/29.
//

#ifndef SMART_HOME_APP_BMP280_H
#define SMART_HOME_APP_BMP280_H
#ifdef __cplusplus
extern "C"{
#endif
    #include "main.h"
    // 初始化BMP280
    void APP_BMP280_Init(void);
    // 任务函数
    void APP_BMP280_Task(void);
#ifdef __cplusplus
}
#endif
#endif //SMART_HOME_APP_BMP280_H
