/**
 * @file wifi.h — PC 桩: 与固件 bsp/wifi/wifi.h 同接口
 * 模拟器给出"已连接 Home-5G + 4 个扫描结果"的固定场景，便于在 PC 上截图自检
 * 网络下级界面；真机构建不走本文件。
 */
#ifndef WIFI_H_STUB
#define WIFI_H_STUB

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR,
} wifi_state_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode;
} wifi_ap_info_t;

#define WIFI_SCAN_MAX_AP 16

esp_err_t wifi_init(void);
esp_err_t wifi_connect_sta(const char *ssid, const char *password, bool auto_save);
esp_err_t wifi_connect_from_nvs(void);
esp_err_t wifi_disconnect(void);
wifi_state_t wifi_get_state(void);
bool wifi_is_connected(void);
bool wifi_get_ip_info(char *ip, char *gw, char *netmask, size_t buf_size);
esp_err_t wifi_scan_start(void);
bool wifi_scan_in_progress(void);
int  wifi_scan_result_count(void);
bool wifi_scan_result_get(int index, wifi_ap_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H_STUB */
