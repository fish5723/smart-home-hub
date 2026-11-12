/**
 * @file f103_iap.h
 * @brief P4 → F103 IAP 发送器（SD 卡镜像 + 逐块 OTA + 进度）
 *
 * 阶段 8：从 /sdcard/ota/f103-node.bin 读取 app 镜像，校验后按 128B
 * 通过现有 P4↔F103 UART 发送。F103 收到后写 W25Q64 暂存、备份旧固件、
 * 写 PENDING 并复位；bootloader 负责搬运和回滚。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    F103_IAP_IDLE = 0,
    F103_IAP_RUNNING,
    F103_IAP_SUCCESS,
    F103_IAP_FAILED,
} f103_iap_status_t;

/** 创建 OTA_RESULT 队列；在 AppProto_Init 之后调用。 */
esp_err_t f103_iap_init(void);

/** 注册到协议层：AppProto_SetOtaResultHandler(f103_iap_on_result)。 */
void f103_iap_on_result(const proto_frame_t *frame);

/** 启动一次升级（后台任务）。要求链路 UP、SD 已挂载、当前无升级任务。 */
esp_err_t f103_iap_start(void);

f103_iap_status_t f103_iap_get_status(void);
int               f103_iap_get_progress(void);   /* 0..100 */
bool              f103_iap_is_running(void);

#ifdef __cplusplus
}
#endif
