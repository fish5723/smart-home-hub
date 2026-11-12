/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * jwp_reader.c — JWP 容器解析器实现
 *
 * 参考来源: https://github.com/maxiumlee/ESP32-S3-LVGL-Clock (Apache-2.0),
 *           HEAD 299ab2e63ea85f3463edc8fd8fd75142573c50fa。
 * 仅移植容器格式与顺序读取算法，未复制其 Arduino 工程。
 */

#include "jwp_reader.h"

#include <string.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
/* Host tests exercise the parser without pulling in the ESP-IDF SDK. */
#define taskYIELD() ((void)0)
#endif
#include "esp_log.h"

static const char *TAG = "JWP";

/* 分块读取：SDMMC Slot 0（SD 卡）与 Slot 1（C6 SDIO）共用同一主机，
 * 一次 fread 读完整帧（60~130KB）会长时间占住总线，期间 C6 的 SDIO
 * 收发包排队到 mempool 耗尽（eh_sdio: mempool OOM (TX/RX)）→ 丢包 →
 * TCP 写阻塞数十秒 → broker 踢线。8KB 一块并在块间 taskYIELD()，
 * 让高优先级的 esp_hosted 任务插进来收发。 */
#define JWP_READ_CHUNK  (8u * 1024u)

static bool read_chunked(FILE *f, uint8_t *dst, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = (len - done > JWP_READ_CHUNK) ? JWP_READ_CHUNK : (len - done);
        if (fread(dst + done, 1, n, f) != n) {
            return false;
        }
        done += n;
        if (done < len) {
            taskYIELD();
        }
    }
    return true;
}

/* ── little-endian 读取辅助（不依赖结构体对齐/打包） ── */
static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t jwp_reader_open(jwp_reader_t *reader, const char *path)
{
    if (!reader || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(reader, 0, sizeof(*reader));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * JWP is consumed sequentially.  A modest stdio read buffer reduces
     * FatFS/SDMMC transaction overhead between the 4-byte length and JPEG
     * payload reads, without changing the container format or frame data.
     */
    (void)setvbuf(f, NULL, _IOFBF, 16u * 1024u);

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size < (long)JWP_HEADER_SIZE || (uint64_t)size > UINT32_MAX) {
        ESP_LOGE(TAG, "file too small: %ld bytes", size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hdr[JWP_HEADER_SIZE];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ESP_FAIL;
    }

    if (memcmp(hdr, JWP_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "bad magic in %s", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    reader->version           = rd_u16(hdr + 4);
    reader->header_size       = rd_u16(hdr + 6);
    reader->width             = rd_u16(hdr + 8);
    reader->height            = rd_u16(hdr + 10);
    reader->fps_times_100     = rd_u16(hdr + 12);
    reader->pixel_format      = rd_u16(hdr + 14);
    reader->frame_count       = rd_u32(hdr + 16);
    reader->max_frame_bytes   = rd_u32(hdr + 20);
    reader->payload_crc32     = rd_u32(hdr + 24);
    reader->transition_offset = rd_u32(hdr + 28);

    if (reader->version != JWP_VERSION) {
        ESP_LOGE(TAG, "unsupported version %u", reader->version);
        fclose(f);
        return ESP_ERR_INVALID_VERSION;
    }
    if (reader->header_size != JWP_HEADER_SIZE) {
        ESP_LOGE(TAG, "unexpected header_size %u", reader->header_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (reader->pixel_format != JWP_PIXEL_FORMAT_BASELINE_JPEG) {
        ESP_LOGE(TAG, "unsupported pixel_format %u", reader->pixel_format);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (reader->width == 0 || reader->height == 0 ||
        reader->frame_count == 0 || reader->max_frame_bytes == 0) {
        ESP_LOGE(TAG, "invalid header (w=%u h=%u frames=%lu max=%lu)",
                 reader->width, reader->height,
                 (unsigned long)reader->frame_count,
                 (unsigned long)reader->max_frame_bytes);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Walk every length prefix before trusting the frame count or loop offset. */
    uint64_t offset = reader->header_size;
    bool loop_is_frame = reader->transition_offset == 0;
    if (reader->frame_count > ((uint64_t)size - offset) / 5 ||
        reader->max_frame_bytes > (uint64_t)size - offset - 4) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t i = 0; i < reader->frame_count; i++) {
        uint8_t lenb[4];
        if (offset == reader->transition_offset) loop_is_frame = true;
        if (offset + 4 > (uint64_t)size ||
            fseek(f, (long)offset, SEEK_SET) != 0 ||
            fread(lenb, 1, 4, f) != 4) {
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t len = rd_u32(lenb);
        if (len == 0 || len > reader->max_frame_bytes ||
            (uint64_t)len > (uint64_t)size - offset - 4) {
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
        offset += 4 + (uint64_t)len;
    }
    if (offset != (uint64_t)size || !loop_is_frame) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    reader->file      = f;
    reader->file_size = (uint32_t)size;
    reader->data_offset = reader->header_size;
    reader->loop_offset = reader->transition_offset ?
                          reader->transition_offset : reader->data_offset;
    reader->current_frame = 0;

    if (fseek(f, (long)reader->data_offset, SEEK_SET) != 0) {
        jwp_reader_close(reader);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "opened %s: %ux%u @%u.%02u fps, %lu frames, max %lu B, loop@%lu",
             path, reader->width, reader->height,
             reader->fps_times_100 / 100, reader->fps_times_100 % 100,
             (unsigned long)reader->frame_count,
             (unsigned long)reader->max_frame_bytes,
             (unsigned long)reader->loop_offset);
    return ESP_OK;
}

esp_err_t jwp_reader_read_frame(jwp_reader_t *reader,
                                uint8_t *buffer,
                                size_t buffer_size,
                                size_t *out_size)
{
    if (!reader || !reader->file || !buffer) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer_size < reader->max_frame_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (reader->current_frame >= reader->frame_count) {
        jwp_reader_rewind_loop(reader);
    }

    /* 最多两次：第一次失败说明到达循环区域末尾，回跳后再试一次 */
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t lenb[4];
        if (fread(lenb, 1, 4, reader->file) != 4) {
            if (attempt == 0 && jwp_reader_rewind_loop(reader) == ESP_OK) {
                continue;
            }
            return ESP_FAIL;
        }
        uint32_t len = rd_u32(lenb);
        if (len == 0 || len > reader->max_frame_bytes || len > buffer_size) {
            ESP_LOGW(TAG, "bad frame length %lu (max %lu)",
                     (unsigned long)len, (unsigned long)reader->max_frame_bytes);
            if (attempt == 0 && jwp_reader_rewind_loop(reader) == ESP_OK) {
                continue;
            }
            return ESP_ERR_INVALID_SIZE;
        }
        if (!read_chunked(reader->file, buffer, len)) {
            if (attempt == 0 && jwp_reader_rewind_loop(reader) == ESP_OK) {
                continue;
            }
            return ESP_FAIL;
        }
        reader->current_frame++;
        if (out_size) {
            *out_size = len;
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t jwp_reader_seek_frame(jwp_reader_t *reader, uint32_t frame)
{
    if (!reader || !reader->file) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame >= reader->frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(reader->file, (long)reader->data_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    for (uint32_t i = 0; i < frame; i++) {
        uint8_t lenb[4];
        if (fread(lenb, 1, 4, reader->file) != 4) {
            return ESP_FAIL;
        }
        uint32_t len = rd_u32(lenb);
        if (len == 0 || len > reader->max_frame_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (fseek(reader->file, (long)len, SEEK_CUR) != 0) {
            return ESP_FAIL;
        }
    }
    reader->current_frame = frame;
    return ESP_OK;
}

esp_err_t jwp_reader_rewind_loop(jwp_reader_t *reader)
{
    if (!reader || !reader->file) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fseek(reader->file, (long)reader->loop_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    reader->current_frame = 0;
    return ESP_OK;
}

uint32_t jwp_reader_loop_offset(const jwp_reader_t *reader)
{
    return reader ? reader->loop_offset : 0u;
}

void jwp_reader_close(jwp_reader_t *reader)
{
    if (!reader) {
        return;
    }
    if (reader->file) {
        fclose(reader->file);
    }
    memset(reader, 0, sizeof(*reader));
}

uint32_t jwp_crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static int table_ready = 0;
    const uint8_t *p = (const uint8_t *)data;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = 1;
    }

    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}
