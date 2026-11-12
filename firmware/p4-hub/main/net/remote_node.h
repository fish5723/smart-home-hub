/**
 * @file remote_node.h
 * @brief 远端 MQTT 节点（阶段 6 的 C3）状态订阅与离线判定
 *
 * 只订阅 docs/17 约定内的两个入口：
 *   smart_home/<node>/availability   online / offline（retain，LWT 写入）
 *   smart_home/<node>/state/+        {"v": ...}（retain，QoS1）
 *
 * 离线判定同时看 availability 与"最近一次消息时间"，避免"在线但没数据"时
 * 面板仍显示正常（等价于 HA 的 expire_after）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mqtt_client.h"

#define REMOTE_NODE_STALE_MS 90000  /* 90s 无任何消息视为离线（HA 用 60s） */

typedef struct
{
    bool online;
    bool has_rssi;
    int  rssi;           /* dBm，来自 state/rssi */
} remote_node_status_t;

/** 在 MQTT_EVENT_CONNECTED 中调用：订阅该节点的 availability 与 state/+ */
void remote_node_subscribe(esp_mqtt_client_handle_t client);

/**
 * 在 MQTT_EVENT_DATA 中调用。
 * 返回 true 表示消息属于远端节点，已被消费（调用方不要再当成本地 cmd 处理）。
 */
bool remote_node_on_message(const char *topic, const char *data, int len);

/** 读取节点状态；返回 false 表示从未收到该节点任何消息 */
bool remote_node_get(remote_node_status_t *out);
