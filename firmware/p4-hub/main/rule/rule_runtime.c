#include "rule_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage.h"
#include "app_proto.h"
#include "protocol.h"
#include "rule_engine.h"

static const char *TAG = "RULES";
static rule_set_t s_set;
static nvs_handle_t s_nvs;
static uint8_t s_active_slot;
static int64_t s_last_tick_us;
static volatile uint32_t s_last_fired;
static volatile int64_t s_last_fire_us;

static float sensor_get(uint8_t dev_id, uint8_t *quality)
{
    sensor_shadow_entry_t sh[SHADOW_MAX_SENSORS];
    int n = AppProto_GetSensorShadow(sh, SHADOW_MAX_SENSORS);
    int64_t now = esp_timer_get_time();
    *quality = PROTO_QUAL_STALE;
    for (int i = 0; i < n; ++i) {
        if (sh[i].dev_id != dev_id) continue;
        if (sh[i].last_update_us <= 0 || now - sh[i].last_update_us > 3000000LL) {
            *quality = PROTO_QUAL_STALE;
            return NAN;
        }
        *quality = sh[i].quality;
        return sh[i].value;
    }
    return NAN;
}

static void send_command(uint8_t dev_id, uint8_t action, uint16_t ttl_ms)
{
    /* Lock is intentionally not part of the default rules: the installed
     * module is N/A and UI never sends dev=0x03. */
    if (dev_id != PROTO_DEV_RELAY1 && dev_id != PROTO_DEV_RELAY2 &&
        dev_id != PROTO_DEV_BEEPER) return;
    AppProto_SendActuatorCmd(dev_id, action, ttl_ms);
}

static void defaults(rule_set_t *set)
{
    rule_engine_clear(set);
    set->count = 5;

    set->rules[0].id = 1;
    set->rules[0].priority = 20;
    set->rules[0].cond_count = 1;
    set->rules[0].act_count = 1;
    set->rules[0].debounce_ms = 500;
    snprintf(set->rules[0].name, RULE_NAME_LEN, "Arrive light");
    set->rules[0].conds[0] = (rule_cond_t){RULE_SRC_SENSOR, PROTO_DEV_FACE,
                                           RULE_CMP_GE, 1, 1.0f};
    set->rules[0].acts[0] = (rule_act_t){PROTO_DEV_RELAY1, PROTO_ACTION_ON, 0, 0, 0};

    set->rules[1].id = 2;
    set->rules[1].priority = 19;
    set->rules[1].cond_count = 1;
    set->rules[1].act_count = 1;
    set->rules[1].debounce_ms = 1000;
    snprintf(set->rules[1].name, RULE_NAME_LEN, "Leave light");
    set->rules[1].conds[0] = (rule_cond_t){RULE_SRC_SENSOR, PROTO_DEV_HUMAN,
                                           RULE_CMP_LT, 1, 1.0f};
    set->rules[1].acts[0] = (rule_act_t){PROTO_DEV_RELAY1, PROTO_ACTION_OFF, 0, 0, 0};

    set->rules[2].id = 3;
    set->rules[2].priority = 30;
    set->rules[2].cond_count = 2;
    set->rules[2].act_count = 1;
    set->rules[2].debounce_ms = 2000;
    snprintf(set->rules[2].name, RULE_NAME_LEN, "Smart watering");
    set->rules[2].conds[0] = (rule_cond_t){RULE_SRC_SENSOR, PROTO_DEV_SOIL,
                                           RULE_CMP_LT, 1, 35.0f};
    set->rules[2].conds[1] = (rule_cond_t){RULE_SRC_SENSOR, PROTO_DEV_RAIN,
                                           RULE_CMP_LT, 1, 50.0f};
    set->rules[2].acts[0] = (rule_act_t){PROTO_DEV_RELAY2, PROTO_ACTION_ON, 0, 0, 6000};

    set->rules[3].id = 4;
    set->rules[3].priority = 31;
    set->rules[3].cond_count = 1;
    set->rules[3].act_count = 1;
    set->rules[3].debounce_ms = 500;
    snprintf(set->rules[3].name, RULE_NAME_LEN, "Rain stop");
    set->rules[3].conds[0] = (rule_cond_t){RULE_SRC_SENSOR, PROTO_DEV_RAIN,
                                           RULE_CMP_GE, 1, 50.0f};
    set->rules[3].acts[0] = (rule_act_t){PROTO_DEV_RELAY2, PROTO_ACTION_OFF, 0, 0, 0};

    set->rules[4].id = 5;
    set->rules[4].priority = 10;
    set->rules[4].cond_count = 1;
    set->rules[4].act_count = 1;
    snprintf(set->rules[4].name, RULE_NAME_LEN, "Quiet mode");
    set->rules[4].conds[0] = (rule_cond_t){RULE_SRC_ALWAYS, 0, RULE_CMP_GE, 0, 1.0f};
    set->rules[4].acts[0] = (rule_act_t){PROTO_DEV_BEEPER, PROTO_ACTION_OFF, 0, 0, 0};
    /* Safety: all defaults are opt-in until the user enables them in UI. */
    for (int i = 0; i < 5; ++i) set->rules[i].enabled = 0;
}

static bool load_slot(uint8_t slot, rule_set_t *out)
{
    size_t len = rule_set_blob_size();
    uint8_t *buf = malloc(len);
    if (!buf) return false;
    char key[8];
    snprintf(key, sizeof(key), "r%u", slot);
    esp_err_t err = nvs_get_blob(s_nvs, key, buf, &len);
    bool ok = err == ESP_OK && rule_set_deserialize(buf, len, out);
    free(buf);
    return ok;
}

static esp_err_t save_set(void)
{
    size_t len = rule_set_blob_size();
    uint8_t *buf = malloc(len);
    if (!buf) return ESP_ERR_NO_MEM;
    uint8_t next = (uint8_t)(s_active_slot ^ 1U);
    char key[8];
    snprintf(key, sizeof(key), "r%u", next);
    size_t written = rule_set_serialize(&s_set, buf, len);
    /* 只计 flash 相关消耗（malloc/序列化不计） */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = written ? nvs_set_blob(s_nvs, key, buf, written) : ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = nvs_set_u8(s_nvs, "active", next);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    int64_t dt_us = esp_timer_get_time() - t0;
    if (err == ESP_OK) s_active_slot = next;
    free(buf);
#if CONFIG_SMART_HOME_STANDBY_SELFTEST
    /* 计时打点：这次写入是在 LVGL 任务里同步完成的（UI 拨开关会直接调到这里）。
     * 若某一次触发 flash 擦除，会长时间占用 MSPI，面板该帧可能出杂色。
     * 用法：连续拨 N 次开关，「第 N 次闪」与日志第 N 条 save_set 直接对应。 */
    static uint32_t s_save_seq;
    ESP_LOGI(TAG, "save_set #%lu slot=%u %ldus blob=%uB%s",
             (unsigned long)++s_save_seq, (unsigned)next, (long)dt_us, (unsigned)written,
             err == ESP_OK ? "" : " FAILED");
#else
    (void)dt_us;
#endif
    return err;
}

static void migrate_face_gate(void)
{
    /* 2026-11-03：开灯条件由「PIR 有人」升级为「PIR 唤醒 + 摄像头人脸确认」。
     * 已持久化的旧默认规则 id=1 仍是 HUMAN>=1，不改写的话升级后依旧“有人就开灯”。
     * 只迁移这一条默认模板，其余用户规则不动。 */
    bool changed = false;
    int arrive = -1, leave = -1;
    for (int i = 0; i < s_set.count && i < RULE_SET_MAX_RULES; ++i) {
        rule_t *r = &s_set.rules[i];
        if (r->id == 1) arrive = i;
        if (r->id == 2) leave = i;
        if (r->id == 1 && r->cond_count >= 1 &&
            r->conds[0].src == RULE_SRC_SENSOR &&
            r->conds[0].dev_id == PROTO_DEV_HUMAN) {
            r->conds[0].dev_id = PROTO_DEV_FACE;
            changed = true;
        }
    }
    /* 旧固件 UI 只能启用 Arrive，Leave 永远是关的；升级后补齐配对，
     * 否则灯仍然“只开不关”。 */
    if (arrive >= 0 && leave >= 0 &&
        s_set.rules[arrive].enabled && !s_set.rules[leave].enabled) {
        s_set.rules[leave].enabled = 1;
        s_set.rules[leave].cond_met = 0;
        s_set.rules[leave].cond_since_us = 0;
        changed = true;
    }
    if (!changed) return;
    esp_err_t err = save_set();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "rule migration not persisted: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "rule migration: arrive=FACE, leave paired");
}

esp_err_t rule_runtime_init(void)
{
    /* 统一使用 storage 模块的 namespace（storage.h 约定：禁止各模块自建
     * namespace，2026-10-18 修正）。键名 r0/r1/active 与 storage 现有键不冲突。
     * 迁移说明：旧 "rules" namespace 中的规则数据不再被读取 —— 首次启动会
     * 重新写入本工程的默认规则（5 条，全部 enabled=0，需 UI 显式开启）。 */
    esp_err_t err = nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) return err;
    uint8_t active = 0;
    bool loaded = false;
    if (nvs_get_u8(s_nvs, "active", &active) == ESP_OK && active < 2)
        loaded = load_slot(active, &s_set);
    if (!loaded) loaded = load_slot((uint8_t)(active ^ 1U), &s_set);
    if (!loaded) {
        defaults(&s_set);
        s_active_slot = 0;
        err = save_set();
        if (err != ESP_OK) ESP_LOGW(TAG, "default rules not persisted: %s", esp_err_to_name(err));
    } else {
        s_active_slot = active < 2 ? active : 0;
        ESP_LOGI(TAG, "loaded %u persisted rules (slot=%u)", s_set.count, s_active_slot);
    }
    migrate_face_gate();
    rule_engine_reset_runtime(&s_set);
    for (int i = 0; i < s_set.count && i < RULE_SET_MAX_RULES; ++i) {
        const rule_t *r = &s_set.rules[i];
        if (r->id == 1 || r->id == 2)
            ESP_LOGI(TAG, "rule id=%u enabled=%u cond=%u dev=0x%02X",
                     (unsigned)r->id, (unsigned)r->enabled,
                     (unsigned)r->conds[0].src, (unsigned)r->conds[0].dev_id);
    }
    s_last_tick_us = 0;
    ESP_LOGI(TAG, "ready: %u rules, all defaults opt-in", s_set.count);
    return ESP_OK;
}

/* ── 延迟合并落盘（理由见 rule_runtime_set_enabled 的注释） ── */
#define NVS_DEBOUNCE_MS 1500
static bool    s_set_dirty;
static int64_t s_set_dirty_us;

void rule_runtime_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_tick_us && now - s_last_tick_us < 200000LL) return;
    s_last_tick_us = now;

    /* 空闲够久 → 把合并的规则改动落盘一次（每次拨都写会每次闪一帧） */
    if (s_set_dirty && now - s_set_dirty_us >= (int64_t)NVS_DEBOUNCE_MS * 1000) {
        s_set_dirty = false;
        esp_err_t err = save_set();
        if (err != ESP_OK) ESP_LOGW(TAG, "deferred save failed: %s", esp_err_to_name(err));
    }

    int fired = rule_engine_tick(&s_set, AppProto_GetLinkState() == LINK_UP,
                                 now, sensor_get, send_command);
    if (fired) {
        s_last_fired = (uint32_t)fired;
        s_last_fire_us = now;
        ESP_LOGI(TAG, "fired=%d", fired);
    }
}

void rule_runtime_get_status(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    int64_t now = esp_timer_get_time();
    uint32_t fired = s_last_fired;
    if (fired && s_last_fire_us > 0 && now - s_last_fire_us < 3000000LL) {
        snprintf(out, out_size, "RULE FIRED / %lu CMD", (unsigned long)fired);
    } else if (AppProto_GetLinkState() != LINK_UP) {
        snprintf(out, out_size, "RULES PAUSED / OFFLINE");
    } else {
        snprintf(out, out_size, "RULES ACTIVE / LOCAL");
    }
}

bool rule_runtime_get_enabled(uint8_t index)
{
    return index < s_set.count && s_set.rules[index].enabled != 0;
}

esp_err_t rule_runtime_set_enabled(uint8_t index, bool enabled)
{
    if (index >= s_set.count) return ESP_ERR_INVALID_ARG;
    uint8_t value = enabled ? 1U : 0U;
    if (s_set.rules[index].enabled == value) return ESP_OK;
    s_set.rules[index].enabled = value;
    s_set.rules[index].cond_met = 0;
    s_set.rules[index].cond_since_us = 0;
    /* 延迟合并落盘：开关是低频操作，但每拨一次就写一次 flash 会「既闪一帧
     * （NVS 写期间 flash 操作占用 MSPI，DSI 拉帧缓冲断流，见 docs/16 §7.8）
     * 又白耗擦写寿命」。改为「改动后空闲 NVS_DEBOUNCE_MS 再写一次」。
     * 语义代价：该窗口内掉电会丢这次改动；且本函数不再同步返回落盘结果
     * （失败只在 tick 里记日志，UI 的"保存失败"提示因此不再触发）。 */
    s_set_dirty = true;
    s_set_dirty_us = esp_timer_get_time();
    return ESP_OK;
}

/* 每个 UI 自动化卡片 ↔ 规则 id。scene 0（回家）同时管理 Arrive 与 Leave：
 * 只开“有人开灯”而不管“无人关灯”，灯会一直亮到手动关闭，二者必须成对启停。
 * scene 1 = 智能灌溉(id 3)，scene 2 = 静音(id 5)，各只有一条。 */
static int scene_rule_indices(uint8_t scene, uint8_t *out, int max)
{
    static const uint8_t ids[3][2] = {{1, 2}, {3, 0}, {5, 0}};
    if (scene >= 3) return 0;
    int n = 0;
    for (int k = 0; k < 2; ++k) {
        uint8_t id = ids[scene][k];
        if (!id) continue;
        for (int i = 0; i < s_set.count && i < RULE_SET_MAX_RULES; ++i) {
            if (s_set.rules[i].id == id) {
                if (n < max) out[n] = (uint8_t)i;
                n++;
                break;
            }
        }
    }
    return n;
}

bool rule_runtime_get_scene_enabled(uint8_t scene)
{
    uint8_t idx[2];
    int n = scene_rule_indices(scene, idx, 2);
    return n > 0 && s_set.rules[idx[0]].enabled != 0;
}

esp_err_t rule_runtime_set_scene_enabled(uint8_t scene, bool enabled)
{
    uint8_t idx[2];
    int n = scene_rule_indices(scene, idx, 2);
    if (n == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < n; ++i) {
        esp_err_t e = rule_runtime_set_enabled(idx[i], enabled);
        if (e != ESP_OK) err = e;
    }
    return err;
}
