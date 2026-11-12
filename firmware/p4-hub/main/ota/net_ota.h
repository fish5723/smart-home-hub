/**
 * @file net_ota.h
 * @brief P4 网络 OTA（HTTP/HTTPS manifest + SHA256 + 防降级 + F103 固件缓存）
 *
 * 阶段 8 补充：在 SD 卡 OTA 之外增加网络升级路径。
 *   1. 从 manifest URL 读取 p4/f103 两个镜像的版本、大小、SHA256；
 *   2. P4：版本比当前新才下载到非活动分区，边下边算 SHA256，校验后
 *      set_boot 并重启（回滚仍由现有 esp_ota + bootloader 机制保证）；
 *   3. F103：下载到 /sdcard/ota/f103-node.bin（临时文件校验后改名），
 *      供面板现有「F103 固件升级」使用。
 *
 * 触发：开机自动检查一次、定时轮询、面板手动检查（net_ota_check_now）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_OTA_IDLE = 0,
    NET_OTA_CHECKING,
    NET_OTA_UP_TO_DATE,
    NET_OTA_DOWNLOADING,
    NET_OTA_SUCCESS,        /* 已写入新镜像，即将重启 */
    NET_OTA_FAILED,
    NET_OTA_OFFLINE,        /* Wi-Fi 未连接 / 服务器不可达 */
} net_ota_status_t;

/** 一次检查的最终结果快照（供 MQTT 上报） */
typedef struct {
    net_ota_status_t status;
    char current[40];       /* 当前运行版本 */
    char target[40];        /* manifest 目标版本（UP_TO_DATE 时同 current） */
    char sha256[65];        /* 目标镜像 SHA256（无目标时为空） */
    char error[32];         /* 失败原因（成功/无需更新时为空） */
    int64_t ts_us;          /* esp_timer 时间戳 */
} net_ota_result_t;

typedef void (*net_ota_result_cb_t)(const net_ota_result_t *result);

/** 注册结果回调（在 net_ota 后台任务上下文执行，必须非阻塞）。 */
void net_ota_set_result_cb(net_ota_result_cb_t cb);

/** 最近一次检查结果；从未检查过时 status=IDLE。 */
const net_ota_result_t *net_ota_get_last_result(void);

/** 初始化：创建后台任务（需在 Wi-Fi 初始化之后调用）。 */
esp_err_t net_ota_init(void);

/** 手动触发一次检查（非阻塞）；返回 ESP_ERR_INVALID_STATE 表示正在执行。 */
esp_err_t net_ota_check_now(void);

net_ota_status_t net_ota_get_status(void);

/** 当前下载进度 0..100（仅 DOWNLOADING 有意义）。 */
int net_ota_get_progress(void);

/** 是否正在检查/下载（面板据此禁用按钮）。 */
bool net_ota_is_busy(void);

#ifdef __cplusplus
}
#endif
