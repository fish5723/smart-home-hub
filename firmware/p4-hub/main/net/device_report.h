/**
 * @file device_report.h
 * @brief 设备属性 / OTA 结果 MQTT 上报（阶段九）
 *
 * 主题：
 *   smart_home/<node>/device      属性快照（retain，连接后立即 + 每 60s）
 *   smart_home/<node>/ota/result  OTA 检查终态事件（retain）
 *
 * 依赖 mqtt_service 已启动；未配置 broker 时 init 返回 ESP_ERR_NOT_FOUND。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建上报任务；需在 mqtt_service_start() 之后调用。 */
esp_err_t device_report_init(void);

/** 请求尽快发布一次属性快照（例如 OTA 结果到达后）。 */
void device_report_request(void);

#ifdef __cplusplus
}
#endif
