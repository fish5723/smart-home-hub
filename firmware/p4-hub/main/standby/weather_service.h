/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * weather_service — SNTP 授时 + uapis.cn 天气 HTTPS 拉取 + 缓存 + 场景映射
 *
 * ⚠️ 凭据策略（ADR-016 / ADR-020）：
 *   Key 与 location 绝不写入源码、sdkconfig.defaults 或 Git。
 *   取值顺序：NVS("wx_key"/"wx_loc") → Kconfig 空默认值 → 无 Key 则只授时、不请求。
 *   wx_key = uapis.cn Bearer Token（必需）；wx_loc 可空 = 按来源 IP 自动定位。
 *
 * ⚠️ 本模块不触碰 LVGL；天气状态通过 weather_state_t 快照 + 回调交给上层。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 壁纸场景类别（决定选哪段 JWP） */
typedef enum {
    WEATHER_SCENE_UNKNOWN = 0,
    WEATHER_SCENE_CLEAR,
    WEATHER_SCENE_CLOUDY,
    WEATHER_SCENE_OVERCAST,
    WEATHER_SCENE_RAIN,
    WEATHER_SCENE_THUNDER,
    WEATHER_SCENE_SNOW,
    WEATHER_SCENE_FOG,
    WEATHER_SCENE_HAZE,
    WEATHER_SCENE_NIGHT,
} weather_scene_t;

/** 天气状态快照 */
typedef struct {
    bool           valid;          /**< 是否拿到过有效数据（含缓存） */
    bool           from_cache;     /**< 本次是否来自 NVS 缓存 */
    int            temperature_c;  /**< 温度（摄氏度） */
    int            weather_code;   /**< 天气图标码（uapis weather_icon，与和风同体系） */
    weather_scene_t scene;         /**< 映射后的场景 */
    bool           is_night;       /**< 日出/日落判定（缺失时按本地时段回退） */
    char           condition[32];  /**< 天气现象文案（中文） */
    char           city[64];       /**< 城市名 */
    time_t         updated_at;     /**< 数据时间戳（epoch） */
} weather_state_t;

/** 天气更新回调（在 weather 任务上下文被调用，禁止在此调用 LVGL API） */
typedef void (*weather_update_cb_t)(const weather_state_t *state, void *user);

/**
 * @brief 启动天气服务（创建后台任务；非阻塞）
 *
 * 内部流程：等待 Wi-Fi → SNTP 同步 → 拉取实况+当日日出日落（forecast=true）→ 映射场景 → 周期刷新。
 * 未配置 Key 时仅同步时间，不发网络请求。
 */
esp_err_t weather_service_start(void);

/** 停止天气服务任务 */
void weather_service_stop(void);

/** 读取当前天气快照（线程安全拷贝） */
void weather_service_get_state(weather_state_t *out);

/** 注册/替换天气更新回调 */
void weather_service_register_cb(weather_update_cb_t cb, void *user);

/** 系统时间是否已通过 SNTP 同步（epoch > 2020） */
bool weather_service_time_valid(void);

/** 请求立即刷新一次（非阻塞） */
esp_err_t weather_service_refresh_now(void);

/** 把天气图标码（uapis/和风同体系）+ 昼/夜映射为壁纸场景（纯函数，便于单测/复用） */
weather_scene_t weather_scene_from_code(int weather_code, bool is_night);

/** 场景 → 稳定字符串（用于壁纸文件路径 <scene>/day.jwp） */
const char *weather_scene_to_string(weather_scene_t scene);

/** 字符串 → 场景（"sunny"/"rain"/... ；无法识别时返回 UNKNOWN）。纯函数 */
weather_scene_t weather_scene_from_string(const char *name);

#ifdef __cplusplus
}
#endif