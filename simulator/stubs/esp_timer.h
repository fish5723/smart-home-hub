/**
 * @file esp_timer.h — PC 桩: 用 SDL 时钟模拟 esp_timer_get_time
 * 仅模拟器构建使用(板上用真实 esp_timer)。
 */
#ifndef ESP_TIMER_H_STUB
#define ESP_TIMER_H_STUB

#include <SDL2/SDL.h>
#include <stdint.h>

static inline int64_t esp_timer_get_time(void)
{
    return (int64_t)SDL_GetTicks() * 1000LL;   /* ms → µs */
}

#endif
