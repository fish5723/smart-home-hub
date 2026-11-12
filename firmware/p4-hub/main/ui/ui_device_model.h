#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_ROOM_LIVING = 0,
    UI_ROOM_BEDROOM,
    UI_ROOM_KITCHEN,
} ui_room_t;

typedef struct {
    uint8_t id;
    const char *name;
    const char *state;
    uint32_t dot_color;
    ui_room_t room;
    bool actuator;
    bool on;
    bool stale;
} ui_device_t;

/* The registry is the only device-specific table the UI needs. */
int ui_device_model_snapshot(ui_device_t *out, int max_entries);

