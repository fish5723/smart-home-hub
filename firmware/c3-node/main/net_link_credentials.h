#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ESP-IDF accepts an open AP, an 8–63 byte WPA passphrase, or a 64-hex PSK. */
static inline bool net_link_credentials_valid(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return false;
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > 32 ||
        (pass_len != 0 && (pass_len < 8 || pass_len > 64))) return false;
    if (pass_len == 64) {
        for (size_t i = 0; i < pass_len; i++) {
            char c = pass[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
    }
    return true;
}
