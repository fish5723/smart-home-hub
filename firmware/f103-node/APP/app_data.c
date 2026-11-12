/**
 * @file app_data.c
 * @brief 系统数据中心 — 全局实例定义
 *
 * 来源：前序工程 APP/app_data.c（原为裸全局 g_sensor_data）
 * 本工程修改：
 *   - 阶段 1：结构体大幅扩展（raw/mv、Rs/R0、ppm、quality），本文件仅提供定义；⚠️ 仍无时间戳、无脏标记、无并发保护（验收报告 §3.4）
 */
//
// Created by A on 2026/5/27.
//

#include "app_data.h"
sensor_data_t g_sensor_data;