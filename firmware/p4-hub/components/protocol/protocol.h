/**
 * @file protocol.h
 * @brief P4↔F103 二进制通信协议定义（与 F103 同源一致）
 *
 * 帧格式（小端序）:
 *   SOF(2B) VER(1B) TYPE(1B) SEQ(2B) LEN(2B) PAYLOAD(LEN) CRC16(2B)
 *
 * 来源: docs/03-通信协议规范.md v2.0-draft
 * 阶段: 阶段 2B — P4 侧协议栈独立实现
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  帧格式常量
 *  ════════════════════════════════════════════════════ */

#define PROTO_SOF1          0xAAU
#define PROTO_SOF2          0x55U
#define PROTO_VER           0x02U
#define PROTO_HEADER_SIZE   8U
#define PROTO_CRC_SIZE      2U
#define PROTO_MIN_FRAME     10U
#define PROTO_MAX_PAYLOAD   256U
#define PROTO_MAX_FRAME     (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CRC_SIZE)

/* ════════════════════════════════════════════════════
 *  超时与重传参数
 *  ════════════════════════════════════════════════════ */

#define PROTO_ACK_TIMEOUT_MS    100U
#define PROTO_MAX_RETRANS       3U
#define PROTO_IDLE_TIMEOUT_MS   20U

/* ════════════════════════════════════════════════════
 *  去重表
 *  ════════════════════════════════════════════════════ */

#define PROTO_DEDUP_SIZE        8U

/* ════════════════════════════════════════════════════
 *  消息类型
 *  ════════════════════════════════════════════════════ */

#define PROTO_HELLO_REQ         0x01U
#define PROTO_HELLO_RSP         0x02U
#define PROTO_PING              0x03U
#define PROTO_PONG              0x04U
#define PROTO_NACK              0x7EU
#define PROTO_ERROR             0x7FU

#define PROTO_SENSOR_REPORT     0x10U
#define PROTO_SENSOR_REPORT_ACK 0x11U
#define PROTO_ALARM_EVENT       0x12U
#define PROTO_ALARM_EVENT_ACK   0x13U

#define PROTO_ACTUATOR_CMD      0x20U
#define PROTO_ACTUATOR_CMD_ACK  0x21U
#define PROTO_ACTUATOR_STATUS   0x22U

#define PROTO_CONFIG_GET        0x30U
#define PROTO_CONFIG_SET        0x31U
#define PROTO_CONFIG_DATA       0x32U
#define PROTO_STATS_GET         0x33U
#define PROTO_STATS_DATA        0x34U
#define PROTO_LOG_FRAGMENT      0x50U
#define PROTO_OTA_BEGIN         0x60U
#define PROTO_OTA_DATA          0x61U
#define PROTO_OTA_END           0x62U
#define PROTO_OTA_RESULT        0x63U
#define PROTO_OTA_ABORT         0x64U

/* OTA 固定分块：128B，保证 W25Q64 页内写入不跨页 */
#define PROTO_OTA_CHUNK_SIZE    128U

/* OTA_RESULT.stage */
#define PROTO_OTA_STAGE_BEGIN   0U
#define PROTO_OTA_STAGE_DATA    1U
#define PROTO_OTA_STAGE_END     2U
#define PROTO_OTA_STAGE_ABORT   3U
#define PROTO_OTA_STAGE_INFO    4U

/* OTA_RESULT.status */
#define PROTO_OTA_OK            0U
#define PROTO_OTA_ERR_PARAM     1U
#define PROTO_OTA_ERR_BUSY      2U
#define PROTO_OTA_ERR_STATE     3U
#define PROTO_OTA_ERR_STORAGE   4U
#define PROTO_OTA_ERR_CRC       5U
#define PROTO_OTA_ERR_SIZE      6U
#define PROTO_OTA_ERR_VERIFY    7U

/* ════════════════════════════════════════════════════
 *  错误码
 *  ════════════════════════════════════════════════════ */

#define PROTO_ERR_CRC            0x01U
#define PROTO_ERR_LEN            0x02U
#define PROTO_ERR_UNKNOWN_TYPE   0x03U
#define PROTO_ERR_VERSION        0x04U
#define PROTO_ERR_RX_FULL        0x05U

#define PROTO_ERR_UNKNOWN_DEV    0x10U
#define PROTO_ERR_PARAM          0x11U
#define PROTO_ERR_BUSY           0x12U
#define PROTO_ERR_DEV_FAULT      0x13U
#define PROTO_ERR_SAFETY_LOCK    0x14U

#define PROTO_ERR_EXEC_TIMEOUT   0x20U
#define PROTO_ERR_NOT_READY      0x21U
#define PROTO_ERR_STORAGE        0x22U
#define PROTO_ERR_STATE_MISMATCH 0x23U

/* ════════════════════════════════════════════════════
 *  设备/通道 ID
 *  ════════════════════════════════════════════════════ */

#define PROTO_DEV_RELAY1         0x01U   /* F103 PB14 */
#define PROTO_DEV_RELAY2         0x02U   /* F103 PB15 */
#define PROTO_DEV_LOCK           0x03U   /* F103 PB8 */
#define PROTO_DEV_BEEPER         0x04U   /* F103 PB12 */
#define PROTO_DEV_DHT11_TEMP     0x10U
#define PROTO_DEV_DHT11_HUM      0x11U
#define PROTO_DEV_BMP280_TEMP    0x12U
#define PROTO_DEV_BMP280_PRESS   0x13U
#define PROTO_DEV_MQ135          0x14U
#define PROTO_DEV_MQ4            0x15U
#define PROTO_DEV_SOIL           0x16U
#define PROTO_DEV_RAIN           0x17U
#define PROTO_DEV_HUMAN          0x18U
/* P4 本地伪传感器（不走上行协议）：摄像头人脸确认结果，供本地规则判定。
 * 0 = 未检出/无事件，1 = 本次事件检出人脸。F103 侧无此 ID。 */
#define PROTO_DEV_FACE           0x19U

/* ════════════════════════════════════════════════════
 *  告警 ID / 级别（ALARM_EVENT 载荷，见 docs/03 §5.2）
 *  ════════════════════════════════════════════════════ */

#define PROTO_ALARM_GAS_MQ4      0x01U   /* 可燃气(MQ-4)超标     — 紧急 */
#define PROTO_ALARM_AIR_MQ135    0x02U   /* 空气质量(MQ-135)超标 — 警告 */
#define PROTO_ALARM_PUMP         0x03U   /* 水泵超时/干烧保护    — 警告 */
#define PROTO_ALARM_LOCK_FAIL    0x04U   /* 钛丝锁动作失败       — 警告 */
#define PROTO_ALARM_SENSOR       0x05U   /* 传感器故障/离线       — 提示 */
#define PROTO_ALARM_LINK         0x06U   /* 通信链路中断         — 警告 */
#define PROTO_ALARM_STORAGE      0x07U   /* 存储写入失败         — 警告 */

#define PROTO_LEVEL_HINT         0U      /* 提示 */
#define PROTO_LEVEL_WARN         1U      /* 警告 */
#define PROTO_LEVEL_URGENT       2U      /* 紧急 */

/* ════════════════════════════════════════════════════
 *  quality 字段
 *  ════════════════════════════════════════════════════ */

#define PROTO_QUAL_VALID         0U
#define PROTO_QUAL_WARMUP        1U
#define PROTO_QUAL_OVERFLOW      2U
#define PROTO_QUAL_FAULT         3U
#define PROTO_QUAL_STALE         4U

/* ════════════════════════════════════════════════════
 *  动作/结果
 *  ════════════════════════════════════════════════════ */

#define PROTO_ACTION_OFF         0U
#define PROTO_ACTION_ON          1U
#define PROTO_ACTION_PULSE       2U
#define PROTO_ACTION_SET         3U
#define PROTO_ACTION_QUERY       4U

#define PROTO_RESULT_OK          0U
#define PROTO_RESULT_ERR_PARAM   1U
#define PROTO_RESULT_ERR_BUSY    2U
#define PROTO_RESULT_ERR_FAULT   3U
#define PROTO_RESULT_ERR_SAFETY  4U
#define PROTO_RESULT_ERR_TTL     5U

/* ════════════════════════════════════════════════════
 *  帧结构体
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  ver;
    uint8_t  type;
    uint16_t seq;
    uint16_t len;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
    uint16_t crc;
} proto_frame_t;

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
