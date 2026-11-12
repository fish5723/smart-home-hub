/**
 * @file jpeg_bench.c
 * @brief P1 验收：JPEG 硬件编码 vs 软件编码对比
 *
 * 数据路径：
 *   输入 RGB565（PSRAM 快照）
 *     ├─ 硬件编码 esp_driver_jpeg → 码流 A（PSRAM）
 *     └─ 软件编码 esp_new_jpeg   → 码流 B（PSRAM）
 *   两条码流各用硬件解码器解回 RGB565，与输入帧算 PSNR。
 *
 * 计时只覆盖单次编码调用（不含缓冲分配/引擎创建）。
 */
#include "jpeg_bench.h"

#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

static const char *TAG = "JPEG_BENCH";

#define BENCH_RUNS 5

/* RGB565 → 8bit 三分量，用于 PSNR */
static inline void rgb565_to_rgb888(uint16_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
    *g = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
    *b = (uint8_t)((v & 0x1F) * 255 / 31);
}

static float psnr_rgb565(const uint16_t *a, const uint16_t *b, uint32_t pixels)
{
    double mse = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < pixels; i += 7) {
        uint8_t ar, ag, ab, br, bg, bb;
        rgb565_to_rgb888(a[i], &ar, &ag, &ab);
        rgb565_to_rgb888(b[i], &br, &bg, &bb);
        double dr = (double)ar - br;
        double dg = (double)ag - bg;
        double db = (double)ab - bb;
        mse += dr * dr + dg * dg + db * db;
        n += 3;
    }
    if (n == 0) return 0.0f;
    mse /= n;
    if (mse <= 0.0) return 99.0f;
    return (float)(10.0 * log10((255.0 * 255.0) / mse));
}

static int64_t median5(const int64_t *v)
{
    int64_t s[BENCH_RUNS];
    memcpy(s, v, sizeof(s));
    for (int i = 1; i < BENCH_RUNS; i++) {
        int64_t k = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > k) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = k;
    }
    return s[BENCH_RUNS / 2];
}

esp_err_t jpeg_bench_run(const uint16_t *rgb565, uint32_t width, uint32_t height,
                         uint8_t quality, jpeg_bench_result_t *out)
{
    if (!rgb565 || !out || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const size_t raw_bytes = (size_t)width * height * 2;
    const size_t cap_bytes = raw_bytes / 2;   /* 码流缓冲上限，质量 80 时远够 */

    /* 输入快照：避免与 CSI DMA 的并发写 */
    uint16_t *src = heap_caps_aligned_calloc(128, 1, raw_bytes,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 输出/解码缓冲必须用驱动的专用分配器：它们要求特定对齐 */
    jpeg_encode_memory_alloc_cfg_t enc_mem = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    jpeg_decode_memory_alloc_cfg_t dec_mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t hw_out_cap = 0, dec_cap = 0;
    uint8_t *hw_out = jpeg_alloc_encoder_mem(cap_bytes, &enc_mem, &hw_out_cap);
    uint16_t *decoded = jpeg_alloc_decoder_mem(raw_bytes, &dec_mem, &dec_cap);
    uint8_t *sw_out = heap_caps_aligned_calloc(128, 1, cap_bytes,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src || !hw_out || !sw_out || !decoded) {
        ESP_LOGE(TAG, "buffer alloc failed");
        free(src); free(hw_out); free(sw_out); free(decoded);
        return ESP_ERR_NO_MEM;
    }
    memcpy(src, rgb565, raw_bytes);
    /* CPU 刚写入的帧必须刷到 PSRAM，DMA 编码器才能读到 */
    esp_cache_msync(src, raw_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* ── 硬件编码 ── */
    jpeg_encoder_handle_t hw_enc = NULL;
    jpeg_encode_engine_cfg_t hw_eng = { .timeout_ms = 300 };
    uint32_t hw_len = 0;
    if (jpeg_new_encoder_engine(&hw_eng, &hw_enc) == ESP_OK) {
        jpeg_encode_cfg_t cfg = {
            .width         = width,
            .height        = height,
            .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = quality,
            .pixel_reverse = false,
        };
        int64_t t[BENCH_RUNS];
        out->hw_ok = true;
        for (int i = 0; i < BENCH_RUNS; i++) {
            uint32_t n = 0;
            int64_t t0 = esp_timer_get_time();
            esp_err_t err = jpeg_encoder_process(hw_enc, &cfg, (const uint8_t *)src,
                                                 (uint32_t)raw_bytes, hw_out,
                                                 (uint32_t)hw_out_cap, &n);
            t[i] = esp_timer_get_time() - t0;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "hw encode run %d failed: %s", i, esp_err_to_name(err));
                out->hw_ok = false;
                break;
            }
            hw_len = n;
        }
        if (out->hw_ok) {
            out->hw_us = median5(t);
            out->hw_bytes = hw_len;
            /* 码流在 CPU 缓存里，解码器走 DMA 读 PSRAM，需写回。
             * msync 要求长度是 cache line（128B）整数倍：向上取整，
             * 不能向下取整（会截掉码流尾部）。 */
            size_t wb = ((size_t)hw_len + 127u) & ~(size_t)127u;
            if (wb > hw_out_cap) wb = hw_out_cap & ~(size_t)127u;
            esp_cache_msync(hw_out, wb, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            ESP_LOGI(TAG, "HW encode: median %lld us, %u bytes, quality %u",
                     (long long)out->hw_us, (unsigned)out->hw_bytes, quality);
        }
        jpeg_del_encoder_engine(hw_enc);
    } else {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed");
    }

    /* ── 软件编码 ── */
    jpeg_enc_handle_t sw_enc = NULL;
    jpeg_enc_config_t sw_cfg = {
        .width             = (int)width,
        .height            = (int)height,
        .src_type          = JPEG_PIXEL_FORMAT_RGB565_LE,
        .subsampling       = JPEG_SUBSAMPLE_420,
        .quality           = quality,
        .rotate            = JPEG_ROTATE_0D,
        .task_enable       = false,
        .hfm_task_priority = 13,
        .hfm_task_core     = 1,
    };
    uint32_t sw_len = 0;
    if (jpeg_enc_open(&sw_cfg, &sw_enc) == JPEG_ERR_OK) {
        int64_t t[BENCH_RUNS];
        out->sw_ok = true;
        for (int i = 0; i < BENCH_RUNS; i++) {
            int n = 0;
            int64_t t0 = esp_timer_get_time();
            jpeg_error_t err = jpeg_enc_process(sw_enc, (const uint8_t *)src,
                                                (int)raw_bytes, sw_out,
                                                (int)cap_bytes, &n);
            t[i] = esp_timer_get_time() - t0;
            if (err != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "sw encode run %d failed: %d", i, (int)err);
                out->sw_ok = false;
                break;
            }
            sw_len = (uint32_t)n;
        }
        if (out->sw_ok) {
            out->sw_us = median5(t);
            out->sw_bytes = sw_len;
            /* 软件编码是 CPU 写内存：刷到 PSRAM，硬件解码器才能读到 */
            size_t wb = ((size_t)sw_len + 127u) & ~(size_t)127u;
            if (wb > cap_bytes) wb = cap_bytes & ~(size_t)127u;
            esp_cache_msync(sw_out, wb, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            ESP_LOGI(TAG, "SW encode: median %lld us, %u bytes, quality %u",
                     (long long)out->sw_us, (unsigned)out->sw_bytes, quality);
        }
        jpeg_enc_close(sw_enc);
    } else {
        ESP_LOGE(TAG, "jpeg_enc_open failed");
    }

    /* ── 回放验证：两条码流都用硬件解码器解回 RGB565 并算 PSNR ── */
    jpeg_decoder_handle_t dec = NULL;
    jpeg_decode_engine_cfg_t dec_eng = { .timeout_ms = 300 };
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    if (jpeg_new_decoder_engine(&dec_eng, &dec) == ESP_OK) {
        uint32_t dec_len = 0;
        if (out->hw_bytes > 0 &&
            jpeg_decoder_process(dec, &dec_cfg, hw_out, out->hw_bytes, (uint8_t *)decoded,
                                 (uint32_t)dec_cap, &dec_len) == ESP_OK &&
            dec_len >= raw_bytes) {
            out->hw_decoded = true;
            out->hw_psnr_db = psnr_rgb565(src, decoded, width * height);
        }
        dec_len = 0;
        if (out->sw_bytes > 0 &&
            jpeg_decoder_process(dec, &dec_cfg, sw_out, out->sw_bytes, (uint8_t *)decoded,
                                 (uint32_t)dec_cap, &dec_len) == ESP_OK &&
            dec_len >= raw_bytes) {
            out->sw_decoded = true;
            out->sw_psnr_db = psnr_rgb565(src, decoded, width * height);
        }
        jpeg_del_decoder_engine(dec);
    } else {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed");
    }

    free(src);
    free(hw_out);
    free(sw_out);
    free(decoded);
    return ESP_OK;
}
