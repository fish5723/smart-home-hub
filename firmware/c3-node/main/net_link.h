/**
 * @file net_link.h
 * @brief 阶段 6：Wi-Fi STA + MQTT（复用阶段 5 的 broker 与主题风格）
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define NET_LINK_SSID_MAX 32
#define NET_LINK_PASS_MAX 64

/* 配网状态：与 BLE 0xFFF6 的第一字节一一对应，改动必须同步 docs/18 §4.2 */
typedef enum
{
    NET_LINK_IDLE = 0,       /* 无凭据，未连网 */
    NET_LINK_CONNECTING = 1, /* 正在关联/认证 */
    NET_LINK_CONNECTED = 2,  /* 已拿到 IP */
    NET_LINK_FAILED = 3,     /* 连续失败，停止重试 */
} net_link_state_t;

typedef struct
{
    uint32_t wifi_disconnects;   /* Wi-Fi 断线累计次数 */
    uint32_t mqtt_disconnects;   /* MQTT 断线累计次数 */
    uint32_t mqtt_connects;      /* MQTT 连接成功累计次数 */
    uint8_t  mqtt_up;            /* 当前 MQTT 是否在线 */
    int8_t   rssi;               /* 最近一次 AP RSSI */
} net_link_stats_t;

/**
 * 启动网络。凭据优先取 NVS（由 BLE 配网写入），其次取 Kconfig 默认值。
 * 两者都没有时只做 BLE，不启 Wi-Fi（返回 ESP_ERR_INVALID_STATE）。
 */
esp_err_t net_link_init(void);

/**
 * 保存新凭据到 NVS 并立即用它重连。首次调用时会把网络栈初始化起来，
 * 因此无凭据开机的设备也能被 BLE 配网救活。
 */
esp_err_t net_link_set_credentials(const char *ssid, const char *pass);

/** 当前配网/连接状态；reason 为最近一次 Wi-Fi 断开原因码（无则填 0） */
uint8_t net_link_get_state(uint8_t *reason);

/**
 * 上行一帧。JSON 保留阶段五的可读字段，并附 "frame" 为阶段二帧的十六进制，
 * 保证 BLE 与 MQTT 携带同一份字节、同一套 CRC。
 */
void net_link_publish(uint16_t seq, uint32_t uptime_s,
                      const uint8_t *frame, uint16_t frame_len);

void net_link_get_stats(net_link_stats_t *out);
