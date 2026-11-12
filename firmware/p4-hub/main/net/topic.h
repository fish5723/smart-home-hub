/**
 * @file topic.h
 * @brief MQTT 主题命名（阶段 5）
 *
 * 主题树（base = smart_home/<node_id>）：
 *   homeassistant/status                              ← 订阅（HA 重启广播 "online"）
 *   homeassistant/<component>/<node_id>/<obj_id>/config  → 发布（retain）
 *   <base>/availability                               → 发布（retain，LWT 遗嘱）
 *   <base>/state/<obj_id>                             → 发布
 *   <base>/cmd/<obj_id>                               ← 订阅
 *
 * 所有函数返回 snprintf 写入长度；返回 <0 表示缓冲区不足。
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HA 自动发现前缀（HA 约定，固定） */
#define TOPIC_HA_DISCOVERY_PREFIX "homeassistant"
/** HA 上线时向该主题广播 "online"，用于触发重发 discovery */
#define TOPIC_HA_STATUS           "homeassistant/status"

/** 设置节点 ID（来自 Kconfig/NVS）；NULL 或空串回退为 "p4hub" */
void topic_set_node_id(const char *node_id);
const char *topic_node_id(void);

/** <base>/availability */
int topic_availability(char *buf, size_t n);
/** <base>/device（设备属性快照，retain） */
int topic_device_info(char *buf, size_t n);
/** <base>/ota/result（OTA 检查结果事件） */
int topic_ota_result(char *buf, size_t n);
/** <base>/state/<obj_id> */
int topic_state(char *buf, size_t n, const char *obj_id);
/** <base>/cmd/<obj_id> */
int topic_cmd(char *buf, size_t n, const char *obj_id);
/** <base>/cmd/ （订阅前缀，用于解析） */
int topic_cmd_prefix(char *buf, size_t n);
/** <base>/cmd/+ （订阅通配） */
int topic_cmd_wildcard(char *buf, size_t n);
/** homeassistant/<component>/<node_id>/<obj_id>/config */
int topic_discovery(char *buf, size_t n, const char *component, const char *obj_id);

#ifdef __cplusplus
}
#endif
