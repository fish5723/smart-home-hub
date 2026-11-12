#include "ota_manifest.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static bool parse_version(const char *s, uint32_t out[3])
{
    if (!s || !*s) return false;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        uint32_t value = 0;
        do {
            uint32_t digit = (uint32_t)(*s++ - '0');
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        } while (*s >= '0' && *s <= '9');
        out[i] = value;
        if (i < 2) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s == '\0') return true;
    if (*s++ != '-' || !*s) return false;
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') || *s == '-' || *s == '.')) return false;
    }
    return true;
}

bool ota_version_is_newer(const char *remote, const char *current)
{
    uint32_t a[3], b[3];
    if (!parse_version(remote, a) || !parse_version(current, b)) return false;
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

static bool is_hex_len(const char *s, size_t length)
{
    if (!s || strlen(s) != length) return false;
    for (size_t i = 0; i < length; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}

bool ota_parse_image_meta(const cJSON *root, const char *key, image_meta_t *out)
{
    if (!root || !key || !out) return false;
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(obj, "version");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(obj, "url");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(obj, "size");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(obj, "sha256");
    const cJSON *sig = cJSON_GetObjectItemCaseSensitive(obj, "sig");
    uint32_t parts[3];
    if (!cJSON_IsString(ver) || !cJSON_IsString(url) || !cJSON_IsNumber(size) ||
        !cJSON_IsString(sha) || (sig && !cJSON_IsString(sig)) ||
        !ver->valuestring || !url->valuestring || !sha->valuestring ||
        strlen(ver->valuestring) >= sizeof(out->version) ||
        strlen(url->valuestring) >= sizeof(out->url) ||
        !parse_version(ver->valuestring, parts) ||
        !(strncmp(url->valuestring, "https://", 8) == 0 ||
          strncmp(url->valuestring, "http://", 7) == 0) ||
        !is_hex_len(sha->valuestring, 64) ||
        !(size->valuedouble >= 1 && size->valuedouble <= UINT32_MAX) ||
        size->valuedouble != (double)(uint32_t)size->valuedouble ||
        (sig && !is_hex_len(sig->valuestring, 128))) return false;

    snprintf(out->version, sizeof(out->version), "%s", ver->valuestring);
    snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
    snprintf(out->sha256, sizeof(out->sha256), "%s", sha->valuestring);
    out->size = (uint32_t)size->valuedouble;
    out->has_sig = sig != NULL;
    if (sig) snprintf(out->sig, sizeof(out->sig), "%s", sig->valuestring);
    else out->sig[0] = '\0';
    return true;
}
