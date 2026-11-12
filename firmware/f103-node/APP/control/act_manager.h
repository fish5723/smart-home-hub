/**
 * @file act_manager.h
 * @brief F103 执行器单一所有权与失效安全状态机。
 */
#ifndef ACT_MANAGER_H
#define ACT_MANAGER_H

#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACT_MANAGER_COUNT       4U
#define ACT_MANAGER_PUMP_MAX_MS 30000U
#define ACT_MANAGER_LOCK_MAX_MS 1000U
#define ACT_MANAGER_BEEP_MAX_MS 5000U

typedef enum {
    ACT_SOURCE_REMOTE = 0,
    ACT_SOURCE_LOCAL   = 1,
} act_source_t;

void act_manager_init(void);

/** Execute a logical command. State is read back after the GPIO write. */
uint8_t act_manager_command(uint8_t dev_id, uint8_t action, uint16_t ttl_ms,
                            act_source_t source, uint32_t now_ms);

/** Apply TTLs and the link-down safety policy. */
void act_manager_poll(uint32_t now_ms, uint8_t link_down);

/** Read the current logical state: relay/pump ON=1, lock LOCKED=1. */
uint8_t act_manager_get_state(uint8_t dev_id);

/** Local safety alarm path; still owns the sole beeper BSP write path. */
void act_manager_set_beeper(uint8_t enabled, uint16_t freq_hz, uint32_t now_ms);

/** Fill status entries as [count, dev_id, state] × count. */
uint16_t act_manager_get_status(uint8_t *payload, uint16_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* ACT_MANAGER_H */
