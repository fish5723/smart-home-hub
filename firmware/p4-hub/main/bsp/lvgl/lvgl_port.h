/**
 * @file lvgl_port.h
 * @brief LVGL Port — esp_lvgl_adapter 封装
 *
 * 硬件: JC-ESP32P4-M3-DEV + JD9165BA 1024×600 MIPI DSI + GT911
 */

#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL (esp_lvgl_adapter + 显示)
 *
 * 内部流程:
 *   1. esp_lv_adapter_init()
 *   2. esp_lv_adapter_register_display() ← 绑定 lcd_get_panel/lcd_get_panel_io
 *   3. esp_lv_adapter_start()
 *
 * 注意: 触摸注册通过 lvgl_port_register_touch() 单独调用。
 *
 * @return ESP_OK 成功
 */
esp_err_t lvgl_port_init(void);

/**
 * @brief 注册触摸输入设备 (在 lvgl_port_init 之后调用)
 *
 * 触摸注册独立于显示初始化，便于控制初始化顺序。
 * 与 WiFi SDIO 共存时，需在 WiFi 初始化后调用以避免 DMA 破坏 touch handle。
 *
 * @return ESP_OK 成功
 */
esp_err_t lvgl_port_register_touch(void);

#ifdef __cplusplus
}
#endif