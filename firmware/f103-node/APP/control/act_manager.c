/**
 * @file act_manager.c
 * @brief All F103 actuator GPIO writes live here.
 */
#include "act_manager.h"

#include "bsp_beep.h"
#include "bsp_pins.h"
#include "app_data.h"
#include "main.h"

#include <string.h>

typedef struct {
    uint8_t state;
    uint8_t beeper_running;
    uint32_t expiry_ms;
    uint32_t started_ms;
} act_state_t;

static act_state_t s_act[ACT_MANAGER_COUNT];
static uint32_t s_link_down_since_ms;

static int act_index(uint8_t dev_id)
{
    switch (dev_id) {
    case PROTO_DEV_RELAY1: return 0;
    case PROTO_DEV_RELAY2: return 1;
    case PROTO_DEV_LOCK:   return 2;
    case PROTO_DEV_BEEPER: return 3;
    default: return -1;
    }
}

static uint8_t gpio_state(uint8_t idx)
{
    switch (idx) {
    case 0: return HAL_GPIO_ReadPin(RELAY_PORT, RELAY_PIN) == GPIO_PIN_RESET;
    case 1: return HAL_GPIO_ReadPin(PUMP_PORT, PUMP_PIN) == GPIO_PIN_SET;
    case 2: return HAL_GPIO_ReadPin(LOCK_PORT, LOCK_PIN) == GPIO_PIN_RESET;
    case 3: return s_act[3].beeper_running;
    default: return 0;
    }
}

static void write_state(uint8_t idx, uint8_t state)
{
    switch (idx) {
    case 0: HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN,
                              state ? GPIO_PIN_RESET : GPIO_PIN_SET); break;
    case 1: HAL_GPIO_WritePin(PUMP_PORT, PUMP_PIN,
                              state ? GPIO_PIN_SET : GPIO_PIN_RESET); break;
    case 2: /* 门锁模块与设计不符、不接入（app_lock/bsp_lock 已删除），
             * 但保留 PB8 的单一写路径：state=1 → 锁定电平。 */
            HAL_GPIO_WritePin(LOCK_PORT, LOCK_PIN,
                              state ? GPIO_PIN_RESET : GPIO_PIN_SET); break;
    case 3: if (state) BEEP_On(); else BEEP_Off(); break;
    default: break;
    }
    s_act[idx].beeper_running = (idx == 3) ? state : s_act[idx].beeper_running;
    s_act[idx].state = gpio_state(idx);
}

void act_manager_init(void)
{
    memset(s_act, 0, sizeof(s_act));
    /* ADR-023: light/pump/beeper OFF; titanium lock LOCKED. */
    HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(PUMP_PORT, PUMP_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LOCK_PORT, LOCK_PIN, GPIO_PIN_RESET);  /* ADR-023: 锁定态 */
    BEEP_Off();
    s_act[2].state = gpio_state(2);
    s_link_down_since_ms = 0;
}

uint8_t act_manager_command(uint8_t dev_id, uint8_t action, uint16_t ttl_ms,
                            act_source_t source, uint32_t now_ms)
{
    int idx = act_index(dev_id);
    if (idx < 0) return PROTO_RESULT_ERR_PARAM;

    /* Only the local fallback may drive the pump while the link is down.
     * The caller gates this policy; the manager still enforces hard limits. */
    uint32_t max_ttl = 0;
    if (idx == 1) max_ttl = ACT_MANAGER_PUMP_MAX_MS;
    if (idx == 2) max_ttl = ACT_MANAGER_LOCK_MAX_MS;
    if (idx == 3) max_ttl = ACT_MANAGER_BEEP_MAX_MS;

    uint8_t state;
    if (idx == 2) {
        if (action == PROTO_ACTION_ON) {
            state = 1; /* lock */
            ttl_ms = 0;
        } else if (action == PROTO_ACTION_PULSE) {
            if (ttl_ms == 0 || ttl_ms > ACT_MANAGER_LOCK_MAX_MS)
                return PROTO_RESULT_ERR_TTL;
            state = 0; /* unlock only for a bounded pulse */
        } else {
            return PROTO_RESULT_ERR_PARAM;
        }
    } else if (idx == 3) {
        if (action == PROTO_ACTION_PULSE) {
            if (ttl_ms == 0 || ttl_ms > ACT_MANAGER_BEEP_MAX_MS)
                return PROTO_RESULT_ERR_TTL;
            state = 1;
        } else if (action == PROTO_ACTION_ON) {
            state = 1;
        } else if (action == PROTO_ACTION_OFF) {
            state = 0;
            ttl_ms = 0;
        } else {
            return PROTO_RESULT_ERR_PARAM;
        }
    } else {
        if (action == PROTO_ACTION_ON) state = 1;
        else if (action == PROTO_ACTION_OFF) state = 0;
        else return PROTO_RESULT_ERR_PARAM;
        if (max_ttl != 0 && ttl_ms > max_ttl) return PROTO_RESULT_ERR_TTL;
        /* A pump may never be persistent in physical time: ttl=0 remains
         * persistent protocol syntax, but the hardware watchdog still ends it. */
        if (idx == 1 && state && ttl_ms == 0) ttl_ms = ACT_MANAGER_PUMP_MAX_MS;
    }

    (void)source; /* source is retained in the API for future arbitration logs. */
    write_state((uint8_t)idx, state);
    s_act[idx].started_ms = now_ms;
    s_act[idx].expiry_ms = ttl_ms ? now_ms + ttl_ms : 0;
    return PROTO_RESULT_OK;
}

void act_manager_poll(uint32_t now_ms, uint8_t link_down)
{
    if (link_down) {
        if (s_link_down_since_ms == 0) s_link_down_since_ms = now_ms;
        if ((uint32_t)(now_ms - s_link_down_since_ms) >= 10000U) {
            /* ADR-023: light/pump/beeper off; lock keeps its safe state. */
            write_state(0, 0);
            write_state(1, 0);
            write_state(3, 0);
        }
    } else {
        s_link_down_since_ms = 0;
    }

    for (uint8_t i = 0; i < ACT_MANAGER_COUNT; i++) {
        if (s_act[i].expiry_ms != 0 &&
            (int32_t)(now_ms - s_act[i].expiry_ms) >= 0) {
            s_act[i].expiry_ms = 0;
            write_state(i, i == 2 ? 1 : 0);
        }
        s_act[i].state = gpio_state(i);
    }

    /* 门锁状态镜像：原由 APP_LOCK_Task 维护，该任务已删除（2026-10-18），
     * 改由唯一 GPIO 写路径这里维护，供 OLED/上报消费。 */
    g_sensor_data.lock_unlocked = gpio_state(2) ? 0U : 1U;
}

uint8_t act_manager_get_state(uint8_t dev_id)
{
    int idx = act_index(dev_id);
    return idx < 0 ? 0 : gpio_state((uint8_t)idx);
}

void act_manager_set_beeper(uint8_t enabled, uint16_t freq_hz, uint32_t now_ms)
{
    if (enabled) BEEP_PWM_Start(freq_hz);
    else BEEP_PWM_Stop();
    s_act[3].beeper_running = enabled ? 1U : 0U;
    s_act[3].state = s_act[3].beeper_running;
    s_act[3].started_ms = now_ms;
    s_act[3].expiry_ms = 0; /* local gas safety alarm owns its stop condition */
}

uint16_t act_manager_get_status(uint8_t *payload, uint16_t max_len)
{
    static const uint8_t ids[ACT_MANAGER_COUNT] = {
        PROTO_DEV_RELAY1, PROTO_DEV_RELAY2, PROTO_DEV_LOCK, PROTO_DEV_BEEPER
    };
    if (!payload || max_len < 1U + ACT_MANAGER_COUNT * 2U) return 0;
    payload[0] = ACT_MANAGER_COUNT;
    for (uint8_t i = 0; i < ACT_MANAGER_COUNT; i++) {
        payload[1U + i * 2U] = ids[i];
        payload[2U + i * 2U] = gpio_state(i);
    }
    return 1U + ACT_MANAGER_COUNT * 2U;
}
