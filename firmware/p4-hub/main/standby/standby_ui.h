/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * standby_ui — 全屏待机层（时钟 + 日期 + Wi-Fi + 天气 + 动态天气壁纸）
 *
 * 覆盖在既有六页智能家居 UI 之上；双击唤醒、空闲超时回到待机。
 *
 * ⚠️ 线程约束：本模块所有函数中，只有 standby_ui_update_* 与 standby_ui_set_scene
 *    这些"只写快照"的接口可以从任意任务调用；凡是直接操作 LVGL 对象的都在
 *    LVGL 任务的 1s 定时器或 lv_async_call 回调里执行。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "weather_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 场景切换回调（在 LVGL 任务上下文被调用；用于切换 JWP 壁纸） */
typedef void (*standby_scene_cb_t)(weather_scene_t scene, bool is_night, void *user);

/**
 * @brief 创建待机层（必须在 LVGL 任务上下文，且在现有 UI 之后创建）
 *
 * 创建全屏覆盖层、背景渐变、壁纸 lv_image、时间/日期/天气/Wi-Fi 标签，
 * 并注册 1s 刷新定时器与屏幕级 activity 监听。
 *
 * @param parent 通常为 lv_screen_active()
 */
void standby_ui_create(lv_obj_t *parent);

/** 显示待机层（隐藏主 UI 交互；不销毁主 UI） */
void standby_ui_show(void);

/** 隐藏待机层（露出既有智能家居 UI）并重置空闲计时 */
void standby_ui_hide(void);

bool standby_ui_is_visible(void);

/** 刷新空闲计时（任意触摸/点击/页面切换都应调用；线程安全） */
void standby_ui_notify_activity(void);

/** 立即按系统时间刷新时钟显示（通常由内部 1s 定时器调用） */
void standby_ui_update_time(void);

/** 系统时间是否已由 SNTP 同步（仅 LVGL 任务上下文有效读值） */
bool standby_ui_time_valid(void);

/** 更新天气快照（线程安全；LVGL 渲染发生在下一个 1s 定时器周期） */
void standby_ui_update_weather(const weather_state_t *state);

/** 用默认渐变背景表达场景（无壁纸/无网络时的兜底静态背景） */
void standby_ui_set_scene(weather_scene_t scene, bool is_night);

/** 注册场景切换回调（例如切 JWP） */
void standby_ui_set_scene_callback(standby_scene_cb_t cb, void *user);

/**
 * @brief 把 wallpaper_player 的帧缓冲接入待机层
 *
 * 内部：读取 wallpaper_player_buffer_count()/buffer()/buffer_bytes() 建立
 * lv_image_dsc_t 映射，并注册 frame sink（回调里 lv_async_call → lv_image_set_src）。
 */
void standby_ui_attach_wallpaper(void);

#ifdef __cplusplus
}
#endif