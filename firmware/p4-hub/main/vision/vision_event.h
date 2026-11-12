/**
 * @file vision_event.h
 * @brief 阶段七 P4：PIR 事件驱动的人脸检测
 *
 * 流程：F103 的 PROTO_DEV_HUMAN 0→1 上升沿 → 全屏预览（滑入动画）
 *       → 抓 3 帧检测 → 角标显示 FACE/NO FACE → 5 s 后滑出恢复
 * 冷却 30 s；摄像头预览本身保持常开（本板无摄像头电源开关）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建事件任务（在协议/摄像头初始化之后调用）。 */
esp_err_t vision_event_init(void);

/** 主循环 10 ms 调用：轮询 PIR 影子，检测上升沿。 */
void vision_event_tick(void);

/** 在 LVGL 任务内调用：把摄像头图像交给事件层做全屏覆盖（可为 NULL）。 */
void vision_event_attach_ui(lv_obj_t *cam_img);

#ifdef __cplusplus
}
#endif
