#include <assert.h>
#include <math.h>
#include <string.h>
#include "../../firmware/p4-hub/main/rule/rule_engine.h"

static float sensor(uint8_t id, uint8_t *q) { (void)id; *q = 0; return 20.0f; }
static int commands;
static void command(uint8_t d, uint8_t a, uint16_t t) { (void)d; (void)a; (void)t; commands++; }

int main(void)
{
    rule_set_t s, loaded;
    uint8_t blob[4096];
    rule_engine_clear(&s);
    s.count = 1; s.rules[0].id = 1; s.rules[0].enabled = 1;
    s.rules[0].cond_count = 1; s.rules[0].act_count = 1;
    s.rules[0].conds[0].src = RULE_SRC_SENSOR;
    s.rules[0].conds[0].cmp = RULE_CMP_LT;
    s.rules[0].conds[0].quality_req = 1;
    s.rules[0].conds[0].threshold = 30;
    s.rules[0].debounce_ms = 100;
    s.rules[0].acts[0].dev_id = 1;
    assert(rule_engine_tick(&s, true, 0, sensor, command) == 0);
    assert(rule_engine_tick(&s, true, 100000, sensor, command) == 1);
    assert(rule_engine_tick(&s, true, 200000, sensor, command) == 0);
    assert(rule_engine_tick(&s, false, 300000, sensor, command) == 0);
    size_t n = rule_set_serialize(&s, blob, sizeof(blob));
    assert(n == rule_set_blob_size());
    assert(rule_set_deserialize(blob, n, &loaded));
    blob[n - 1] ^= 1;
    assert(!rule_set_deserialize(blob, n, &loaded));
    return 0;
}
