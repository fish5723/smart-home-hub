/**
 * @file remote_node.c
 * @brief 远端节点状态订阅（见 remote_node.h）
 */
#include "remote_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "sdkconfig.h"

#ifndef CONFIG_SMART_HOME_REMOTE_NODE_ID
#define CONFIG_SMART_HOME_REMOTE_NODE_ID "c3node"
#endif

static char s_topic_avail[96];
static char s_topic_state[96];   /* 前缀：smart_home/<node>/state/ */

static struct {
    bool    ever_seen;
    bool    online;
    bool    has_rssi;
    int     rssi;
    int64_t last_us;
} s_state;

static void build_topics(void)
{
    if (s_topic_avail[0] != '\0') {
        return;
    }
    snprintf(s_topic_avail, sizeof(s_topic_avail), "smart_home/%s/availability",
             CONFIG_SMART_HOME_REMOTE_NODE_ID);
    snprintf(s_topic_state, sizeof(s_topic_state), "smart_home/%s/state/",
             CONFIG_SMART_HOME_REMOTE_NODE_ID);
}

void remote_node_subscribe(esp_mqtt_client_handle_t client)
{
    if (client == NULL) {
        return;
    }
    build_topics();

    esp_mqtt_client_subscribe(client, s_topic_avail, 1);

    char wild[104];
    snprintf(wild, sizeof(wild), "%s+", s_topic_state);
    esp_mqtt_client_subscribe(client, wild, 1);
}

/* 从 {"v":-45} 里取出数值。
 * data 是 MQTT 事件负载，长度由 data_len 给出、不保证 NUL 结尾，
 * 所以不能直接 strstr —— 一律按 len 做边界。 */
static bool parse_value(const char *data, int len, int *out)
{
    int i = 0;
    for (; i + 3 < len; i++) {
        if (data[i] == '"' && data[i + 1] == 'v' && data[i + 2] == '"' && data[i + 3] == ':') {
            break;
        }
    }
    if (i + 3 >= len) {
        return false;
    }

    i += 4;   /* 跳过 "v":，紧接着就是数字 */
    char num[16];
    int n = 0;
    while (i < len && n < 15 &&
           (data[i] == '-' || data[i] == '+' || (data[i] >= '0' && data[i] <= '9'))) {
        num[n++] = data[i++];
    }
    if (n == 0) {
        return false;
    }
    num[n] = '\0';
    *out = atoi(num);
    return true;
}

bool remote_node_on_message(const char *topic, const char *data, int len)
{
    if (topic == NULL || data == NULL || len <= 0) {
        return false;
    }
    build_topics();

    if (strcmp(topic, s_topic_avail) == 0) {
        s_state.ever_seen = true;
        s_state.online = (len == 6 && memcmp(data, "online", 6) == 0);
        s_state.last_us = esp_timer_get_time();
        return true;
    }

    const size_t pl = strlen(s_topic_state);
    if (strncmp(topic, s_topic_state, pl) != 0) {
        return false;
    }

    s_state.ever_seen = true;
    s_state.last_us = esp_timer_get_time();

    const char *obj = topic + pl;
    int v = 0;
    if (strcmp(obj, "rssi") == 0 && parse_value(data, len, &v)) {
        s_state.rssi = v;
        s_state.has_rssi = true;
    }
    return true;
}

bool remote_node_get(remote_node_status_t *out)
{
    if (out == NULL || !s_state.ever_seen) {
        return false;
    }
    out->has_rssi = s_state.has_rssi;
    out->rssi = s_state.rssi;
    /* 在线 + 有数据：availability 为 online，且最近 90s 内有任何消息 */
    const int64_t age_ms = (esp_timer_get_time() - s_state.last_us) / 1000;
    out->online = s_state.online && age_ms < REMOTE_NODE_STALE_MS;
    return true;
}
