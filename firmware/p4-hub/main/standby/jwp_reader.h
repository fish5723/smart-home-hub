/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * jwp_reader — JWP (JPEG Wallpaper) 容器解析器
 *
 * 参考来源: https://github.com/maxiumlee/ESP32-S3-LVGL-Clock
 *           HEAD 299ab2e63ea85f3463edc8fd8fd75142573c50fa (Apache-2.0)
 * 移植范围: 仅 JWP 容器格式定义与顺序读取算法。
 *           未复制参考工程任何 Arduino / 播放器 / 天气业务代码。
 *
 * 本模块只负责"文件与格式"，不依赖 LVGL、不依赖任何显示驱动。
 * 因此可以被宿主机 (PC) 单元测试直接编译（见 tests/jwp-reader/）。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JWP_MAGIC                 "JWP1"
#define JWP_HEADER_SIZE           32u
#define JWP_VERSION               1u
#define JWP_PIXEL_FORMAT_BASELINE_JPEG  1u

/**
 * @brief JWP 32 字节文件头（little-endian）
 *
 *   offset  size  field
 *   ------  ----  --------------------------------------------
 *   0       4     magic[4] = "JWP1"
 *   4       2     version
 *   6       2     header_size (= 32)
 *   8       2     width
 *   10      2     height
 *   12      2     fps_times_100   (fps * 100)
 *   14      2     pixel_format    (1 = baseline JPEG)
 *   16      4     frame_count
 *   20      4     max_frame_bytes
 *   24      4     payload_crc32   (0 = 未计算/不校验)
 *   28      4     transition_offset (循环回跳点，相对文件起始的字节偏移)
 *
 * 之后重复 frame_count 次:
 *   uint32_t jpeg_frame_length;
 *   uint8_t  jpeg_frame_data[jpeg_frame_length];
 */
typedef struct {
    FILE    *file;              /**< 已打开的文件句柄（fopen/rb） */
    uint16_t version;           /**< 格式版本，必须 == 1 */
    uint16_t header_size;       /**< 头部长度，必须 == 32 */
    uint16_t width;             /**< 图像宽（像素） */
    uint16_t height;            /**< 图像高（像素） */
    uint16_t fps_times_100;     /**< 帧率 × 100 */
    uint16_t pixel_format;      /**< 1 = baseline JPEG */
    uint32_t frame_count;       /**< 完整循环一遍的帧数 */
    uint32_t max_frame_bytes;   /**< 单帧 JPEG 上限（用于校验缓冲区） */
    uint32_t payload_crc32;     /**< 载荷 CRC32（0 = 未计算） */
    uint32_t transition_offset; /**< 循环回跳字节偏移（0 = 回到第一帧） */
    uint32_t current_frame;     /**< 下一次 read_frame 将返回的帧序号 */
    uint32_t data_offset;       /**< 第一帧长度字段的文件偏移（== header_size） */
    uint32_t file_size;         /**< 文件总长度（字节） */
    uint32_t loop_offset;       /**< 实际生效的循环回跳偏移 */
} jwp_reader_t;

/**
 * @brief 打开并校验 JWP 文件头、全部帧边界及循环偏移
 *
 * @param[out] reader 解析器实例（调用方负责生命周期）
 * @param[in]  path   VFS 路径，如 "/sdcard/wallpaper/sunny/day.jwp"
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_SIZE /
 *         ESP_ERR_INVALID_VERSION / ESP_ERR_NOT_SUPPORTED
 */
esp_err_t jwp_reader_open(jwp_reader_t *reader, const char *path);

/**
 * @brief 顺序读取下一帧 JPEG
 *
 * 到达 frame_count 之后自动回到 loop_offset 继续（无缝循环）。
 * 若 loop_offset 位于文件中段，读到区域末尾（记录不完整）时也会自动回跳。
 *
 * @param[in]  reader      已打开且已定位的解析器
 * @param[out] buffer      接收 JPEG 数据的缓冲区
 * @param[in]  buffer_size 缓冲区大小（应 >= max_frame_bytes）
 * @param[out] out_size    实际写入的字节数，可为 NULL
 * @return ESP_OK / ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_SIZE / ESP_FAIL
 */
esp_err_t jwp_reader_read_frame(jwp_reader_t *reader,
                                uint8_t *buffer,
                                size_t buffer_size,
                                size_t *out_size);

/**
 * @brief 随机定位到第 frame 帧（0 基），下一次 read_frame 返回该帧
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE / ESP_FAIL
 */
esp_err_t jwp_reader_seek_frame(jwp_reader_t *reader, uint32_t frame);

/**
 * @brief 回到循环起点（优先 transition_offset，否则第一帧）
 */
esp_err_t jwp_reader_rewind_loop(jwp_reader_t *reader);

/**
 * @brief 当前生效的循环回跳字节偏移
 */
uint32_t jwp_reader_loop_offset(const jwp_reader_t *reader);

/**
 * @brief 关闭文件并清零实例
 */
void jwp_reader_close(jwp_reader_t *reader);

/**
 * @brief 计算图像载荷 CRC32（IEEE 802.3 / zlib 兼容）
 *
 * 载荷 = 从 data_offset 到 EOF 的全部字节（含每帧长度字段）。
 * 转换脚本 make_wallpaper_jwp.py 用同一算法写入 header.payload_crc32。
 *
 * @param[in] crc  前一次结果，首次传入 0
 * @param[in] data 数据
 * @param[in] len  长度
 */
uint32_t jwp_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
