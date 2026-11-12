#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

typedef struct {
    char version[40];
    char url[192];
    char sha256[65];
    char sig[129];
    bool has_sig;
    uint32_t size;
} image_meta_t;

bool ota_parse_image_meta(const cJSON *root, const char *key, image_meta_t *out);
bool ota_version_is_newer(const char *remote, const char *current);
