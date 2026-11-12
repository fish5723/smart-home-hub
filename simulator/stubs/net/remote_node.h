/* UI simulator: no remote MQTT node is connected. */
#pragma once

#include <stdbool.h>

typedef struct {
    bool online;
    bool has_rssi;
    int rssi;
} remote_node_status_t;

static inline bool remote_node_get(remote_node_status_t *out)
{
    if (out) *out = (remote_node_status_t){0};
    return false;
}
