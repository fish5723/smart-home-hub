/**
 * @file esp_log.h — PC 桩: ESP_LOGx → printf(仅模拟器构建)
 */
#ifndef ESP_LOG_H_STUB
#define ESP_LOG_H_STUB

#include <stdio.h>

#define ESP_LOGI(tag, fmt, ...) printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[D] %s: " fmt "\n", tag, ##__VA_ARGS__)

#endif
