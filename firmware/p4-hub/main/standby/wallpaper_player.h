/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * wallpaper_player — JWP 动态壁纸播放器
 *
 * 参考来源: https://github.com/maxiumlee/ESP32-S3-LVGL-Clock (Apache-2.0),
 *           HEAD 299ab2e63ea85f3463edc8fd8fd75142573c50fa。
 * 移植范围: "SD 流式读取 → 逐帧 JPEG 解码 → RGB565 帧缓冲 → 显示" 的流水线
 *           思路与循环/暂停语义；解码改用 ESP-IDF 组件 esp_new_jpeg，
 *           分辨率/缓冲/帧率全部按 1024×600 重新设计。
 *
 * 边界约定（重要）:
 *  - 本模块不 include LVGL，也不创建/销毁任何 LVGL 对象。
 *  - 帧通过 frame_sink 回调以"缓冲区索引"的形式交给上层；上层在 LVGL
 *    任务上下文中用 lv_async_call 把它映射成 lv_image_set_src。
 *  - 发布后的缓冲区处于 in-flight 状态，只有上层确认显示刷新已经完成后
 *    调用 wallpaper_player_frame_release() 才允许播放器再次写入。这样
 *    不依赖异步队列的长度，也不会用“固定轮转次数”猜测显示 DMA 状态。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 播放器持有并轮转的 RGB565 帧缓冲数量（>= 2 即可安全轮转） */
#define WALLPAPER_BUFFER_COUNT 3

/** 播放统计（用于帧率/错误恢复观测） */
typedef struct {
    uint32_t frames_decoded;  /**< 成功解码并发布的帧数 */
    uint32_t frames_dropped;  /**< 因解码超时或无空闲帧缓冲而跳过的帧数 */
    uint32_t decode_errors;   /**< 累计解码/读取错误 */
    uint32_t loop_count;      /**< 循环回跳次数 */
    uint32_t avg_decode_us;   /**< 平均单帧解码耗时 */
    uint32_t last_decode_us;  /**< 最近一次单帧解码耗时 */
    uint32_t target_fps;      /**< 当前目标帧率 */
    uint32_t source_fps;      /**< JWP 头部声明的帧率 */
    uint32_t source_frames;   /**< JWP 头部声明的帧数 */
    uint16_t width;           /**< 当前播放分辨率 */
    uint16_t height;

    /* ── 显示侧持有缓冲的性能打点（壁纸帧率排查用；不是功能量）──
     *   sink = 播放器任务在 frame_sink 回调内的停留时间。上层回调要取
     *          LVGL 适配器锁才能 lv_async_call，所以它约等于"等 LVGL 让出
     *          锁 + 排队"的时间；它大 = 播放器被显示侧卡住。
     *   hold = 一个缓冲从发布到被 release 的存活时间（显示侧持有时间）。
     *          它 ≫ 帧间隔 = 缓冲周转不过来 → frame_try_acquire 失败丢帧。 */
    uint32_t last_sink_us;    /**< 最近一帧 sink 停留时间 */
    uint32_t max_sink_us;     /**< sink 停留时间的本次开机最大值 */
    uint32_t last_hold_us;    /**< 最近一个缓冲的存活时间 */
    uint32_t max_hold_us;     /**< 缓冲存活时间的本次开机最大值 */
} wallpaper_stats_t;

/** 帧回调：buffer_index 为可用缓冲区下标；在播放器任务上下文被调用。 */
typedef void (*wallpaper_frame_sink_t)(uint32_t buffer_index, void *user);

/**
 * @brief 初始化播放器（分配缓冲 + 创建播放任务）
 *
 * @param width  图像宽（须与 JWP 头部一致）
 * @param height 图像高
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM
 */
esp_err_t wallpaper_player_init(uint16_t width, uint16_t height);

/** 停止播放并释放全部资源 */
void wallpaper_player_deinit(void);

/** 缓冲区数量（供上层建立 lv_image_dsc_t 映射） */
uint32_t wallpaper_player_buffer_count(void);

/** 第 index 个 RGB565 缓冲区地址（PSRAM，16 字节对齐） */
const void *wallpaper_player_buffer(uint32_t index);

/** 单个缓冲区字节数 = width * height * 2 */
size_t wallpaper_player_buffer_bytes(void);

/**
 * @brief 释放一个已经完成显示的帧缓冲
 *
 * 播放器发布帧后会把对应缓冲标记为 in-flight，直到调用此函数才会
 * 再次用于 JPEG 解码。调用方必须在 LVGL/display flush 完成之后调用；
 * 对同一个 index 重复调用是安全的。可从 LVGL 任务调用。
 */
void wallpaper_player_frame_release(uint32_t index);

/** 注册帧回调（可在 init 之后任意时刻设置） */
void wallpaper_player_set_sink(wallpaper_frame_sink_t sink, void *user);

/**
 * @brief 播放循环回跳（一轮播完、从头再来）时的通知回调
 *
 * 在播放任务上下文被调用，故回调里只能做轻量动作。需要换片时可直接调用
 * `wallpaper_player_play()` —— 它只是向播放任务投递命令，不会重入。
 */
typedef void (*wallpaper_loop_cb_t)(void *user);

/** 注册循环回跳回调（可在 init 之后任意时刻设置） */
void wallpaper_player_set_loop_cb(wallpaper_loop_cb_t cb, void *user);

/**
 * @brief 请求播放指定 JWP（非阻塞；失败在播放任务内以日志/统计体现）
 *
 * 路径不存在或格式非法时不会崩溃，只会停止播放并保留上一帧。
 */
esp_err_t wallpaper_player_play(const char *jwp_path);

/** 暂停解码（保留当前帧显示） */
void wallpaper_player_pause(void);

/** 恢复解码 */
void wallpaper_player_resume(void);

/** 停止播放并关闭文件（清屏交给上层决定） */
void wallpaper_player_stop(void);

bool wallpaper_player_is_playing(void);
bool wallpaper_player_is_paused(void);

/** 设置目标帧率（用于按实测性能降级：15 → 10 → 8 …） */
void wallpaper_player_set_target_fps(uint32_t fps);
uint32_t wallpaper_player_target_fps(void);

/**
 * @brief 网络就绪门控
 *
 * 置 false 后所有 play 请求只记下路径、不启动解码；置 true 时补播最后一次
 * 请求。用于开机阶段避让 Wi-Fi/MQTT 首轮突发：壁纸的 SD 流式读取与 C6 的
 * SDIO 共用 SDMMC 主机，重叠会把 esp_hosted 的 TX/RX mempool 打空
 * （mempool OOM），丢包后 TCP 写阻塞、broker 踢线。
 */
void wallpaper_player_set_network_ready(bool ready);
bool wallpaper_player_network_ready(void);

void wallpaper_player_get_stats(wallpaper_stats_t *out);

#ifdef __cplusplus
}
#endif
