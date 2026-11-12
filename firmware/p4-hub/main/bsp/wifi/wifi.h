/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi.h
 * @brief WiFi 模块（ESP32-P4 + ESP32-C6 SDIO esp_hosted）
 *
 * 架构说明：
 * - ESP32-P4 无射频，通过 SDIO Slot 1 连接板载 ESP32-C6
 * - C6 运行 esp_hosted slave 固件，P4 侧用 esp_wifi_remote 透明转发 WiFi API
 * - 应用层调用标准 esp_wifi_*() API，与普通 ESP32 无异
 *
 * 设计约束（继承自前序工程）：
 * 1. SD 卡（Slot 0）必须在 WiFi（Slot 1）之前初始化 — 共享时钟分频器
 * 2. CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE 设为 n — 防 boot loop
 * 3. esp_hosted 锁定 2.9.6 — 2.12.9 会必现 SDIO 258 错误
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi 连接状态 */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,    /**< 未连接 */
    WIFI_STATE_CONNECTING,          /**< 正在连接 */
    WIFI_STATE_CONNECTED,           /**< 已连接 */
    WIFI_STATE_ERROR,               /**< 错误状态 */
} wifi_state_t;

/** WiFi 状态回调 */
typedef void (*wifi_state_cb_t)(wifi_state_t state, void *user_data);

/**
 * @brief 初始化 WiFi（初始化 esp_hosted SDIO + esp_wifi_remote）
 *
 * - 初始化 esp_netif + 默认事件循环（若未初始化）
 * - 调用 esp_wifi_remote_init() 建立与 C6 的 SDIO 通信
 * - 注册 WiFi 事件处理器
 *
 * @note 必须在 SD 卡初始化之后调用（Slot 0 与 Slot 1 共享时钟分频器）
 * @note 非致命 — 失败仅打日志，不阻塞启动
 * @return ESP_OK 成功 / 其他 失败
 */
esp_err_t wifi_init(void);

/**
 * @brief 连接 WiFi（STA 模式）
 *
 * - 设置 SSID/密码到 WiFi 配置
 * - 调用 esp_wifi_connect()
 * - 事件处理器会自动处理重连
 *
 * @param ssid      AP SSID
 * @param password  AP 密码（可为空字符串或 NULL 表示开放网络）
 * @param auto_save 是否将凭证持久化到 NVS
 * @return ESP_OK 成功 / 其他 失败
 */
esp_err_t wifi_connect_sta(const char *ssid, const char *password, bool auto_save);

/**
 * @brief 从 NVS 读取保存的凭证并自动连接
 * @return ESP_OK 已发起连接 / ESP_ERR_NOT_FOUND 无保存凭证
 */
esp_err_t wifi_connect_from_nvs(void);

/**
 * @brief 断开 WiFi 连接
 * @return ESP_OK 成功
 */
esp_err_t wifi_disconnect(void);

/**
 * @brief 获取当前连接状态
 * @return wifi_state_t
 */
wifi_state_t wifi_get_state(void);

/**
 * @brief 查询 WiFi 是否已连接（用于 SD 卡守卫检查）
 * @return true 已连接 / false 未连接
 */
bool wifi_is_connected(void);

/**
 * @brief 获取当前连接的 IP 信息
 * @param[out] ip       IP 地址字符串缓冲区（>= 16 字节），可为 NULL
 * @param[out] gw       网关地址字符串缓冲区（>= 16 字节），可为 NULL
 * @param[out] netmask  子网掩码字符串缓冲区（>= 16 字节），可为 NULL
 * @param[out] buf_size 缓冲区大小
 * @return true 有 IP / false 无 IP
 */
bool wifi_get_ip_info(char *ip, char *gw, char *netmask, size_t buf_size);

/**
 * @brief 注册 WiFi 状态变更回调
 * @param cb    回调函数
 * @param user_data 用户数据指针
 */
void wifi_register_callback(wifi_state_cb_t cb, void *user_data);

/**
 * @brief 注销回调
 */
void wifi_unregister_callback(void);

/* ─── 异步扫描 ─── */

/** 单个扫描到的 AP */
typedef struct {
    char    ssid[33];    /* 需保证 NUL 结尾 */
    int8_t  rssi;        /* dBm */
    uint8_t authmode;    /* wifi_auth_mode_t; 0 = OPEN */
} wifi_ap_info_t;

#define WIFI_SCAN_MAX_AP 16

/** 触发一次异步扫描（非阻塞）。返回 ESP_OK 表示已发起；
 *  ESP_ERR_INVALID_STATE 表示上一次扫描尚未完成。 */
esp_err_t wifi_scan_start(void);

/** 是否有扫描正在进行 */
bool wifi_scan_in_progress(void);

/** 最近一次扫描结果条数（0 = 无结果 / 从未扫描） */
int  wifi_scan_result_count(void);

/** 按 RSSI 从强到弱取第 index 条；越界返回 false */
bool wifi_scan_result_get(int index, wifi_ap_info_t *out);

#ifdef __cplusplus
}
#endif