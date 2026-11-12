/**
 * @file face_batch.h
 * @brief 阶段七：SD 卡批量读图 + 端侧人脸检测入口
 *
 * 数据来源：/sdcard/images/positives 与 /sdcard/images/negatives 下的 JPG
 * （F 盘根目录的 images/labels.json/negatives.json 直接拷到 SD 卡根目录即可）。
 * 每张图：硬件 JPEG 解码 → RGB565 → 端侧检测，串口输出机器可读的 FACE_BATCH 行。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动批量任务（Kconfig SMART_HOME_FACE_BATCH_TEST 开启时由 app_main 调用）。
 *  任务在启动 10 s 后开始，避免与摄像头自检/UI 初始化抢资源。 */
esp_err_t face_batch_start(void);

/** 同步跑完全部图片并返回（Kconfig SMART_HOME_FACE_BATCH_STANDALONE 使用）。
 *  调用方负责先完成 SD 卡挂载；不会创建任务，不会返回前打印 summary。 */
esp_err_t face_batch_run_sync(void);

#ifdef __cplusplus
}
#endif
