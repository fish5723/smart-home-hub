/**
 * @file ble_svc.h
 * @brief 阶段 6：BLE GATT Server（NimBLE 外设）
 *
 * 服务/特征（16 位自定义 UUID）：
 *   FFF0 主服务    FFF1 Notify+Read 链路/遥测载荷
 *                  FFF2 Write       控制（非 0 点亮板载 LED）
 *   FFF3 配网服务  FFF4 Write       文本：`ssid=xxx` 或 `pass=xxx`
 *                  FFF5 Write       文本 `1` 提交（落 NVS 并重连）
 *                  FFF6 Read+Notify 2 字节：status, reason
 *
 * 配网服务要求加密链路（LE Secure Connections + bonding，Just Works）。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define BLE_SVC_NAME "SHH-C3"

typedef struct
{
    uint32_t conn_count;      /* 累计连接次数 */
    uint32_t disc_count;      /* 累计断开次数 */
    uint32_t notify_fail;     /* Notify 入队失败次数 */
    uint16_t mtu;             /* 最近一次协商的 MTU */
    uint8_t  connected;       /* 当前是否已连接 */
    uint8_t  subscribed;      /* 当前是否已订阅 Notify */
    uint8_t  led_on;
} ble_svc_stats_t;

esp_err_t ble_svc_init(void);

/**
 * 更新 FFF1 的 Read 值并在已订阅时发一次 Notify。
 * data 为完整的阶段二帧（frame_build 产出），BLE 与 MQTT 共用同一份字节。
 */
void ble_svc_notify_frame(const uint8_t *data, uint16_t len);

/** 配网状态变化时推送 2 字节（status 与 net_link_state_t 同值，reason 为断开原因码） */
void ble_svc_prov_notify(uint8_t status, uint8_t reason);

void ble_svc_get_stats(ble_svc_stats_t *out);
