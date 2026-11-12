/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * 宿主机（PC）版 esp_log.h —— 仅为 tests/jwp-reader 提供最小实现。
 */
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stdout, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)