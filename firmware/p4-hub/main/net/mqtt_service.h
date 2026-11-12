/**
 * @file mqtt_service.h
 * @brief MQTT 服务（阶段 5）：连接、LWT、重连、全量/增量状态发布、云命令接收
 *
 * 设计要点：
 *  - 独立后台任务做发布，网络/连接动作不在 LVGL 任务里做（同 weather_service 范式）
 *  - 凭据来源：NVS(storage) → Kconfig 空默认值；uri 为空 = 不启动（延续 ADR-016）
 *  - 本地优先（ADR-004）：云端开关指令经 AppProto_SendActuatorCmdEx(.., CLOUD) 下发，
 *    受协议层 1s 仲裁窗口约束
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 MQTT 服务
 *
 * 读取 broker uri（NVS 'mqtt_uri' → Kconfig），uri 为空时打印一条日志并返回
 * ESP_ERR_NOT_FOUND（不视为错误，不影响启动）。
 *
 * @return ESP_OK 已启动 / ESP_ERR_NOT_FOUND 未配置 / 其他 失败
 */
esp_err_t mqtt_service_start(void);

/** 停止服务并发布 availability=offline（优雅退出时用） */
void mqtt_service_stop(void);

/** broker 当前是否已连接 */
bool mqtt_service_connected(void);

/**
 * @brief 发布一条消息（供 ha_discovery 等使用）
 * @return true 已投递 / false 未连接或失败
 */
bool mqtt_service_publish(const char *topic, const char *payload, int qos, bool retain);

/**
 * @brief 取出一条待下发的云端指令
 *
 * 云端指令不在 MQTT 事件任务里直接下发——协议层 ProtoTx 是单发送槽、无锁，
 * 必须只在主任务上下文写入。因此 MQTT 事件处理只把指令投进队列，
 * 由 app_main 主循环在 AppProto_Poll() 之后取出并调用 AppProto_SendActuatorCmdEx(CLOUD)。
 *
 * @param[out] dev_id  PROTO_DEV_xxx
 * @param[out] action  PROTO_ACTION_xxx
 * @return true = 取到一条；false = 队列空
 */
bool mqtt_service_take_cloud_cmd(uint8_t *dev_id, uint8_t *action);

#ifdef __cplusplus
}
#endif
