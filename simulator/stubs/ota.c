/* UI-only simulator: OTA has no hardware or network backend. */
#include "ota/net_ota.h"
#include "ota/f103_iap.h"

esp_err_t net_ota_check_now(void) { return ESP_FAIL; }
net_ota_status_t net_ota_get_status(void) { return NET_OTA_OFFLINE; }
int net_ota_get_progress(void) { return 0; }
bool net_ota_is_busy(void) { return false; }

esp_err_t f103_iap_start(void) { return ESP_FAIL; }
f103_iap_status_t f103_iap_get_status(void) { return F103_IAP_IDLE; }
int f103_iap_get_progress(void) { return 0; }
bool f103_iap_is_running(void) { return false; }
