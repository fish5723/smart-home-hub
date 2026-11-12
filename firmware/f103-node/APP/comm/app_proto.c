/**
 * @file app_proto.c
 * @brief 协议调度层 — 连接传输层与发送管理层
 *
 * 数据流:
 *   ProtoUart RX → app_proto rx_cb → 去重 → ACK/NACK判定 → 业务分发
 *   Scheduler    → AppProto_Poll  → 链路状态机 + ProtoTx_Poll (超时/重传)
 *   Scheduler    → AppProto_SendReportTask → SENSOR_REPORT 帧
 *
 * 链路状态机:
 *   LINK_UP → 3s 无帧 → LINK_DEGRADED → 额外 3s → LINK_DOWN
 *   任何有效帧 → 立即恢复 LINK_UP
 *   状态变化打印到调试串口
 *
 * 来源: docs/12-阶段2准备.md 决策3
 */
#include "app_proto.h"
#include "proto_uart.h"
#include "proto_tx.h"
#include "crc16.h"
#include "app_data.h"
#include "bsp_pins.h"
#include "bsp_beep.h"
#include "act_manager.h"
#include "delay.h"
#include "main.h"
#include "app_f103_ota.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════
 *  能力位图（HELLO_RSP 中上报）
 *  ════════════════════════════════════════════════════ */

#define CAP_RELAY      (1U << 0)   /* 照明继电器 */
#define CAP_PUMP       (1U << 1)   /* 水泵继电器 */
#define CAP_LOCK       (1U << 2)   /* 钛丝锁 */
#define CAP_BEEPER     (1U << 3)   /* 蜂鸣器 */
#define CAP_DHT11      (1U << 4)   /* 温湿度 */
#define CAP_BMP280     (1U << 5)   /* 气压温度 */
#define CAP_SOIL       (1U << 6)   /* 土壤湿度 */
#define CAP_RAIN       (1U << 7)   /* 雨滴检测 */
#define CAP_MQ         (1U << 8)   /* MQ 气体传感器 */
#define CAP_PIR        (1U << 9)   /* 人体红外 */
#define CAP_OLED       (1U << 10)  /* OLED 显示屏 */
#define CAP_EEPROM     (1U << 11)  /* EEPROM 存储 */

#define F103_CAPS  (CAP_RELAY | CAP_PUMP | CAP_LOCK | CAP_BEEPER |  \
                    CAP_DHT11 | CAP_BMP280 | CAP_SOIL | CAP_RAIN |  \
                    CAP_MQ | CAP_PIR | CAP_OLED | CAP_EEPROM)

#define F103_DEV_NAME  "F103-HUB"

/* ════════════════════════════════════════════════════
 *  执行器 TTL 默认值
 *  ════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════
 *  链路状态机
 *  ════════════════════════════════════════════════════ */

#define LINK_DEGRADED_MS  3000U
#define LINK_DOWN_MS      6000U

static link_state_t  s_link_state      = LINK_DOWN;
static uint32_t      s_link_last_rx_ms = 0;
static uint32_t      s_link_enter_ms   = 0;
static link_stats_t  s_link_stats;

/* 前向声明：告警重发轮询（完整定义见文件后部"告警上报"一节） */
static void alarm_retry_poll(void);

/* ── 传感器上报周期 ── */
static uint32_t s_last_report_ms = 0;
#define REPORT_INTERVAL_MS  1000U
static uint8_t s_last_status[1U + ACT_MANAGER_COUNT * 2U];
static uint8_t s_have_status;

static const char *link_state_str(link_state_t s)
{
    switch (s) {
    case LINK_UP:       return "UP";
    case LINK_DEGRADED: return "DEGRADED";
    case LINK_DOWN:     return "DOWN";
    default:            return "?";
    }
}

static void link_set_state(link_state_t new_state)
{
    if (new_state == s_link_state) return;

    uint32_t now = HAL_GetTick();
    uint32_t duration = 0;
    link_state_t old = s_link_state;

    if (now >= s_link_enter_ms)
        duration = now - s_link_enter_ms;
    else
        duration = (0xFFFFFFFFUL - s_link_enter_ms) + now + 1;

    switch (old) {
    case LINK_UP:       s_link_stats.up_ms       += duration; break;
    case LINK_DEGRADED: s_link_stats.degraded_ms += duration; break;
    case LINK_DOWN:     s_link_stats.down_ms     += duration; break;
    }

    s_link_state    = new_state;
    s_link_enter_ms = now;
    s_link_stats.state_changes++;

    printf("[LINK] %s -> %s  (was %lu ms)\r\n",
           link_state_str(old), link_state_str(new_state),
           (unsigned long)duration);
}

static void link_feed(void)
{
    s_link_last_rx_ms = HAL_GetTick();
    if (s_link_state != LINK_UP) link_set_state(LINK_UP);
}

static void link_poll(void)
{
    if (s_link_state == LINK_DOWN) return;

    uint32_t now = HAL_GetTick();
    uint32_t idle;

    if (now >= s_link_last_rx_ms)
        idle = now - s_link_last_rx_ms;
    else
        idle = (0xFFFFFFFFUL - s_link_last_rx_ms) + now + 1;

    if (s_link_state == LINK_UP && idle >= LINK_DEGRADED_MS) {
        link_set_state(LINK_DEGRADED);
        s_link_stats.missed_heartbeats++;
    } else if (s_link_state == LINK_DEGRADED && idle >= LINK_DOWN_MS) {
        link_set_state(LINK_DOWN);
        s_link_stats.missed_heartbeats++;
    }
}

/* ════════════════════════════════════════════════════
 *  传感器数据 → 二进制帧构建
 *  ════════════════════════════════════════════════════ */

#define SENSOR_ENTRY_SIZE  6U   /* dev_id(1)+qual(1)+f32(4) */

/* ⚠️ qual 字段语义（阶段 4 修复，2026-10-12）：
 *   协议规范 §4.1 定义 quality 为 1 字节质量枚举
 *   （0=有效 1=预热中 2=超量程 3=传感器故障 4=陈旧），
 *   因此这里必须直接发送 g_sensor_data 中的 sensor_quality_t 原值。
 *
 *   曾经的实现发送 sensor_valid_from_quality() 的**布尔结果**
 *   （有效→1、无效→0），与枚举语义恰好相反：
 *   P4 把"有效"读成 WARMUP（面板显示 "--"），
 *   把"预热/超量程/故障"读成 VALID（面板反而显示出来）。
 *   修复依据：F103-Q 实机日志 qual=1 伴随真实值 28.00/61.00/1.00。
 *
 *   两套枚举数值已对齐：VALID/WARMUP/OVERRANGE≈OVERFLOW/FAULT = 0/1/2/3。
 *   任何一侧改动枚举值都必须同步另一侧（下方 _Static_assert 兜底）。 */
_Static_assert((int)SENSOR_QUAL_VALID     == (int)PROTO_QUAL_VALID    &&
               (int)SENSOR_QUAL_WARMUP    == (int)PROTO_QUAL_WARMUP   &&
               (int)SENSOR_QUAL_OVERRANGE == (int)PROTO_QUAL_OVERFLOW &&
               (int)SENSOR_QUAL_FAULT     == (int)PROTO_QUAL_FAULT,
               "sensor_quality_t must stay aligned with PROTO_QUAL_*");

static uint16_t sensor_build_report(uint8_t *payload, uint16_t max_len)
{
    /* 布局: num_entries(1B) + entry[SENSOR_ENTRY_SIZE] × N */
    /* 最大传感器数 */
#define MAX_SENSORS  9
    if (max_len < 1U + MAX_SENSORS * SENSOR_ENTRY_SIZE) return 0;

    uint16_t off = 1;  /* 跳过 num_entries，填完再回来写 */

    const sensor_data_t *d = &g_sensor_data;

#define APPEND_SENSOR(dev, qual, val_f32)  do {              \
    if (off + SENSOR_ENTRY_SIZE <= max_len) {                \
        payload[off++] = (uint8_t)(dev);                     \
        payload[off++] = (uint8_t)(qual);                    \
        float _v = (float)(val_f32);                         \
        memcpy(&payload[off], &_v, 4);                       \
        off += 4;                                            \
    }                                                        \
} while(0)

    /* ── DHT11 ── */
    APPEND_SENSOR(PROTO_DEV_DHT11_TEMP,
                  d->dht11_quality,
                  d->temperature);

    APPEND_SENSOR(PROTO_DEV_DHT11_HUM,
                  d->dht11_quality,
                  d->humidity);

    /* ── BMP280 ── */
    APPEND_SENSOR(PROTO_DEV_BMP280_TEMP,
                  d->bmp280_quality,
                  d->bmp280_temp);

    APPEND_SENSOR(PROTO_DEV_BMP280_PRESS,
                  d->bmp280_quality,
                  d->pressure);

    /* ── 土壤 ── */
    APPEND_SENSOR(PROTO_DEV_SOIL,
                  d->soil_quality,
                  d->soil);

    /* ── 雨滴 ── */
    APPEND_SENSOR(PROTO_DEV_RAIN,
                  d->rain_quality,
                  d->rain);

    /* ── MQ-4 ──
     * 上报**百分比字段**（0~100），与 ALARM_EVENT 的 value/threshold 同单位：
     * P4 侧"数据恢复后自动解除告警"是拿本值与该告警的阈值（60%）比较的，
     * 若这里发 gas_ppm（ppm 量级恒 > 60）会导致告警永远无法解除。 */
    APPEND_SENSOR(PROTO_DEV_MQ4,
                  d->gas_quality,
                  d->gas);

    /* ── MQ-135 ── 同上：发百分比，与 0~100 阶梯 + 60% 阈值同单位。 */
    APPEND_SENSOR(PROTO_DEV_MQ135,
                  d->air_quality,
                  d->air);

    /* ── PIR ── */
    APPEND_SENSOR(PROTO_DEV_HUMAN,
                  d->human_quality,
                  d->human);

#undef APPEND_SENSOR

    /* 填 num_entries */
    uint8_t num = (uint8_t)((off - 1U) / SENSOR_ENTRY_SIZE);
    payload[0] = num;

    return off;
}

/* ════════════════════════════════════════════════════
 *  RX 回调
 *  ════════════════════════════════════════════════════ */

static void app_proto_rx_cb(const proto_frame_t *frame)
{
    if (frame->type == PROTO_ACTUATOR_CMD) {
        printf("[PROTO] RX ACTUATOR_CMD seq=%u len=%u\r\n",
               (unsigned)frame->seq, (unsigned)frame->len);
    }
    /* OTA 流必须幂等：P4 超时重传同一 seq 时要重新处理并回 ACK，
     * 因此不参与普通去重表（避免重传被静默丢弃，P4 永远等不到结果）。 */
    int is_ota = (frame->type == PROTO_OTA_BEGIN ||
                  frame->type == PROTO_OTA_DATA ||
                  frame->type == PROTO_OTA_END ||
                  frame->type == PROTO_OTA_ABORT);

    /* 去重检查 */
    /* P4 and F103 maintain independent sequence spaces.  An actuator command
     * may legitimately reuse a number already seen in an F103-originated
     * sensor/status stream; never discard it solely on that shared number. */
    if (!is_ota) {
        if (ProtoTx_IsDuplicate(frame->seq) && frame->type != PROTO_ACTUATOR_CMD) {
            /* 重复帧处理：HELLO_REQ/PING 是建链与保活帧，必须照常响应——
             * 对端复位后其 seq 重新从 0 递增，可能命中本会话去重表中的
             * 旧序号；若一并丢弃会导致重握手被无谓延迟。其余类型直接丢弃。 */
            if (frame->type != PROTO_HELLO_REQ && frame->type != PROTO_PING) {
                return;
            }
        } else if (frame->type != PROTO_ACTUATOR_CMD) {
            ProtoTx_RecordSeq(frame->seq);
        }
    }

    /* 任何有效帧都意味着链路存活 */
    link_feed();

    switch (frame->type) {

    /* ── 传输层 ACK/NACK ── */
    case PROTO_SENSOR_REPORT_ACK:
    case PROTO_ALARM_EVENT_ACK:
        if (frame->len >= 2) {
            uint16_t acked = (uint16_t)frame->payload[0]
                           | ((uint16_t)frame->payload[1] << 8);
            ProtoTx_OnAck(acked);
        }
        break;

    case PROTO_ACTUATOR_CMD_ACK:
        if (frame->len >= 2) {
            ProtoTx_OnAck(frame->seq);
            if (frame->len >= 3) {
                uint8_t result = frame->payload[0];
                printf("[PROTO] ACTUATOR_ACK  dev=0x%02X result=%u\r\n",
                       (unsigned)frame->seq, (unsigned)result);
            }
        }
        break;

    case PROTO_NACK:
        if (frame->len >= 3) {
            uint16_t nacked = (uint16_t)frame->payload[0]
                            | ((uint16_t)frame->payload[1] << 8);
            ProtoTx_OnNack(nacked, frame->payload[2]);
        }
        break;

    /* ── PING/PONG ── */
    case PROTO_PING:
        ProtoUart_SendFrame(PROTO_PONG, frame->seq, 0, 0);
        break;

    case PROTO_PONG:
        break;

    /* ── HELLO 版本协商 ── */
    case PROTO_HELLO_REQ: {
        uint8_t hello[24];
        memset(hello, 0, sizeof(hello));
        hello[0] = PROTO_VER;
        hello[1] = (uint8_t)(F103_CAPS & 0xFF);
        hello[2] = (uint8_t)((F103_CAPS >> 8) & 0xFF);
        memcpy(&hello[3], F103_DEV_NAME,
               strlen(F103_DEV_NAME) < 16 ? strlen(F103_DEV_NAME) : 16);
        ProtoUart_SendFrame(PROTO_HELLO_RSP, frame->seq, hello, sizeof(hello));
        printf("[PROTO] HELLO_RSP  ver=%u caps=0x%03X\r\n",
               (unsigned)PROTO_VER, (unsigned)F103_CAPS);
        break;
    }

    case PROTO_HELLO_RSP: {
        if (frame->len >= 3) {
            uint8_t  peer_ver  = frame->payload[0];
            uint16_t peer_caps = (uint16_t)frame->payload[1]
                               | ((uint16_t)frame->payload[2] << 8);
            if (peer_ver != PROTO_VER) {
                printf("[PROTO] HELLO version mismatch: local=%u peer=%u\r\n",
                       (unsigned)PROTO_VER, (unsigned)peer_ver);
            }
            printf("[PROTO] HELLO_RSP from peer  ver=%u caps=0x%03X\r\n",
                   (unsigned)peer_ver, (unsigned)peer_caps);
        }
        break;
    }

    /* ── 执行器指令 ── */
    case PROTO_ACTUATOR_CMD: {
        uint8_t result = PROTO_RESULT_ERR_PARAM;
        uint32_t now = HAL_GetTick();
        uint8_t  ack_dev = (frame->len >= 8) ? frame->payload[0] : 0;

        /* 延迟打点：t0 为进入本帧处理，t1 为 act_manager 写完 GPIO。
         * 跨芯片时钟不可比，因此只回传本侧区间耗时（微秒），由 P4 结算端到端。 */
        uint32_t t0_us = delay_micros();

        if (frame->len >= 8) {
            uint8_t  dev_id = frame->payload[0];
            uint8_t  action = frame->payload[1];
            uint16_t ttl_ms = (uint16_t)frame->payload[5]
                            | ((uint16_t)frame->payload[6] << 8);

            /* P4 and F103 have independent boot-relative clocks; the
             * optional command timestamp cannot be compared across chips.
             * TTL is enforced by act_manager as a local execution limit. */

            printf("[PROTO] ACTUATOR_CMD  dev=0x%02X action=%u ttl=%ums\r\n",
                   (unsigned)dev_id, (unsigned)action, (unsigned)ttl_ms);

            result = act_manager_command(dev_id, action, ttl_ms,
                                         ACT_SOURCE_REMOTE, now);
        }

        uint32_t proc_us = delay_micros() - t0_us;

    {
            uint8_t ack[7];
            memset(ack, 0, sizeof(ack));
            ack[0] = ack_dev;
            ack[1] = result;
            ack[2] = act_manager_get_state(ack_dev);
            /* ACK payload[3..6]: 本侧 "收帧 → 写 GPIO" 耗时（微秒, little-endian） */
            ack[3] = (uint8_t)(proc_us & 0xFF);
            ack[4] = (uint8_t)((proc_us >> 8) & 0xFF);
            ack[5] = (uint8_t)((proc_us >> 16) & 0xFF);
            ack[6] = (uint8_t)((proc_us >> 24) & 0xFF);
            ProtoUart_SendFrame(PROTO_ACTUATOR_CMD_ACK,
                                frame->seq, ack, 7);
        }
        printf("[LAT] ACTUATOR_CMD dev=0x%02X result=%u rx->gpio=%lu us\r\n",
               (unsigned)ack_dev, (unsigned)result, (unsigned long)proc_us);
        break;
    }

    /* ── 状态查询 ── */
    case PROTO_ACTUATOR_STATUS: {
        uint8_t status[16];
        uint16_t len = AppProto_GetActuatorStatus(status, sizeof(status));
        ProtoUart_SendFrame(PROTO_ACTUATOR_STATUS, frame->seq,
                            status, len);
        break;
    }

    /* ── 统计查询 ── */
    case PROTO_STATS_GET: {
        link_stats_t ls;
        AppProto_GetLinkStats(&ls);
        uint8_t stats[20];
        memset(stats, 0, sizeof(stats));
        /* 填链路统计 */
        memcpy(&stats[0], &ls.up_ms,          4);
        memcpy(&stats[4], &ls.degraded_ms,    4);
        memcpy(&stats[8], &ls.down_ms,        4);
        memcpy(&stats[12], &ls.state_changes, 4);
        stats[16] = (uint8_t)ls.state;
        /* 协议传输统计 */
        proto_stats_t us;
        ProtoUart_GetStats(&us);
        stats[17] = (uint8_t)(us.rx_frames & 0xFF);
        stats[18] = (uint8_t)(us.crc_errors & 0xFF);
        stats[19] = (uint8_t)(us.resyncs & 0xFF);
        ProtoUart_SendFrame(PROTO_STATS_DATA, frame->seq, stats, sizeof(stats));
        break;
    }

    /* ── 阶段 8: F103 IAP ── */
    case PROTO_OTA_BEGIN:
    case PROTO_OTA_DATA:
    case PROTO_OTA_END:
    case PROTO_OTA_ABORT:
        (void)AppF103Ota_HandleFrame(frame);
        break;

    /* ── 未实现帧 ── */
    case PROTO_ERROR:
    case PROTO_SENSOR_REPORT:
    case PROTO_ALARM_EVENT:
    case PROTO_CONFIG_GET:
    case PROTO_CONFIG_SET:
    case PROTO_CONFIG_DATA:
    default:
        break;
    }
}

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

void AppProto_Init(void)
{
    ProtoTx_Init();

    s_link_state      = LINK_DOWN;
    s_link_last_rx_ms = 0;
    s_link_enter_ms   = HAL_GetTick();
    memset((void *)&s_link_stats, 0, sizeof(s_link_stats));
    s_last_report_ms = 0;
    memset(s_last_status, 0, sizeof(s_last_status));
    s_have_status = 0;

    ProtoUart_SetRxCallback(app_proto_rx_cb);
    printf("[PROTO] dispatch ready  link=DOWN\r\n");

    /* 自测 */
    printf("[PROTO] loopback self-test: send PING on USART1...\r\n");
    ProtoUart_SendFrame(PROTO_PING, 1, 0, 0);

    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 500) {
        ProtoUart_Poll();
        if (s_link_state == LINK_UP) break;
    }

    if (s_link_state == LINK_UP) {
        printf("[PROTO] loopback self-test PASSED  link=UP ✓\r\n");
    } else {
        printf("[PROTO] loopback self-test FAILED — connect PA9↔PA10?\r\n");
    }
}

void AppProto_Poll(void)
{
    uint32_t now = HAL_GetTick();
    link_poll();
    ProtoTx_Poll();
    alarm_retry_poll();
    act_manager_poll(now, s_link_state == LINK_DOWN);
    AppF103Ota_Poll();

    /* State-change report: exposes local fallback and safety trips. */
    uint8_t status[1U + ACT_MANAGER_COUNT * 2U];
    uint16_t len = act_manager_get_status(status, sizeof(status));
    if (len && (!s_have_status || memcmp(status, s_last_status, len) != 0)) {
        memcpy(s_last_status, status, len);
        s_have_status = 1;
        ProtoUart_SendFrame(PROTO_ACTUATOR_STATUS, ProtoTx_NextSeq(), status, len);
    }
}

link_state_t AppProto_GetLinkState(void)
{
    return s_link_state;
}

void AppProto_GetLinkStats(link_stats_t *out)
{
    if (out) memcpy(out, &s_link_stats, sizeof(link_stats_t));
}

/* ════════════════════════════════════════════════════
 *  告警上报（ALARM_EVENT 0x12）
 *
 *  载荷: alarm_id(1B) + level(1B) + value(f32) + threshold(f32)
 *  语义: 仅"通知面板显示"。安全动作（蜂鸣/关断）仍全部本地完成 ——
 *        故障域隔离（ADR-009）在协议层的体现。
 *  TX 同一时刻只允许 1 个待 ACK 帧在途，而 1Hz 的 SENSOR_REPORT 会占槽，
 *  因此发送失败时挂起，由 AppProto_Poll 每轮重试，直到发出为止。
 * ════════════════════════════════════════════════════ */

static uint8_t s_alarm_pending;
static uint8_t s_alarm_id;
static uint8_t s_alarm_level;
static float   s_alarm_value;
static float   s_alarm_threshold;

static int alarm_try_send(uint8_t alarm_id, uint8_t level,
                          float value, float threshold)
{
    uint8_t payload[1U + 1U + 4U + 4U];
    payload[0] = alarm_id;
    payload[1] = level;
    memcpy(&payload[2], &value,     4);
    memcpy(&payload[6], &threshold, 4);

    return ProtoTx_SendWithAck(PROTO_ALARM_EVENT, payload,
                               (uint16_t)sizeof(payload)) != 0;
}

int AppProto_NotifyAlarm(uint8_t alarm_id, uint8_t level,
                         float value, float threshold)
{
    if (alarm_try_send(alarm_id, level, value, threshold)) {
        s_alarm_pending = 0;
        printf("[PROTO] ALARM_EVENT tx  id=0x%02X level=%u value=%ld thr=%ld\r\n",
               (unsigned)alarm_id, (unsigned)level,
               (long)value, (long)threshold);
        return 1;
    }

    /* TX 槽被占（例如 SENSOR_REPORT 在途）：挂起，等下一轮重试 */
    s_alarm_pending   = 1;
    s_alarm_id        = alarm_id;
    s_alarm_level     = level;
    s_alarm_value     = value;
    s_alarm_threshold = threshold;
    printf("[PROTO] ALARM_EVENT queued (tx busy)  id=0x%02X\r\n",
           (unsigned)alarm_id);
    return 0;
}

static void alarm_retry_poll(void)
{
    if (!s_alarm_pending) return;

    if (alarm_try_send(s_alarm_id, s_alarm_level,
                       s_alarm_value, s_alarm_threshold)) {
        s_alarm_pending = 0;
        printf("[PROTO] ALARM_EVENT tx (retry)  id=0x%02X\r\n",
               (unsigned)s_alarm_id);
    }
}

/* ════════════════════════════════════════════════════
 *  传感器上报任务
 *  ════════════════════════════════════════════════════ */

void AppProto_SendReportTask(void)
{
    uint32_t now = HAL_GetTick();

    /* LINK_DOWN 才停止上报；DEGRADED 只是暂未收到对方帧，仍尝试发送 */
    if (s_link_state == LINK_DOWN) return;

    /* IAP 期间暂停 1Hz 传感器帧，避免与 OTA 流抢 UART/发送槽；
     * P4 的 PING 仍会继续，链路保持 UP。 */
    if (AppF103Ota_IsActive()) return;

    /* 1Hz 周期 */
    if (now - s_last_report_ms < REPORT_INTERVAL_MS) return;
    s_last_report_ms = now;

    /* 构建传感器帧 */
    uint8_t payload[PROTO_MAX_PAYLOAD];
    uint16_t len = sensor_build_report(payload, sizeof(payload));
    if (len == 0) return;

    /* 发送（带 ACK） */
    ProtoTx_SendWithAck(PROTO_SENSOR_REPORT, payload, len);
}

/* ════════════════════════════════════════════════════
 *  执行器状态查询
 *  ════════════════════════════════════════════════════ */

uint16_t AppProto_GetActuatorStatus(uint8_t *payload, uint16_t max_len)
{
    return act_manager_get_status(payload, max_len);
}
