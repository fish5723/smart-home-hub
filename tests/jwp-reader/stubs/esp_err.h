/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * 宿主机（PC）版 esp_err.h —— 仅为 tests/jwp-reader 提供最小定义。
 * 数值故意与 ESP-IDF 保持一致，便于对照真机日志。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_VERSION   0x10A

const char *esp_err_to_name(esp_err_t code);