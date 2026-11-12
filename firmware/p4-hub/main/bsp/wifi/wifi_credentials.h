#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ESP-IDF's STA fields are fixed-size byte arrays, not C strings at capacity. */
static inline bool wifi_prepare_credentials(const char *ssid, const char *password,
                                             uint8_t out_ssid[32], uint8_t out_password[64],
                                             bool *secured)
{
    if (!ssid || !out_ssid || !out_password || !secured) return false;
    size_t ssid_len = strlen(ssid);
    size_t pwd_len = password ? strlen(password) : 0;
    if (ssid_len == 0 || ssid_len > 32 ||
        (pwd_len != 0 && (pwd_len < 8 || pwd_len > 64))) return false;
    if (pwd_len == 64) {
        for (size_t i = 0; i < pwd_len; i++) {
            char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
    }
    memset(out_ssid, 0, 32);
    memset(out_password, 0, 64);
    memcpy(out_ssid, ssid, ssid_len);
    if (pwd_len) memcpy(out_password, password, pwd_len);
    *secured = pwd_len != 0;
    return true;
}
