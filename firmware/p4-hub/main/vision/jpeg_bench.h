/**
 * @file jpeg_bench.h
 * @brief P1 验收：JPEG 硬件编码 vs 软件编码对比
 *
 * 同一帧 RGB565 分别走：
 *   - 硬件：esp_driver_jpeg（P4 JPEG codec）
 *   - 软件：esp_new_jpeg（纯 CPU）
 * 各跑 5 次取耗时中位数、输出字节数，并用硬件解码器解回 RGB565 计算 PSNR。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     hw_ok;         /*!< 硬件编码 5 次全部成功 */
    bool     sw_ok;         /*!< 软件编码 5 次全部成功 */
    int64_t  hw_us;         /*!< 硬件编码耗时中位数（微秒） */
    int64_t  sw_us;         /*!< 软件编码耗时中位数（微秒） */
    uint32_t hw_bytes;      /*!< 硬件编码输出字节数 */
    uint32_t sw_bytes;      /*!< 软件编码输出字节数 */
    bool     hw_decoded;    /*!< 硬件码流可被硬件解码器解回 RGB565 */
    bool     sw_decoded;    /*!< 软件码流可被硬件解码器解回 RGB565 */
    float    hw_psnr_db;    /*!< 硬件码流解码后与原帧的 PSNR */
    float    sw_psnr_db;    /*!< 软件码流解码后与原帧的 PSNR */
} jpeg_bench_result_t;

/**
 * @brief 对一帧 RGB565 跑硬件/软件编码对比
 *
 * @param rgb565   输入帧（w*h*2 字节，RGB565 小端）
 * @param width    宽
 * @param height   高
 * @param quality  JPEG 质量（建议 60~90）
 * @param out      结果
 * @return ESP_OK 表示整个流程执行完毕（单项失败记录在 out->*_ok）
 */
esp_err_t jpeg_bench_run(const uint16_t *rgb565, uint32_t width, uint32_t height,
                         uint8_t quality, jpeg_bench_result_t *out);
