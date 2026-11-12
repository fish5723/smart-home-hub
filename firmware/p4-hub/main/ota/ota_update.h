/**
 * @file ota_update.h
 * @brief P4 双分区 OTA（SD 卡触发 + bootloader 回滚）
 *
 * 适用场景：面板没有可用的网络 OTA 通道时，用 SD 卡投放固件。
 * 流程：启动时检查 CONFIG_SMART_HOME_OTA_SD_PATH → 校验镜像 → 写入非活动
 * OTA 分区 → 设置启动分区 → 重启。新镜像必须在系统自检通过后调用
 * ota_update_mark_valid()，否则 bootloader 会在下一次复位时回滚。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 打印当前运行分区/版本；检测上次 OTA 是否已回滚。应在 NVS 初始化后调用。 */
esp_err_t ota_update_init(void);

/** 系统自检通过后调用：把当前镜像标记为 valid，取消 bootloader 回滚。 */
esp_err_t ota_update_mark_valid(void);

/** 检查 SD 卡 OTA 文件；存在则写入非活动分区并重启。
 *  无文件返回 ESP_ERR_NOT_FOUND；SD 未挂载返回 ESP_ERR_INVALID_STATE。 */
esp_err_t ota_update_check_sd(void);

/** 当前运行分区 label（如 ota_0/ota_1，只读）。 */
const char *ota_update_running_label(void);

/** 当前运行镜像版本（esp_app_desc_t.version，只读）。 */
const char *ota_update_running_version(void);

#ifdef __cplusplus
}
#endif
