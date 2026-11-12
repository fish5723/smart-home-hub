/**
 * @file app_f103_ota.h
 * @brief F103 IAP 接收端 — 通过 P4 转发把新固件写入 W25Q64 暂存区
 *
 * 阶段 8：W25Q64 暂存 + 旧固件备份 + 单一内部 app 槽。
 * 协议载荷见 APP/comm/protocol.h 的 PROTO_OTA_* 定义。
 */
#ifndef APP_F103_OTA_H
#define APP_F103_OTA_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 W25Q64、读取 JEDEC ID、安排启动确认。 */
void AppF103Ota_Init(void);

/** 最近一次读到的 W25Q64 JEDEC ID（0 = 未读到）。 */
uint32_t AppF103Ota_GetStorageId(void);

/** 存储是否可信：JEDEC ID 匹配 Winbond 0xEF40xx 且容量 ≥ 1MB。 */
int AppF103Ota_StorageReady(void);

/** 是否正在接收/校验/备份（期间暂停 SENSOR_REPORT）。 */
int AppF103Ota_IsActive(void);

/** 主循环轮询：接收超时、启动 10s 后确认 TESTING→CONFIRMED。 */
void AppF103Ota_Poll(void);

/** 处理 OTA 帧；返回 1 表示该帧属于 OTA 并已处理。 */
int AppF103Ota_HandleFrame(const proto_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* APP_F103_OTA_H */
