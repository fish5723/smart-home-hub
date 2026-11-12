/**
 * @file storage.h — PC 桩: 与固件 bsp/storage/storage.h 同接口
 * 模拟器用内存表替代 NVS（键值只在本次进程内有效）。
 * 真机构建不走本文件（include 顺序: stubs 目录只在模拟器 CMake 里）。
 */
#ifndef STORAGE_H_STUB
#define STORAGE_H_STUB

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MAX_STR_LEN 128

esp_err_t storage_init(void);
esp_err_t storage_get_str(const char *key, char *buf, size_t *len);
esp_err_t storage_set_str(const char *key, const char *val);
esp_err_t storage_erase_key(const char *key);
esp_err_t storage_get_bool(const char *key, bool *val);
esp_err_t storage_set_bool(const char *key, bool val);
esp_err_t storage_get_u32(const char *key, uint32_t *val);
esp_err_t storage_set_u32(const char *key, uint32_t val);
esp_err_t storage_save_wifi_cred(const char *ssid, const char *pwd);
esp_err_t storage_load_wifi_cred(char *ssid_buf, size_t ssid_len, char *pwd_buf, size_t pwd_len);
esp_err_t storage_erase_wifi_cred(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H_STUB */
