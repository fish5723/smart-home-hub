/**
 * @file face_detect.cpp
 * @brief 端侧人脸检测封装（esp-dl HumanFaceDetect，模型由 Kconfig 选择）
 *
 * esp-dl 是 C++ 接口，这里用 extern "C" 暴露给 C 侧（app_main / 规则引擎）。
 * 模型位置与型号由 Kconfig 决定（MSRMNP_S8_V1 或 ESPDET_PICO_416_416_FACE），
 * RODATA 嵌入。
 */
#include "face_detect.h"

#include <list>

#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "human_face_detect.hpp"
#include "sdkconfig.h"

static const char *TAG = "FACE_DETECT";

static HumanFaceDetect *s_det = nullptr;
/* 模型实例被 PIR 事件任务和 SD 批量任务共用：run() 不是线程安全的，
 * 这里串行化，避免同一时刻两个任务同时进入推理。 */
static SemaphoreHandle_t s_run_mtx = nullptr;

esp_err_t face_detect_init(void)
{
    if (s_run_mtx == nullptr) {
        s_run_mtx = xSemaphoreCreateMutex();
    }
    if (s_det != nullptr) {
        return ESP_OK;
    }
#if CONFIG_HUMAN_FACE_DETECT_MSRMNP_S8_V1
    /* P3 基线：MSR+MNP 两阶段，输入 160×120，实测 18–24 ms/帧；
     * 远距离召回较弱。NMS 沿用 P3 现场复核的 0.3。 */
    s_det = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, true);
    face_detect_set_threshold(0.3f, 0.3f);
    ESP_LOGI(TAG, "HumanFaceDetect(MSRMNP_S8_V1) created, score_thr=0.3 nms_thr=0.3");
#else
    /* ESPDet PICO 416：416×416 单阶段，远距离召回更好，单帧约 230 ms。 */
    s_det = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_416_416_FACE, true);
    face_detect_set_threshold(0.3f, 0.7f);
    ESP_LOGI(TAG, "HumanFaceDetect(ESPDET_PICO_416_416_FACE) created, score_thr=0.3 nms_thr=0.7");
#endif
    if (s_det == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t face_detect_frame_rgb565(const uint16_t *rgb565, int width, int height,
                                   face_detect_result_t *out)
{
    if (s_det == nullptr || rgb565 == nullptr || out == nullptr ||
        width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    dl::image::img_t img = {};
    img.data     = const_cast<uint16_t *>(rgb565);
    img.width    = static_cast<uint16_t>(width);
    img.height   = static_cast<uint16_t>(height);
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

    if (s_run_mtx) {
        xSemaphoreTake(s_run_mtx, portMAX_DELAY);
    }
    int64_t t0 = esp_timer_get_time();
    std::list<dl::detect::result_t> &results = s_det->run(img);
    int64_t dt = esp_timer_get_time() - t0;

    out->count      = static_cast<int>(results.size());
    out->best_score = 0.0f;
    out->best_area  = 0;
    out->x = out->y = out->w = out->h = 0;
    out->detect_us  = dt;

    out->box_count  = 0;
    for (const auto &r : results) {
        if (r.score > out->best_score) {
            out->best_score = r.score;
            out->x = r.box[0];
            out->y = r.box[1];
            out->w = r.box[2] - r.box[0];
            out->h = r.box[3] - r.box[1];
            out->best_area = r.box_area();
        }
        if (out->box_count < FACE_DETECT_MAX_BOXES) {
            face_detect_box_t *b = &out->boxes[out->box_count++];
            b->x = r.box[0];
            b->y = r.box[1];
            b->w = r.box[2] - r.box[0];
            b->h = r.box[3] - r.box[1];
            b->score = r.score;
        }
    }
    if (s_run_mtx) {
        xSemaphoreGive(s_run_mtx);
    }
    return ESP_OK;
}

void face_detect_set_threshold(float score, float nms)
{
    if (s_det == nullptr) {
        return;
    }
    s_det->set_score_thr(score, 0);
    s_det->set_score_thr(score, 1);
    s_det->set_nms_thr(nms, 0);
    s_det->set_nms_thr(nms, 1);
}
