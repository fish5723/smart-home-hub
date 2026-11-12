#include "ui_device_model.h"
#include "ui_tokens.h"
#include "app_proto.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t id;
    const char *name;
    ui_room_t room;
    bool actuator;
    uint32_t color;
} device_def_t;

/* ponytail: one flat registry keeps adding a device a data-only change. */
static const device_def_t k_registry[] = {
    { PROTO_DEV_RELAY1,     "Living Light",   UI_ROOM_LIVING,  true,  TK_COLOR_ACCENT },
    { PROTO_DEV_RELAY2,     "Floor Lamp",     UI_ROOM_LIVING,  true,  TK_COLOR_ACCENT_WARM },
    { PROTO_DEV_DHT11_TEMP, "AC",              UI_ROOM_LIVING,  false, TK_COLOR_ACCENT_BLUE },
    { PROTO_DEV_DHT11_HUM,  "Humidifier",     UI_ROOM_BEDROOM, false, TK_COLOR_ACCENT_BLUE },
    { PROTO_DEV_SOIL,       "Kitchen Socket",  UI_ROOM_KITCHEN, true,  TK_COLOR_MUTED_FG },
};

static const char *sensor_state(uint8_t id, float value, uint8_t quality,
                                char *buf, size_t len)
{
    if (quality != PROTO_QUAL_VALID) return "Offline";
    if (id == PROTO_DEV_DHT11_TEMP) snprintf(buf, len, "%.1f C", (double)value);
    else if (id == PROTO_DEV_DHT11_HUM) snprintf(buf, len, "%.0f%%", (double)value);
    else snprintf(buf, len, "%.1f", (double)value);
    return buf;
}

int ui_device_model_snapshot(ui_device_t *out, int max_entries)
{
    if (!out || max_entries <= 0) return 0;
    sensor_shadow_entry_t sensors[SHADOW_MAX_SENSORS];
    actuator_shadow_entry_t acts[8];
    int sensor_count = AppProto_GetSensorShadow(sensors, SHADOW_MAX_SENSORS);
    int act_count = AppProto_GetActuatorShadow(acts, 8);
    link_state_t link = AppProto_GetLinkState();
    int n = (int)(sizeof(k_registry) / sizeof(k_registry[0]));
    if (n > max_entries) n = max_entries;

    for (int i = 0; i < n; i++) {
        const device_def_t *def = &k_registry[i];
        ui_device_t *dst = &out[i];
        memset(dst, 0, sizeof(*dst));
        dst->id = def->id;
        dst->name = def->name;
        dst->room = def->room;
        dst->actuator = def->actuator;
        dst->dot_color = def->color;
        dst->state = "Offline";
        dst->stale = (link != LINK_UP);

        if (def->actuator) {
            for (int j = 0; j < act_count; j++) {
                if (acts[j].dev_id == def->id) {
                    dst->on = acts[j].state != 0;
                    dst->state = dst->on ? "On" : "Off";
                    break;
                }
            }
        } else {
            for (int j = 0; j < sensor_count; j++) {
                if (sensors[j].dev_id == def->id) {
                    static char values[SHADOW_MAX_SENSORS][16];
                    dst->state = sensor_state(def->id, sensors[j].value,
                                               sensors[j].quality,
                                               values[i], sizeof(values[i]));
                    dst->stale |= sensors[j].quality != PROTO_QUAL_VALID;
                    break;
                }
            }
        }
        if (dst->stale) dst->dot_color = TK_COLOR_LINK_STALE;
    }
    return n;
}

