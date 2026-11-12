/**
 * @file camera_manager.h
 * @brief 摄像头管理器 — "唯一 Camera Owner" 模式
 *
 * 提供应用层摄像头操作的高级接口:
 *   - 预览启停 (自动注入到 LVGL)
 *   - 单帧捕获
 *   - LVGL 图像显示集成
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 摄像头管理器事件 ── */
typedef enum {
    CAM_MGR_EVENT_PREVIEW_STARTED,   /**< 预览已启动 */
    CAM_MGR_EVENT_PREVIEW_STOPPED,   /**< 预览已停止 */
    CAM_MGR_EVENT_FRAME_READY,       /**< 新帧就绪 (预览中) */
    CAM_MGR_EVENT_CAPTURE_DONE,      /**< 捕获完成 */
    CAM_MGR_EVENT_ERROR,             /**< 错误 */
} camera_mgr_event_t;

typedef void (*camera_mgr_cb_t)(camera_mgr_event_t event, void *user_data);

/* ── 公开 API ── */

/**
 * @brief 初始化摄像头管理器
 *
 * 内部调用 camera_init()。
 *
 * @return ESP_OK 成功
 */
esp_err_t camera_manager_init(void);

/**
 * @brief 反初始化摄像头管理器
 */
esp_err_t camera_manager_deinit(void);

/**
 * @brief 注册管理器事件回调
 */
esp_err_t camera_manager_register_cb(camera_mgr_cb_t cb, void *user_data);

/**
 * @brief 启动预览 (帧将自动推送到 LVGL 显示)
 *
 * @return ESP_OK 成功
 */
esp_err_t camera_manager_preview_start(void);

/**
 * @brief 停止预览
 */
esp_err_t camera_manager_preview_stop(void);

/**
 * @brief 检查是否正在预览
 */
bool camera_manager_is_previewing(void);

/**
 * @brief 捕获单帧到指定缓冲区
 *
 * @param[out] buf        输出缓冲区 (至少 CAMERA_FRAME_SIZE 字节)
 * @param[in]  timeout_ms 超时 (ms)
 * @return ESP_OK 成功
 */
esp_err_t camera_manager_capture(void *buf, uint32_t timeout_ms);

/**
 * @brief 获取当前帧缓冲 (预览中可用)
 */
const void *camera_manager_get_frame(void);

/**
 * @brief 获取帧大小 (字节)
 */
size_t camera_manager_get_frame_size(void);

/**
 * @brief 创建 LVGL 图像控件用于显示摄像头预览
 *
 * 必须在 LVGL 任务上下文中调用 (例如 ui_create() 中)。
 * 图像数据源为内部显示缓冲区, 由 camera_manager_lvgl_refresh() 更新。
 *
 * @param[in] parent 父对象
 * @param[in] x      位置 X
 * @param[in] y      位置 Y
 * @return lv_obj_t* 图像对象指针, 失败返回 NULL
 */
lv_obj_t *camera_manager_create_lvgl_image(lv_obj_t *parent, int x, int y);

/**
 * @brief 在 LVGL 任务中刷新一帧
 *
 * 应由 LVGL 定时器周期性调用 (例如每 20-30ms),
 * 将摄像头最新帧数据同步到 LVGL 显示缓冲区。
 */
void camera_manager_lvgl_refresh(void);

/**
 * @brief 临时暂停/恢复预览刷新
 *
 * 覆盖层滑入/滑出动画期间暂停，避免 20 ms 级的整屏重绘拖住动画；
 * 预览（CSI 采集）本身不停。
 */
void camera_manager_set_refresh_enabled(bool enabled);

#ifdef __cplusplus
}
#endif