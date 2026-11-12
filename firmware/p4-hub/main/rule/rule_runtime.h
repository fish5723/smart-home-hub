#ifndef RULE_RUNTIME_H
#define RULE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Runtime bridge: NVS-backed rules + protocol sensor/actuator shadows. */
esp_err_t rule_runtime_init(void);
void rule_runtime_tick(void);
bool rule_runtime_get_enabled(uint8_t index);
esp_err_t rule_runtime_set_enabled(uint8_t index, bool enabled);
bool rule_runtime_get_scene_enabled(uint8_t scene);
esp_err_t rule_runtime_set_scene_enabled(uint8_t scene, bool enabled);
/* Returns a short ASCII status suitable for the existing automation header. */
void rule_runtime_get_status(char *out, size_t out_size);

#endif
