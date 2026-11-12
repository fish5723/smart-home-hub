/**
 * @file face_detect.h
 * @brief 端侧人脸检测（esp-dl HumanFaceDetect，模型由 Kconfig 选择）
 *
 * 模型：`MSRMNP_S8_V1`（160×120，约 18–24 ms/帧）或
 * `ESPDET_PICO_416_416_FACE`（416×416，约 230 ms/帧），互斥选择。
 * 输入是摄像头 ISP 输出的 RGB565 整帧（800×640，小端）；模型内部做 letterbox
 * 缩放。输出保留「数量 + 最高分 + 最高分框」，并额外带回全部检出的框，
 * 供离线评测统计 TP/FP/FN（面板/规则仍只使用 count/best 字段）。
 *
 * 首次调用会加载模型（约 1 s 量级），之后单帧推理。
 * 每个调用需在任务上下文执行（MSRMNP 约 20 ms，ESPDET 416 约 230 ms，不可放 ISR）。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_DETECT_MAX_BOXES 16

typedef struct {
    int   x, y, w, h;  /*!< 源图像素坐标 */
    float score;
} face_detect_box_t;

typedef struct {
    int      count;       /*!< 检出人脸数量 */
    float    best_score;  /*!< 最高置信度；count==0 时为 0 */
    int      x, y, w, h;  /*!< 最高分人脸的框（源图像素坐标）；count==0 时为 0 */
    int      best_area;   /*!< 最高分人脸的框面积（像素²） */
    int64_t  detect_us;   /*!< 本次调用端到端耗时（含预处理与后处理） */
    int      box_count;   /*!< boxes[] 中实际写入的框数（最多 FACE_DETECT_MAX_BOXES） */
    face_detect_box_t boxes[FACE_DETECT_MAX_BOXES];
} face_detect_result_t;

/** 创建检测器（不加载模型，首次推理时惰性加载）。可重复调用。 */
esp_err_t face_detect_init(void);

/** 对一帧 RGB565（小端）做检测。width/height 为源图尺寸。 */
esp_err_t face_detect_frame_rgb565(const uint16_t *rgb565, int width, int height,
                                   face_detect_result_t *out);

/** 调整检测阈值，用于现场调试；默认 0.3/0.7（厂商默认 0.5/0.7）。 */
void face_detect_set_threshold(float score, float nms);

#ifdef __cplusplus
}
#endif
