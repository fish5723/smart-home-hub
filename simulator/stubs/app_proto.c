/**
 * @file app_proto.c — PC 桩实现: 假影子数据(演示模式)
 * 改动这里的数值/把 link 拨到 LINK_DOWN, 即可模拟断链灰态。
 * 环境变量(供自动化截图自检):
 *   SIM_LINK=0/1/2  链路 UP/DEGRADED/DOWN
 *   SIM_ALARM=0/1/2 无告警 / MQ-4 紧急进行中 / 已恢复
 */
#include "app_proto.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static link_state_t s_link = LINK_UP;
static bool s_env_read;

static sensor_shadow_entry_t s_sensors[] = {
    { PROTO_DEV_DHT11_TEMP,  PROTO_QUAL_VALID, 24.6f,    0 },
    { PROTO_DEV_DHT11_HUM,   PROTO_QUAL_VALID, 58.0f,    0 },
    { PROTO_DEV_BMP280_TEMP, PROTO_QUAL_VALID, 26.1f,    0 },
    { PROTO_DEV_BMP280_PRESS,PROTO_QUAL_VALID, 101320.0f,0 },
    { PROTO_DEV_MQ135,       PROTO_QUAL_VALID, 12.0f,    0 },
    { PROTO_DEV_MQ4,         PROTO_QUAL_VALID, 21.0f,    0 },
    { PROTO_DEV_SOIL,        PROTO_QUAL_VALID, 41.0f,    0 },
    { PROTO_DEV_RAIN,        PROTO_QUAL_VALID, 12.0f,    0 },
    { PROTO_DEV_HUMAN,       PROTO_QUAL_VALID, 1.0f,     0 },
};

static actuator_shadow_entry_t s_acts[] = {
    { PROTO_DEV_RELAY1, 0, 0 },   /* 客厅灯 关 */
    { PROTO_DEV_RELAY2, 0, 0 },   /* 水泵 关 */
    { PROTO_DEV_LOCK,   0, 0 },   /* 门锁 上锁 */
    { PROTO_DEV_BEEPER, 0, 0 },
};

static bool s_feedback_valid;
static actuator_feedback_t s_feedback;

static void read_env_once(void)
{
    if (s_env_read) return;
    s_env_read = true;
    const char *lk = getenv("SIM_LINK");
    if (lk) s_link = (link_state_t)atoi(lk);
    const char *al = getenv("SIM_ALARM");
    if (al && atoi(al) == 1) {
        s_sensors[5].value = 62.0f;   /* MQ-4 超阈值 */
    }
    const char *t = getenv("SIM_TEMP");
    if (t) s_sensors[0].value = (float)atof(t);
    const char *h = getenv("SIM_HUM");
    if (h) s_sensors[1].value = (float)atof(h);
}

/* 让温度随"调用次数"缓慢起伏，这样在 PC 上推进时钟后能看到一条真实的趋势线；
 * 属于桩数据本身的形态，与固件逻辑无关。 */
static int s_tick_n;
static void advance_stub_data(void)
{
    s_tick_n++;
    if (getenv("SIM_TEMP") || getenv("SIM_HUM")) return;   /* 显式覆盖时不漂移 */
    s_sensors[0].value = 24.6f + 2.2f * (float)sin((double)s_tick_n / 42.0);
    s_sensors[1].value = 58.0f + 9.0f * (float)sin((double)s_tick_n / 97.0);
    s_sensors[8].value = (s_tick_n % 240) < 120 ? 1.0f : 0.0f;
}

static void stamp_all(void)
{
    static bool stamped = false;
    if (stamped) return;
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < sizeof(s_sensors)/sizeof(s_sensors[0]); i++)
        s_sensors[i].last_update_us = now;
    for (unsigned i = 0; i < sizeof(s_acts)/sizeof(s_acts[0]); i++)
        s_acts[i].last_update_us = now;
    stamped = true;
}

/* 让影子时间戳一直新鲜（模拟器里没有真实的 F103 周期上报） */
static void refresh_stamps(void)
{
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < sizeof(s_sensors)/sizeof(s_sensors[0]); i++)
        s_sensors[i].last_update_us = now;
}

link_state_t AppProto_GetLinkState(void) { read_env_once(); return s_link; }

int AppProto_GetSensorShadow(sensor_shadow_entry_t *out, int max_entries)
{
    read_env_once();
    advance_stub_data();
    stamp_all();
    refresh_stamps();
    const char *q = getenv("SIM_QUAL");
    if (q) {
        uint8_t qv = (uint8_t)atoi(q);
        for (unsigned i = 0; i < sizeof(s_sensors)/sizeof(s_sensors[0]); i++) s_sensors[i].quality = qv;
    }
    /* 单独把雨滴置成"超量程"，用于核对三合一读数列的逐路出词 */
    const char *rq = getenv("SIM_RAINQ");
    if (rq) s_sensors[7].quality = (uint8_t)atoi(rq);
    int n = (int)(sizeof(s_sensors)/sizeof(s_sensors[0]));
    if (n > max_entries) n = max_entries;
    for (int i = 0; i < n; i++) out[i] = s_sensors[i];
    return n;
}

int AppProto_GetActuatorShadow(actuator_shadow_entry_t *out, int max_entries)
{
    read_env_once();
    stamp_all();
    int n = (int)(sizeof(s_acts)/sizeof(s_acts[0]));
    if (n > max_entries) n = max_entries;
    for (int i = 0; i < n; i++) out[i] = s_acts[i];
    return n;
}

bool AppProto_GetLastActuatorFeedback(actuator_feedback_t *out)
{
    if (!s_feedback_valid) return false;
    s_feedback.last_update_us = esp_timer_get_time();
    if (out) *out = s_feedback;
    return true;
}

bool AppProto_GetActiveAlarm(alarm_shadow_t *out)
{
    alarm_shadow_t tmp;
    AppProto_GetAlarmShadow(&tmp);
    if (!tmp.active) return false;
    if (out) *out = tmp;
    return true;
}

void AppProto_GetAlarmShadow(alarm_shadow_t *out)
{
    read_env_once();
    const char *al = getenv("SIM_ALARM");
    int mode = al ? atoi(al) : 0;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (mode == 0) return;                     /* 从未发生 */
    out->alarm_id = PROTO_ALARM_GAS_MQ4;
    out->level = PROTO_LEVEL_URGENT;
    out->value = (mode == 1) ? 62.0f : 41.0f;
    out->threshold = 60.0f;
    out->active = (mode == 1);
    out->last_update_us = esp_timer_get_time();
}

float AppProto_GetSensorValue(uint8_t dev_id)
{
    for (unsigned i = 0; i < sizeof(s_sensors)/sizeof(s_sensors[0]); i++)
        if (s_sensors[i].dev_id == dev_id) return s_sensors[i].value;
    return NAN;
}

void AppProto_SendActuatorCmd(uint8_t dev_id, uint8_t action, uint16_t ttl_ms)
{
    (void)ttl_ms;
    for (unsigned i = 0; i < sizeof(s_acts)/sizeof(s_acts[0]); i++) {
        if (s_acts[i].dev_id != dev_id) continue;
        s_acts[i].state = (action == PROTO_ACTION_ON) ? 1 : 0;
        s_acts[i].last_update_us = esp_timer_get_time();
        s_feedback.dev_id = dev_id;
        s_feedback.result = PROTO_RESULT_OK;
        s_feedback.state = s_acts[i].state;
        s_feedback.last_update_us = esp_timer_get_time();
        s_feedback_valid = true;
    }
}

void AppProto_MarkUiClick(void) {}
void AppProto_QueryActuatorStatus(void) {}
