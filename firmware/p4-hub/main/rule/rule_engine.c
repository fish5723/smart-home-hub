/**
 * @file rule_engine.c
 * @brief Portable rule evaluator and fixed-layout blob codec.
 */
#include "rule_engine.h"

#include <string.h>
#include <math.h>

static int cond_compare(float value, uint8_t cmp, float threshold)
{
    switch (cmp) {
    case RULE_CMP_LT: return value < threshold;
    case RULE_CMP_LE: return value <= threshold;
    case RULE_CMP_GT: return value > threshold;
    case RULE_CMP_GE: return value >= threshold;
    default: return 0;
    }
}

static int rule_conditions_met(const rule_t *r, bool link_up,
                               rule_sensor_fn get)
{
    if (r->cond_count == 0 || r->cond_count > RULE_MAX_CONDS) return 0;
    for (uint8_t i = 0; i < r->cond_count; i++) {
        const rule_cond_t *c = &r->conds[i];
        float value;
        uint8_t quality = 0xff;
        if (c->src == RULE_SRC_LINK) {
            value = link_up ? 1.0f : 0.0f;
        } else if (c->src == RULE_SRC_ALWAYS) {
            value = 1.0f;
        } else if (c->src == RULE_SRC_SENSOR && get) {
            value = get(c->dev_id, &quality);
            if (c->quality_req && quality != 0U) return 0;
            if (!isfinite(value)) return 0;
        } else {
            return 0;
        }
        if (!cond_compare(value, c->cmp, c->threshold)) return 0;
    }
    return 1;
}

void rule_engine_clear(rule_set_t *set)
{
    if (set) memset(set, 0, sizeof(*set));
}

void rule_engine_reset_runtime(rule_set_t *set)
{
    if (!set) return;
    for (uint8_t i = 0; i < RULE_SET_MAX_RULES; i++) {
        set->rules[i].cond_met = 0;
        set->rules[i].cond_since_us = 0;
        set->rules[i].last_fire_us = 0;
    }
}

int rule_engine_tick(rule_set_t *set, bool link_up, int64_t now_us,
                     rule_sensor_fn get, rule_cmd_fn cmd)
{
    if (!set || !link_up) return 0;
    int fired = 0;
    /* ponytail: max eight rules; insertion sort is smaller than a queue here. */
    uint8_t order[RULE_SET_MAX_RULES];
    uint8_t count = set->count > RULE_SET_MAX_RULES ? RULE_SET_MAX_RULES : set->count;
    for (uint8_t i = 0; i < count; i++) order[i] = i;
    for (uint8_t i = 1; i < count; i++) {
        uint8_t k = order[i], j = i;
        while (j > 0 && set->rules[order[j - 1]].priority < set->rules[k].priority) {
            order[j] = order[j - 1]; j--;
        }
        order[j] = k;
    }
    for (uint8_t oi = 0; oi < count; oi++) {
        rule_t *r = &set->rules[order[oi]];
        if (!r->id || !r->enabled || r->act_count > RULE_MAX_ACTS) continue;
        int met = rule_conditions_met(r, link_up, get);
        if (!met) { r->cond_met = 0; r->cond_since_us = 0; continue; }
        /* cond_met=2 means this edge already fired; wait for a false edge. */
        if (r->cond_met == 2) continue;
        if (!r->cond_met) {
            r->cond_met = 1;
            r->cond_since_us = now_us;
        }
        if (r->debounce_ms && now_us - r->cond_since_us < (int64_t)r->debounce_ms * 1000) continue;
        if (r->last_fire_us && r->cooldown_ms &&
            now_us - r->last_fire_us < (int64_t)r->cooldown_ms * 1000) continue;
        for (uint8_t a = 0; a < r->act_count; a++) {
            if (cmd) cmd(r->acts[a].dev_id, r->acts[a].action, r->acts[a].ttl_ms);
            fired++;
        }
        r->last_fire_us = now_us;
        r->cond_met = 2;
    }
    return fired;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffUL;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320UL & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t crc;
    rule_set_t set;
} rule_blob_t;

size_t rule_set_blob_size(void) { return sizeof(rule_blob_t); }

size_t rule_set_serialize(const rule_set_t *set, uint8_t *buf, size_t cap)
{
    if (!set || !buf || cap < sizeof(rule_blob_t)) return 0;
    rule_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = RULE_BLOB_MAGIC;
    b.version = (uint16_t)RULE_BLOB_VERSION;
    b.set = *set;
    rule_engine_reset_runtime(&b.set);
    b.crc = crc32((const uint8_t *)&b.set, sizeof(b.set));
    memcpy(buf, &b, sizeof(b));
    return sizeof(b);
}

bool rule_set_deserialize(const uint8_t *buf, size_t len, rule_set_t *out)
{
    if (!buf || !out || len != sizeof(rule_blob_t)) return false;
    rule_blob_t b;
    memcpy(&b, buf, sizeof(b));
    if (b.magic != RULE_BLOB_MAGIC || b.version != RULE_BLOB_VERSION ||
        b.set.count > RULE_SET_MAX_RULES ||
        b.crc != crc32((const uint8_t *)&b.set, sizeof(b.set))) return false;
    for (uint8_t i = 0; i < b.set.count; i++) {
        if (b.set.rules[i].cond_count > RULE_MAX_CONDS ||
            b.set.rules[i].act_count > RULE_MAX_ACTS) return false;
    }
    rule_engine_reset_runtime(&b.set);
    *out = b.set;
    return true;
}
