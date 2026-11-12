/**
 * @file camera.h
 * @brief Camera BSP 模块 — OV5647 摄像头 (MIPI-CSI)
 *
 * 硬件: OV5647 (500万像素, MIPI-CSI 2-lane)
 * 接口: MIPI-CSI + SCCB (I2C, 复用触摸屏 I2C_NUM_1 GPIO7/8)
 *
 * 数据流（2026-10-30 实测修正）:
 *   OV5647 → MIPI-CSI 2-lane → CSI Controller（RAW8，rev v1.3 不做颜色转换）
 *   → ISP（RAW8 → RGB565）→ Frame Buffer（PSRAM，1,024,000 字节/帧）
 *
 * 为什么 CSI 配置里输出仍是 RAW8：本板 P4 芯片 rev **v1.3 < v3.0**，IDF 6.1 的
 * `esp_cam_ctlr_csi.c` 对 rev<3.0 直接禁用 CSI 颜色格式转换
 * （`s_csi_ctlr_format_conversion()` 返回 `ESP_ERR_NOT_SUPPORTED`），
 * 所以 CSI 只能同格式直通，最后由 ISP 完成 RAW8→RGB565。
 *
 * 注意：驱动上报的 `trans->received_size` 按 CSI 输入格式(RAW8)算成 512,000，
 * 但 DMA 实际写满整帧 1,024,000 字节（实测帧缓冲后半段有数据），
 * 因此上层统一按 CAMERA_FRAME_SIZE 处理。
 *
 * 依赖:
 *   - touch.h (共享 I2C 总线)
 *   - esp_cam_sensor (OV5647 驱动)
 *   - esp_cam_ctlr_csi (MIPI-CSI 控制器)
 *   - esp_driver_isp (图像信号处理器)
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 摄像头配置参数 ── */
#define CAMERA_H_RES            800     /**< 水平分辨率 */
#define CAMERA_V_RES            640     /**< 垂直分辨率 */
#define CAMERA_BITS_PER_PIXEL   16      /**< ISP 输出 RGB565 = 16bpp */
#define CAMERA_FRAME_SIZE       (CAMERA_H_RES * CAMERA_V_RES * CAMERA_BITS_PER_PIXEL / 8)  /**< 整帧字节数（RGB565） */
#define CAMERA_FORMAT_NAME      "MIPI_2lane_24Minput_RAW8_800x640_50fps"  /**< OV5647 格式名 */

#define CAMERA_SCCB_FREQ_HZ     100000 /**< SCCB 频率：与 IDF 官方 CSI 用例一致（原 10kHz 实测收不到帧） */
#define CAMERA_MIPI_LDO_CHAN    3      /**< MIPI PHY LDO 通道 ID */
#define CAMERA_MIPI_LDO_MV      2500   /**< MIPI PHY LDO 电压 (mV) */

/* ── 摄像头状态 ── */
typedef enum {
    CAMERA_STATE_UNINIT = 0,    /**< 未初始化 */
    CAMERA_STATE_IDLE,          /**< 已初始化, 未启动 */
    CAMERA_STATE_PREVIEW,       /**< 预览中 (持续出帧) */
    CAMERA_STATE_ERROR,         /**< 错误状态 */
} camera_state_t;

/* ── 帧回调 ── */
typedef void (*camera_frame_cb_t)(const void *frame_buffer, size_t frame_size, void *user_data);

/* ── 公开 API ── */

/**
 * @brief 初始化摄像头
 *
 * 流程:
 *  1. 从 touch 获取共享 I2C 总线
 *  2. 创建 SCCB 接口 (10kHz)
 *  3. 检测并初始化 OV5647 传感器
 *  4. 配置 MIPI-CSI 2-lane RAW8 输入
 *  5. 配置 ISP (RAW8 → RGB565)
 *  6. 分配帧缓冲 (PSRAM)
 *
 * @return ESP_OK 成功, 否则失败
 */
esp_err_t camera_init(void);

/**
 * @brief 反初始化摄像头
 */
esp_err_t camera_deinit(void);

/**
 * @brief 获取摄像头当前状态
 */
camera_state_t camera_get_state(void);

/**
 * @brief 启动预览 (循环出帧)
 *
 * @param[in] cb       每帧完成回调 (在 CSI ISR 上下文中调用)
 * @param[in] user_data 回调用户数据
 * @return ESP_OK 成功
 */
esp_err_t camera_preview_start(camera_frame_cb_t cb, void *user_data);

/**
 * @brief 停止预览
 */
esp_err_t camera_preview_stop(void);

/**
 * @brief 捕获单帧 (阻塞)
 *
 * @param[out] out_buf  输出缓冲区 (至少 CAMERA_FRAME_SIZE 字节)
 * @param[in]  timeout_ms 超时时间 (ms)
 * @return ESP_OK 成功
 */
esp_err_t camera_capture_frame(void *out_buf, uint32_t timeout_ms);

/**
 * @brief 获取当前帧缓冲指针 (预览中有效)
 *
 * @return 帧缓冲指针, 未启动预览时返回 NULL
 */
const void *camera_get_frame_buffer(void);

#ifdef __cplusplus
}
#endif