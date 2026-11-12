/**
 * @file watchdog.h
 * @brief 统一看门狗策略（ADR-017）
 *
 * 规则:
 * 1. 主任务超时 10 秒（sdkconfig.defaults 固化）
 * 2. 耗时操作临时放宽到 60 秒，完成后恢复
 * 3. 全程不调用 esp_task_wdt_deinit()
 * 4. 每个后台任务注册 TWDT + 循环内喂狗
 *
 * 来源: 本工程新建
 */
#pragma once

#include "esp_task_wdt.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDT_TAG "WDT"

/** 默认超时 (s) — 与 sdkconfig.defaults 的 CONFIG_ESP_TASK_WDT_TIMEOUT_S 一致 */
#define WDT_DEFAULT_TIMEOUT_S  10

/** 耗时操作超时 (s) — 字体加载 / OTA / 图像初始化 */
#define WDT_LONGOP_TIMEOUT_S   60

/**
 * @brief 注册当前任务到 TWDT
 * @return esp_err_t
 */
static inline esp_err_t wdt_add_current_task(void)
{
    esp_err_t ret = esp_task_wdt_add(NULL);
    if (ret == ESP_OK) {
        ESP_LOGD(WDT_TAG, "Current task added to TWDT");
    } else {
        ESP_LOGW(WDT_TAG, "TWDT add failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 临时放宽 TWDT 超时（用于耗时操作前）
 * @param timeout_s 超时秒数
 * @return 之前的超时配置（用于恢复）
 */
static inline int wdt_relax(int timeout_s)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = timeout_s * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic  = false,
    };
    esp_err_t ret = esp_task_wdt_reconfigure(&cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(WDT_TAG, "TWDT relaxed to %ds", timeout_s);
    } else {
        ESP_LOGW(WDT_TAG, "TWDT reconfigure failed: %s", esp_err_to_name(ret));
    }
    return timeout_s;  /* 注意: 实际返回的是请求值, 调用方需自行记住旧值 */
}

/**
 * @brief 恢复 TWDT 超时到默认值
 */
static inline void wdt_restore(void)
{
    wdt_relax(WDT_DEFAULT_TIMEOUT_S);
}

/**
 * @brief 将 TWDT 超时临时放宽到 60s（耗时操作专用）
 *
 * 用法:
 *   wdt_relax_for_long_op();
 *   ... 耗时操作 ...
 *   wdt_restore();
 */
static inline void wdt_relax_for_long_op(void)
{
    wdt_relax(WDT_LONGOP_TIMEOUT_S);
}

/**
 * @brief 喂狗（当前任务的 esp_task_wdt_reset）
 */
static inline void wdt_feed(void)
{
    esp_task_wdt_reset();
}

#ifdef __cplusplus
}
#endif