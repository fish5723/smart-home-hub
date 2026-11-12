/**
 * @file storage.c — PC 桩: 内存键值表（替代 NVS）
 */
#include "storage.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define KV_MAX 16
typedef struct { char key[32]; char val[STORAGE_MAX_STR_LEN]; } kv_t;
static kv_t s_kv[KV_MAX];
static int  s_kv_n;

static const char *kv_find(const char *key)
{
    for (int i = 0; i < s_kv_n; i++) if (strcmp(s_kv[i].key, key) == 0) return s_kv[i].val;
    return NULL;
}

esp_err_t storage_set_str(const char *key, const char *val)
{
    for (int i = 0; i < s_kv_n; i++)
        if (strcmp(s_kv[i].key, key) == 0) { strncpy(s_kv[i].val, val, STORAGE_MAX_STR_LEN-1); s_kv[i].val[STORAGE_MAX_STR_LEN-1]='\0'; return ESP_OK; }
    if (s_kv_n >= KV_MAX) return ESP_FAIL;
    strncpy(s_kv[s_kv_n].key, key, sizeof(s_kv[s_kv_n].key)-1);
    strncpy(s_kv[s_kv_n].val, val, STORAGE_MAX_STR_LEN-1);
    s_kv[s_kv_n].val[STORAGE_MAX_STR_LEN-1] = '\0';
    s_kv_n++;
    return ESP_OK;
}

esp_err_t storage_get_str(const char *key, char *buf, size_t *len)
{
    const char *v = kv_find(key);
    if (!v) return ESP_ERR_NOT_FOUND;
    size_t n = strlen(v);
    if (*len <= n) return ESP_ERR_INVALID_SIZE;
    memcpy(buf, v, n + 1);
    *len = n;
    return ESP_OK;
}

esp_err_t storage_erase_key(const char *key)
{
    for (int i = 0; i < s_kv_n; i++)
        if (strcmp(s_kv[i].key, key) == 0) { s_kv[i] = s_kv[--s_kv_n]; return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t storage_set_bool(const char *key, bool val) { return storage_set_str(key, val ? "1" : "0"); }
esp_err_t storage_get_bool(const char *key, bool *val)
{
    const char *v = kv_find(key);
    if (!v) return ESP_ERR_NOT_FOUND;
    if (val) *val = (v[0] == '1');
    return ESP_OK;
}
esp_err_t storage_set_u32(const char *key, uint32_t val)
{
    char b[16]; snprintf(b, sizeof(b), "%u", (unsigned)val);
    return storage_set_str(key, b);
}
esp_err_t storage_get_u32(const char *key, uint32_t *val)
{
    const char *v = kv_find(key);
    if (!v) return ESP_ERR_NOT_FOUND;
    if (val) *val = (uint32_t)strtoul(v, NULL, 10);
    return ESP_OK;
}

esp_err_t storage_save_wifi_cred(const char *ssid, const char *pwd)
{
    storage_set_str("wifi_ssid", ssid ? ssid : "");
    storage_set_str("wifi_pwd", pwd ? pwd : "");
    return ESP_OK;
}
esp_err_t storage_load_wifi_cred(char *ssid_buf, size_t ssid_len, char *pwd_buf, size_t pwd_len)
{
    char buf[STORAGE_MAX_STR_LEN]; size_t len;
    len = sizeof(buf); if (storage_get_str("wifi_ssid", buf, &len) != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (ssid_len <= len) return ESP_ERR_INVALID_SIZE;
    memcpy(ssid_buf, buf, len + 1);
    if (pwd_buf) {
        len = sizeof(buf);
        if (storage_get_str("wifi_pwd", buf, &len) == ESP_OK && pwd_len > len) memcpy(pwd_buf, buf, len + 1);
        else if (pwd_buf) pwd_buf[0] = '\0';
    }
    return ESP_OK;
}
esp_err_t storage_erase_wifi_cred(void) { storage_erase_key("wifi_ssid"); storage_erase_key("wifi_pwd"); return ESP_OK; }
esp_err_t storage_init(void)
{
    /* 与原型一致：预置一张已保存的 Home-5G 凭证 */
    if (!kv_find("wifi_ssid")) storage_save_wifi_cred("Home-5G", "12345678");
    if (!kv_find("ui_idle_sec")) storage_set_u32("ui_idle_sec", 60);
    return ESP_OK;
}
