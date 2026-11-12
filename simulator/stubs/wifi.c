/**
 * @file wifi.c — PC 桩: 固定的扫描结果 + 已连接状态
 */
#include "wifi.h"
#include <string.h>
#include <stdio.h>

static wifi_state_t s_state = WIFI_STATE_CONNECTED;
static bool s_scanning;

static const wifi_ap_info_t s_aps[] = {
    { "Home-5G",        -48, 3 },
    { "Home-2.4G",      -66, 4 },
    { "Xiaomi_AX3000",  -74, 4 },
    { "IoT-Lab",        -85, 0 },
};
#define AP_N (int)(sizeof(s_aps)/sizeof(s_aps[0]))

esp_err_t wifi_init(void) { return ESP_OK; }
esp_err_t wifi_connect_sta(const char *ssid, const char *password, bool auto_save)
{
    (void)password; (void)auto_save;
    printf("[WIFI-STUB] connect %s\n", ssid ? ssid : "(null)");
    s_state = WIFI_STATE_CONNECTED;
    return ESP_OK;
}
esp_err_t wifi_connect_from_nvs(void) { s_state = WIFI_STATE_CONNECTED; return ESP_OK; }
esp_err_t wifi_disconnect(void) { s_state = WIFI_STATE_DISCONNECTED; return ESP_OK; }
wifi_state_t wifi_get_state(void) { return s_state; }
bool wifi_is_connected(void) { return s_state == WIFI_STATE_CONNECTED; }
bool wifi_get_ip_info(char *ip, char *gw, char *netmask, size_t buf_size)
{
    if (ip && buf_size >= 12) strncpy(ip, "192.168.1.42", buf_size);
    if (gw && buf_size >= 12) strncpy(gw, "192.168.1.1", buf_size);
    return s_state == WIFI_STATE_CONNECTED;
}
esp_err_t wifi_scan_start(void) { s_scanning = true; return ESP_OK; }
bool wifi_scan_in_progress(void) { return false; }   /* 桩：立即完成 */
int  wifi_scan_result_count(void) { s_scanning = false; return AP_N; }
bool wifi_scan_result_get(int index, wifi_ap_info_t *out)
{
    if (index < 0 || index >= AP_N || !out) return false;
    *out = s_aps[index];
    return true;
}
