/**
 * @file face_batch.c
 * @brief SD 卡批量读图 + 端侧人脸检测（阶段七验收口径）
 *
 * 流程：递归遍历 /sdcard/images/{positives,negatives} → 读 JPEG 到 PSRAM
 *       → jpeg_decoder_get_info 取原始宽高 → 硬件解码为 RGB565
 *       → face_detect_frame_rgb565（模型内部自带宽高保持 letterbox）
 *       → 串口输出一行 FACE_BATCH，供 PC 侧按 labels.json 统计 TP/FP/FN。
 *
 * 模型与相机事件共用同一个 HumanFaceDetect 实例，因此 face_detect 层加了互斥锁；
 * 本任务与 PIR 事件同时发生时只会串行执行，不会并发调用模型。
 */
#include "face_batch.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face_detect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "common/watchdog.h"

static const char *TAG = "FACE_BATCH";

#define BATCH_START_DELAY_MS 10000
#define BATCH_MAX_DEPTH      3
#define BATCH_MAX_JPEG_BYTES (4U * 1024U * 1024U)
#define BATCH_MAX_PIXELS     (4096U * 4096U)

typedef struct {
    int images;
    int ok;
    int fail;
} batch_stats_t;

static jpeg_decoder_handle_t s_dec;

static bool has_jpg_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 5U && strcasecmp(name + n - 5U, ".jpeg") == 0) return true;
    if (n >= 4U && strcasecmp(name + n - 4U, ".jpg") == 0) return true;
    return false;
}

static void run_one(const char *full, const char *rel, const char *label, batch_stats_t *st)
{
    st->images++;
    wdt_feed();

    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        st->fail++;
        ESP_LOGW(TAG, "open failed: %s", rel);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || (uint32_t)fsz > BATCH_MAX_JPEG_BYTES) {
        fclose(f);
        st->fail++;
        ESP_LOGW(TAG, "bad size %ld: %s", fsz, rel);
        return;
    }

    size_t sz = (size_t)fsz;
    size_t sz_pad = (sz + 127U) & ~(size_t)127U;
    uint8_t *jpeg = heap_caps_aligned_calloc(128, 1, sz_pad,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg == NULL) {
        fclose(f);
        st->fail++;
        ESP_LOGW(TAG, "jpeg buffer alloc failed: %s", rel);
        return;
    }
    size_t got = fread(jpeg, 1, sz, f);
    fclose(f);
    if (got != sz) {
        free(jpeg);
        st->fail++;
        ESP_LOGW(TAG, "read %u/%u: %s", (unsigned)got, (unsigned)sz, rel);
        return;
    }
    /* CPU 写过的码流必须写回 PSRAM，DMA 解码器才能读到 */
    esp_cache_msync(jpeg, sz_pad, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpeg, (uint32_t)sz, &info) != ESP_OK ||
        info.width == 0 || info.height == 0 ||
        (uint64_t)info.width * info.height > BATCH_MAX_PIXELS) {
        free(jpeg);
        st->fail++;
        ESP_LOGW(TAG, "header invalid: %s", rel);
        return;
    }

    size_t raw = (size_t)info.width * info.height * 2U;
    /* 解码器输出按 16 对齐（宽高各向上取整），按 w*h*2 申请会偏小：
     * 实测 1024×683 需要 1024×688×2。这里按对齐后的尺寸申请。 */
    uint32_t stride_w = (info.width + 15U) & ~15U;
    uint32_t stride_h = (info.height + 15U) & ~15U;
    size_t raw_pad = (size_t)stride_w * stride_h * 2U;
    if (raw_pad < raw) raw_pad = raw;
    raw_pad = (raw_pad + 127U) & ~(size_t)127U;
    jpeg_decode_memory_alloc_cfg_t mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t cap = 0;
    uint16_t *pix = (uint16_t *)jpeg_alloc_decoder_mem(raw_pad, &mem, &cap);
    if (pix == NULL || cap < raw_pad) {
        free(pix);
        free(jpeg);
        st->fail++;
        ESP_LOGW(TAG, "decode buffer alloc failed: %s", rel);
        return;
    }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* BGR = small endian：与相机 ISP 的 RGB565LE 内存布局一致。
         * 用 RGB（big endian）会让模型的 R/B 通道错位，ESPDet 分数掉到 0。 */
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_len = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_decoder_process(s_dec, &cfg, jpeg, (uint32_t)sz,
                                         (uint8_t *)pix, (uint32_t)cap, &out_len);
    int64_t dec_us = esp_timer_get_time() - t0;
    if (err != ESP_OK || out_len < raw) {
        free(pix);
        free(jpeg);
        st->fail++;
        ESP_LOGW(TAG, "decode failed (%s, %u/%u): %s",
                 esp_err_to_name(err), (unsigned)out_len, (unsigned)raw, rel);
        return;
    }
    size_t sync = raw_pad;
    if (sync > cap) sync = cap & ~(size_t)127U;
    esp_cache_msync(pix, sync, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    /* 行跨度是 stride_w；本数据集宽度都是 16 对齐，因此可直接按原宽高检测。
     * 若将来出现非对齐宽度，用对齐后的宽高交给模型，避免行错位。 */
    int det_w = (int)info.width;
    int det_h = (int)info.height;
    if (stride_w != info.width) {
        det_w = (int)stride_w;
        det_h = (int)stride_h;
        ESP_LOGW(TAG, "non-aligned width %u → detect as %dx%d: %s",
                 (unsigned)info.width, det_w, det_h, rel);
    }

    face_detect_result_t r;
    err = face_detect_frame_rgb565(pix, det_w, det_h, &r);
    if (err != ESP_OK) {
        free(pix);
        free(jpeg);
        st->fail++;
        ESP_LOGW(TAG, "detect failed (%s): %s", esp_err_to_name(err), rel);
        return;
    }

    /* 机器可读行：路径无空格（数据集文件名均已确认），PC 侧按 label + path 对齐
     * labels.json / negatives.json。box 是最高分框；boxes 是全部检出框
     * （x,y,w,h,score 以 ';' 分隔），用于框级 TP/FP/FN 统计。 */
    char boxes[640];
    size_t boff = 0;
    boxes[0] = '\0';
    for (int i = 0; i < r.box_count && boff < sizeof(boxes) - 1; ++i) {
        boff += (size_t)snprintf(boxes + boff, sizeof(boxes) - boff,
                                 "%s%d,%d,%d,%d,%.3f",
                                 i ? ";" : "",
                                 r.boxes[i].x, r.boxes[i].y,
                                 r.boxes[i].w, r.boxes[i].h,
                                 (double)r.boxes[i].score);
    }
    ESP_LOGI(TAG,
             "image label=%s path=%s w=%u h=%u count=%d best=%.3f box=%d,%d,%d,%d "
             "boxes=%s det_us=%lld dec_us=%lld",
             label, rel, (unsigned)info.width, (unsigned)info.height,
             r.count, (double)r.best_score, r.x, r.y, r.w, r.h,
             boxes, (long long)r.detect_us, (long long)dec_us);
    st->ok++;

    free(pix);
    free(jpeg);
    wdt_feed();
}

static void scan_dir(const char *dir, const char *base, const char *label,
                     int depth, batch_stats_t *st)
{
    if (depth > BATCH_MAX_DEPTH) return;

    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir failed: %s", dir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[320];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat sb;
        if (stat(full, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            scan_dir(full, base, label, depth + 1, st);
        } else if (S_ISREG(sb.st_mode) && has_jpg_ext(ent->d_name)) {
            size_t bl = strlen(base);
            const char *rel = full;
            if (strncmp(full, base, bl) == 0 && full[bl] == '/') rel = full + bl + 1;
            run_one(full, rel, label, st);
        }
    }
    closedir(d);
}

static esp_err_t batch_run_all(void)
{
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted — batch skipped");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_dec != NULL) {
        ESP_LOGW(TAG, "batch already running");
        return ESP_ERR_INVALID_STATE;
    }

    jpeg_decode_engine_cfg_t eng = { .timeout_ms = 500 };
    if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg decoder engine create failed");
        return ESP_FAIL;
    }

    const char *mount = sd_card_get_mount_point();
    char base[64];
    char dir[96];
    snprintf(base, sizeof(base), "%s/images", mount);

    ESP_LOGI(TAG, "start mount=%s", mount);

    batch_stats_t st = { 0 };
    snprintf(dir, sizeof(dir), "%s/positives", base);
    scan_dir(dir, base, "pos", 0, &st);
    snprintf(dir, sizeof(dir), "%s/negatives", base);
    scan_dir(dir, base, "neg", 1, &st);

    ESP_LOGI(TAG, "summary images=%d ok=%d fail=%d",
             st.images, st.ok, st.fail);

    jpeg_del_decoder_engine(s_dec);
    s_dec = NULL;
    return ESP_OK;
}

static void batch_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BATCH_START_DELAY_MS));
    wdt_add_current_task();
    (void)batch_run_all();
    vTaskDelete(NULL);
}

esp_err_t face_batch_run_sync(void)
{
    if (face_detect_init() != ESP_OK) {
        ESP_LOGE(TAG, "face_detect_init failed");
        return ESP_FAIL;
    }
    return batch_run_all();
}

esp_err_t face_batch_start(void)
{
    /* 先串行初始化模型对象：批量任务与 PIR 事件任务共用同一实例，
     * 在 app_main 上下文完成创建可避免两个任务同时 new。 */
    if (face_detect_init() != ESP_OK) {
        ESP_LOGE(TAG, "face_detect_init failed");
        return ESP_FAIL;
    }
    if (xTaskCreate(batch_task, "face_batch", 12288, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "queued (runs %d ms after boot)", BATCH_START_DELAY_MS);
    return ESP_OK;
}
