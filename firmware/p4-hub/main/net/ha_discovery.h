/**
 * @file ha_discovery.h
 * @brief Home Assistant MQTT Discovery 发布（阶段 5）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 为全部启用的实体发布 discovery 配置（retain, qos1）
 *
 * 主题：homeassistant/<component>/<node_id>/<obj_id>/config
 * 在 MQTT 连接建立时、以及收到 homeassistant/status=online 时调用。
 *
 * @return 实际发布的实体条数
 */
int ha_discovery_publish_all(void);

#ifdef __cplusplus
}
#endif
