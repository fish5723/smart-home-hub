/**
 * @file topic.c
 * @brief MQTT 主题命名实现（见 topic.h）
 */
#include "topic.h"
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_SMART_HOME_MQTT_NODE_ID
#define CONFIG_SMART_HOME_MQTT_NODE_ID "p4hub"
#endif

#define BASE_PREFIX "smart_home"

static char s_node_id[33] = CONFIG_SMART_HOME_MQTT_NODE_ID;

void topic_set_node_id(const char *node_id)
{
    if (node_id && node_id[0]) {
        strncpy(s_node_id, node_id, sizeof(s_node_id) - 1);
        s_node_id[sizeof(s_node_id) - 1] = '\0';
    } else {
        strncpy(s_node_id, "p4hub", sizeof(s_node_id) - 1);
        s_node_id[sizeof(s_node_id) - 1] = '\0';
    }
}

const char *topic_node_id(void)
{
    return s_node_id;
}

int topic_availability(char *buf, size_t n)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/availability", s_node_id);
}

int topic_device_info(char *buf, size_t n)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/device", s_node_id);
}

int topic_ota_result(char *buf, size_t n)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/ota/result", s_node_id);
}

int topic_state(char *buf, size_t n, const char *obj_id)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/state/%s", s_node_id, obj_id);
}

int topic_cmd(char *buf, size_t n, const char *obj_id)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/cmd/%s", s_node_id, obj_id);
}

int topic_cmd_prefix(char *buf, size_t n)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/cmd/", s_node_id);
}

int topic_cmd_wildcard(char *buf, size_t n)
{
    return snprintf(buf, n, BASE_PREFIX "/%s/cmd/+", s_node_id);
}

int topic_discovery(char *buf, size_t n, const char *component, const char *obj_id)
{
    return snprintf(buf, n, TOPIC_HA_DISCOVERY_PREFIX "/%s/%s/%s/config",
                    component, s_node_id, obj_id);
}
