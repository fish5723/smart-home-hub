/**
 * @file rule_runtime.c — PC 桩: 规则运行时的内存实现
 * 真机走 NVS 双槽持久化; 模拟器只保证 ui_pages.c 的读写路径可跑通,
 * 便于在 PC 上按开关状态截图自检(默认与原型一致: 回家/灌溉开, 夜间守护关)。
 */
#include "rule_runtime.h"
#include <stdio.h>
#include <string.h>

static bool s_scene_on[3] = { true, true, false };

esp_err_t rule_runtime_init(void) { return ESP_OK; }
void rule_runtime_tick(void) {}

bool rule_runtime_get_enabled(uint8_t index)
{
    return index < 3 ? s_scene_on[index] : false;
}

esp_err_t rule_runtime_set_enabled(uint8_t index, bool enabled)
{
    if (index >= 3) return ESP_FAIL;
    s_scene_on[index] = enabled;
    return ESP_OK;
}

bool rule_runtime_get_scene_enabled(uint8_t scene)
{
    return rule_runtime_get_enabled(scene);
}

esp_err_t rule_runtime_set_scene_enabled(uint8_t scene, bool enabled)
{
    return rule_runtime_set_enabled(scene, enabled);
}

void rule_runtime_get_status(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    int on = 0;
    for (int i = 0; i < 3; i++) if (s_scene_on[i]) on++;
    snprintf(out, out_size, "RULES %d/3 ACTIVE / LOCAL", on);
}
