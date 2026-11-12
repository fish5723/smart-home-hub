/**
 * @file app_proto.c
 * @brief 协议调度层 — P4 侧（主控端）
 *
 * 本层职责:
 *   1. 注册 ProtoUart RX callback，接收帧
 *   2. 去重 → ACK/NACK 判定 → 业务帧分发
 *   3. 链路状态机: UP → 3s 无帧 → DEGRADED → 6s → DOWN
 *   4. HELLO 协商（发起端）
 *   5. 设备影子（传感器数据缓存 + 执行器状态跟踪）
 *   6. 命令下发（ACTUATOR_CMD 带 TTL）
 *
 * 阶段 2B: P4 侧独立实现。
 */
#include "app_proto.h"
#include "proto_uart.h"
#include "proto_tx.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "app_proto";

/* ════════════════════════════════════════════════════
 *  P4 能力位图
 *  ════════════════════════════════════════════════════ */

#define P4_CAP_HMI           (1U << 0)
#define P4_CAP_WIFI          (1U << 1)
#define P4_CAP_VISION        (1U << 2)
#define P4_CAP_STORAGE       (1U << 3)
#define P4_CAP_PROTO_MASTER  (1U << 4)

#define P4_CAPS  (P4_CAP_HMI | P4_CAP_WIFI | P4_CAP_VISION |       \
                  P4_CAP_STORAGE | P4_CAP_PROTO_MASTER)

#define P4_DEV_NAME  "P4-HUB"

/* ════════════════════════════════════════════════════
 *  链路状态机
 *  ════════════════════════════════════════════════════ */

#define LINK_DEGRADED_MS  3000U
#define LINK_DOWN_MS      6000U

static link_state_t  s_link_state      = LINK_DOWN;
static int64_t       s_link_last_rx_us = 0;
static int64_t       s_link_enter_us   = 0;
static link_stats_t  s_link_stats;

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

    int64_t now = esp_timer_get_time();
    int64_t duration = now - s_link_enter_us;
    if (duration < 0) duration = 0;

    uint32_t dur_ms = (uint32_t)(duration / 1000);

    switch (s_link_state) {
    case LINK_UP:       s_link_stats.up_ms       += dur_ms; break;
    case LINK_DEGRADED: s_link_stats.degraded_ms += dur_ms; break;
    case LINK_DOWN:     s_link_stats.down_ms     += dur_ms; break;
    }

    s_link_state    = new_state;
    s_link_enter_us = now;
    s_link_stats.state_changes++;

    ESP_LOGI(TAG, "LINK %s -> %s  (was %lu ms)",
             link_state_str(s_link_state == LINK_UP ? LINK_DOWN :
                            s_link_state == LINK_DEGRADED ? LINK_UP :
                            s_link_state == LINK_DOWN ? LINK_DEGRADED : LINK_UP),
             /* 简化：直接打印新状态 */
             link_state_str(new_state),
             (unsigned long)dur_ms);
}

static void link_feed(void)
{
    s_link_last_rx_us = esp_timer_get_time();
    if (s_link_state != LINK_UP) link_set_state(LINK_UP);
}

static void link_poll(void)
{
    if (s_link_state == LINK_DOWN) return;

    int64_t now = esp_timer_get_time();
    int64_t idle_us = now - s_link_last_rx_us;
    uint32_t idle_ms = (uint32_t)(idle_us / 1000);

    if (s_link_state == LINK_UP && idle_ms >= LINK_DEGRADED_MS) {
        link_set_state(LINK_DEGRADED);
        s_link_stats.missed_heartbeats++;
    } else if (s_link_state == LINK_DEGRADED && idle_ms >= LINK_DOWN_MS) {
        link_set_state(LINK_DOWN);
        s_link_stats.missed_heartbeats++;
    }
}

/* ════════════════════════════════════════════════════
 *  设备影子
 *  ════════════════════════════════════════════════════ */

static sensor_shadow_entry_t   s_sensor_shadow[SHADOW_MAX_SENSORS];
static int                     s_sensor_count;
static actuator_shadow_entry_t s_actuator_shadow[4];
static int                     s_actuator_count;
static actuator_feedback_t     s_last_actuator_feedback;
static alarm_shadow_t          s_alarm;   /* 最近一次 ALARM_EVENT + 是否仍告警 */
static app_proto_ota_result_cb_t s_ota_result_cb;

/* 本地优先仲裁（ADR-004）：每个执行器最近一次"本地"操作时间戳（毫秒）。
 * 索引按 dev_id；只覆盖执行器 ID 范围（< 0x20），不落 NVS。 */
#define ACTUATOR_CMD_CAP       32
static int64_t s_last_local_cmd_ms[ACTUATOR_CMD_CAP];

/* 告警源 → 传感器 dev_id 映射（用于"数据恢复后自动解除告警"） */
static uint8_t alarm_source_dev(uint8_t alarm_id)
{
    switch (alarm_id) {
    case PROTO_ALARM_GAS_MQ4:   return PROTO_DEV_MQ4;
    case PROTO_ALARM_AIR_MQ135: return PROTO_DEV_MQ135;
    default:                    return 0xFFU;
    }
}

static void shadow_update_sensor(uint8_t dev_id, uint8_t quality, float value)
{
    int64_t now = esp_timer_get_time();

    /* 告警解除（2026-10-18）：ALARM_EVENT 只负责"触发通知"，解除由数据判定 ——
     * 收到该告警对应设备的新鲜有效数据、且低于上报阈值时清掉 active。 */
    if (s_alarm.active && quality == PROTO_QUAL_VALID &&
        dev_id == alarm_source_dev(s_alarm.alarm_id) &&
        value < s_alarm.threshold) {
        s_alarm.active = false;
        ESP_LOGI(TAG, "ALARM_EVENT cleared  id=0x%02X dev=0x%02X val=%.1f thr=%.1f",
                 (unsigned)s_alarm.alarm_id, (unsigned)dev_id,
                 (double)value, (double)s_alarm.threshold);
    }

    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensor_shadow[i].dev_id == dev_id) {
            s_sensor_shadow[i].quality       = quality;
            s_sensor_shadow[i].value         = value;
            s_sensor_shadow[i].last_update_us = now;
            return;
        }
    }

    if (s_sensor_count < SHADOW_MAX_SENSORS) {
        s_sensor_shadow[s_sensor_count].dev_id         = dev_id;
        s_sensor_shadow[s_sensor_count].quality        = quality;
        s_sensor_shadow[s_sensor_count].value          = value;
        s_sensor_shadow[s_sensor_count].last_update_us = now;
        s_sensor_count++;
    }
}

static void shadow_update_actuator(uint8_t dev_id, uint8_t state)
{
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < s_actuator_count; i++) {
        if (s_actuator_shadow[i].dev_id == dev_id) {
            s_actuator_shadow[i].state          = state;
            s_actuator_shadow[i].last_update_us  = now;
            return;
        }
    }

    if (s_actuator_count < 4) {
        s_actuator_shadow[s_actuator_count].dev_id         = dev_id;
        s_actuator_shadow[s_actuator_count].state          = state;
        s_actuator_shadow[s_actuator_count].last_update_us  = now;
        s_actuator_count++;
    }
}

/* ════════════════════════════════════════════════════
 *  端到端延迟打点（验收：屏幕点击 → 执行器吸合 < 200ms）
 *
 *  起点：UI 点击事件调用 AppProto_MarkUiClick()（esp_timer）
 *  终点：收到 F103 的 ACTUATOR_CMD_ACK —— 此时 F103 已写完 GPIO，
 *        并在 ACK payload[3..6] 回传其内部"收帧 → 写 GPIO"耗时（微秒）
 *  注意：P4 往返含回程 UART，是略保守的上界；真机数字以本日志为准。
 * ════════════════════════════════════════════════════ */

#define LAT_ARM_TIMEOUT_US   1000000LL   /* 1s 未见 ACK 视为无效样本 */
#define LAT_BUDGET_US        200000LL    /* 验收上限 200ms */

static bool     s_lat_armed;
static int64_t  s_lat_start_us;
static uint32_t s_lat_samples;
static int64_t  s_lat_sum_us;
static int64_t  s_lat_min_us;
static bool     s_lat_min_set;
static int64_t  s_lat_max_us;
static uint32_t s_lat_over_budget;

void AppProto_MarkUiClick(void)
{
    s_lat_start_us = esp_timer_get_time();
    s_lat_armed    = true;
}

static void lat_record(uint8_t dev_id, int64_t dt_us, uint32_t peer_proc_us)
{
    if (dt_us < 0) dt_us = 0;

    s_lat_samples++;
    s_lat_sum_us += dt_us;
    if (!s_lat_min_set || dt_us < s_lat_min_us) { s_lat_min_us = dt_us; s_lat_min_set = true; }
    if (dt_us > s_lat_max_us) s_lat_max_us = dt_us;
    if (dt_us > LAT_BUDGET_US) s_lat_over_budget++;

    ESP_LOGI(TAG, "LATENCY  dev=0x%02X  click->relay=%.2f ms  f103_proc=%.2f ms  "
             "min=%.2f max=%.2f avg=%.2f ms  n=%lu  over200=%lu",
             (unsigned)dev_id,
             (double)dt_us / 1000.0,
             (double)peer_proc_us / 1000.0,
             (double)s_lat_min_us / 1000.0,
             (double)s_lat_max_us / 1000.0,
             (double)s_lat_sum_us / (double)s_lat_samples / 1000.0,
             (unsigned long)s_lat_samples,
             (unsigned long)s_lat_over_budget);

    if (dt_us > LAT_BUDGET_US) {
        ESP_LOGW(TAG, "LATENCY  OVER BUDGET (>200ms): %.2f ms", (double)dt_us / 1000.0);
    }
}

/* ════════════════════════════════════════════════════
 *  HELLO 协商
 *  ════════════════════════════════════════════════════ */

static bool    s_hello_done;
static uint8_t s_peer_ver;       /* F103 协议版本 */
static uint8_t s_peer_caps[2];   /* F103 能力位图 */
static char    s_peer_name[17];
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;

void AppProto_RequestHello(void)
{
    ESP_LOGI(TAG, "Sending HELLO_REQ...");
    ProtoTx_SendNoAck(PROTO_HELLO_REQ, NULL, 0);
}

/* ════════════════════════════════════════════════════
 *  SENSOR_REPORT 解析
 *  ════════════════════════════════════════════════════ */

#define SENSOR_ENTRY_SIZE  6U

/* 周期性 SENSOR_REPORT 的逐帧打印开关。
 * (2026-10-18 勘误) 此前本注释声称 console 是非阻塞输出，与实际不符：
 * 非阻塞的 nb_console_vprintf 已回滚（完整实现留在 git 历史 cdbe42b），
 * 当前 console 是**忙等写** —— 见 app_main.c 中 app_main() 头部说明。
 * 因此串口工具未打开时日志写入会阻塞主任务并触发看门狗：联调期请保持
 * 串口窗口打开；无人值守运行时把本开关置 0。
 * 影子状态（shadow_update_sensor）是数据的正式去向，打印仅供人观测。 */
#ifndef PROTO_LOG_SENSOR_FRAMES
#define PROTO_LOG_SENSOR_FRAMES   1
#endif

static void handle_sensor_report(const proto_frame_t *frame)
{
    if (frame->len < 1) return;

    uint8_t num = frame->payload[0];
    uint16_t off = 1;

#if PROTO_LOG_SENSOR_FRAMES
    ESP_LOGI(TAG, "SENSOR_REPORT  num=%u", (unsigned)num);
#endif

    for (uint8_t i = 0; i < num; i++) {
        if (off + SENSOR_ENTRY_SIZE > frame->len) break;

        uint8_t dev_id = frame->payload[off];
        uint8_t qual   = frame->payload[off + 1];
        float value;
        memcpy(&value, &frame->payload[off + 2], 4);
        off += SENSOR_ENTRY_SIZE;

        shadow_update_sensor(dev_id, qual, value);

#if PROTO_LOG_SENSOR_FRAMES
        if (value != 0.0f || qual == PROTO_QUAL_VALID) {
            ESP_LOGI(TAG, "  dev=0x%02X qual=%u val=%.2f",
                     (unsigned)dev_id, (unsigned)qual, (double)value);
        }
#endif
    }
}

/* ════════════════════════════════════════════════════
 *  ALARM_EVENT 解析（F103 → P4）
 *  载荷: alarm_id(1B) + level(1B) + value(f32) + threshold(f32) = 10B
 *  语义: 只做"通知面板显示"。安全动作由 F103 本地完成（ADR-009）。
 * ════════════════════════════════════════════════════ */

static void handle_alarm_event(const proto_frame_t *frame)
{
    if (frame->len < 10) return;

    uint8_t alarm_id = frame->payload[0];
    uint8_t level    = frame->payload[1];
    float   value, threshold;
    memcpy(&value,     &frame->payload[2], 4);
    memcpy(&threshold, &frame->payload[6], 4);

    s_alarm.alarm_id       = alarm_id;
    s_alarm.level          = level;
    s_alarm.value          = value;
    s_alarm.threshold      = threshold;
    s_alarm.active         = true;
    s_alarm.last_update_us = esp_timer_get_time();

    ESP_LOGW(TAG, "ALARM_EVENT  id=0x%02X level=%u value=%.1f threshold=%.1f",
             (unsigned)alarm_id, (unsigned)level,
             (double)value, (double)threshold);
}

/* ════════════════════════════════════════════════════
 *  HELLO_RSP 解析
 *  ════════════════════════════════════════════════════ */

static void handle_hello_rsp(const proto_frame_t *frame)
{
    if (frame->len < 3) return;

    uint8_t  peer_ver  = frame->payload[0];
    uint16_t peer_caps = (uint16_t)frame->payload[1]
                       | ((uint16_t)frame->payload[2] << 8);
    memcpy(s_peer_caps, &frame->payload[1], 2);
    s_peer_ver = peer_ver;

    memset(s_peer_name, 0, sizeof(s_peer_name));
    if (frame->len >= 19) {
        memcpy(s_peer_name, &frame->payload[3], 16);
        /* 确保终止符 */
        s_peer_name[16] = '\0';
    }

    s_hello_done = true;

    /* Synchronize the UI with physical outputs after every link handshake. */
    AppProto_QueryActuatorStatus();

    ESP_LOGI(TAG, "HELLO_RSP  ver=%u caps=0x%03X name=%s",
             (unsigned)peer_ver, (unsigned)peer_caps, s_peer_name);

    if (peer_ver != PROTO_VER) {
        ESP_LOGW(TAG, "Version mismatch: local=%u peer=%u — "
                      "compatibility not guaranteed",
                 (unsigned)PROTO_VER, (unsigned)peer_ver);
    }

    ESP_LOGI(TAG, "F103 capabilities: RELAY=%d PUMP=%d LOCK=%d "
             "BEEPER=%d DHT11=%d BMP280=%d SOIL=%d RAIN=%d MQ=%d PIR=%d",
             !!(peer_caps & 0x001), !!(peer_caps & 0x002),
             !!(peer_caps & 0x004), !!(peer_caps & 0x008),
             !!(peer_caps & 0x010), !!(peer_caps & 0x020),
             !!(peer_caps & 0x040), !!(peer_caps & 0x080),
             !!(peer_caps & 0x100), !!(peer_caps & 0x200));
}

/* ════════════════════════════════════════════════════
 *  命令下发
 *  ════════════════════════════════════════════════════ */

void AppProto_SendActuatorCmdEx(uint8_t dev_id, uint8_t action, uint16_t ttl_ms,
                                proto_cmd_src_t src)
{
    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;

    /* 本地优先仲裁（ADR-004）：记录本地操作时间戳；云端指令在窗口内被丢弃 */
    if (src == PROTO_CMD_SRC_LOCAL) {
        if (dev_id < ACTUATOR_CMD_CAP) s_last_local_cmd_ms[dev_id] = now_ms;
    } else if (dev_id < ACTUATOR_CMD_CAP && s_last_local_cmd_ms[dev_id] != 0) {
        int64_t since = now_ms - s_last_local_cmd_ms[dev_id];
        if (since < PROTO_CLOUD_ARB_WINDOW_MS) {
            ESP_LOGW(TAG, "[ARB] cloud cmd dropped: local op %lldms ago (<%dms) dev=0x%02X",
                     (long long)since, PROTO_CLOUD_ARB_WINDOW_MS, (unsigned)dev_id);
            return;
        }
    }

    uint32_t ts_ms = (uint32_t)now_ms;

    uint8_t cmd[8];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = dev_id;
    cmd[1] = action;
    cmd[3] = (uint8_t)(ts_ms & 0xFF);
    cmd[4] = (uint8_t)((ts_ms >> 8) & 0xFF);
    cmd[5] = (uint8_t)(ttl_ms & 0xFF);
    cmd[6] = (uint8_t)((ttl_ms >> 8) & 0xFF);
    cmd[7] = (uint8_t)((ts_ms >> 16) & 0xFF);

    ESP_LOGI(TAG, "ACTUATOR_CMD  dev=0x%02X action=%u ttl=%ums",
             (unsigned)dev_id, (unsigned)action, (unsigned)ttl_ms);

    /* 发送槽占用时该命令会被丢弃，无法形成有效延迟样本，主动取消打点。 */
    if (s_lat_armed && ProtoTx_IsBusy()) {
        s_lat_armed = false;
        ESP_LOGW(TAG, "LATENCY  cmd dropped (tx busy) — sample cancelled");
    }

    ProtoTx_SendWithAck(PROTO_ACTUATOR_CMD, cmd, sizeof(cmd));
}

void AppProto_SendActuatorCmd(uint8_t dev_id, uint8_t action, uint16_t ttl_ms)
{
    AppProto_SendActuatorCmdEx(dev_id, action, ttl_ms, PROTO_CMD_SRC_LOCAL);
}

int64_t AppProto_GetLastLocalCmdMs(uint8_t dev_id)
{
    return dev_id < ACTUATOR_CMD_CAP ? s_last_local_cmd_ms[dev_id] : 0;
}

void AppProto_QueryActuatorStatus(void)
{
    ESP_LOGI(TAG, "Querying actuator status...");
    /* The 0x22 response carries the complete snapshot and is also the
     * transaction acknowledgement.  Do not occupy the single reliable-TX
     * slot: a delayed status response must never make a user relay command
     * get silently dropped by ProtoTx_SendWithAck(). */
    ProtoTx_SendNoAck(PROTO_ACTUATOR_STATUS, NULL, 0);
}

/* ════════════════════════════════════════════════════
 *  RX 回调 — 帧分发
 *  ════════════════════════════════════════════════════ */

static void app_proto_rx_cb(const proto_frame_t *frame)
{
    /* T-26 帧类型日志：记录每次收到帧的类型和负载长度，用于验证通信生命周期。
     * OTA 升级会连续收到 400+ 条 OTA_RESULT，逐帧打印会拖慢发送节奏，故跳过。 */
    if (frame->type != PROTO_OTA_RESULT) {
        ESP_LOGI(TAG, "RX  type=0x%02X seq=%u len=%u",
                 (unsigned)frame->type, (unsigned)frame->seq,
                 (unsigned)frame->len);
    }

    /* 任何有效帧都意味着链路存活 */
    link_feed();

    /* 去重 */
    if (ProtoTx_IsDuplicate(frame->seq)) {
        switch (frame->type) {
        case PROTO_SENSOR_REPORT:
        case PROTO_ALARM_EVENT:
            /* 重复数据帧仍需回 ACK */
            break;
        case PROTO_HELLO_REQ:
        case PROTO_HELLO_RSP:
            /* HELLO 帧始终处理，不受去重限制 */
            break;
        case PROTO_ACTUATOR_CMD_ACK:
        case PROTO_ACTUATOR_STATUS:
        case PROTO_OTA_RESULT:
        case PROTO_NACK:
            /* P4/F103 use independent TX sequence counters.  A response can
             * legitimately reuse a number previously seen on the opposite
             * direction; dropping it here leaves the UI waiting forever. */
            break;
        default:
            return;
        }
    }
    ProtoTx_RecordSeq(frame->seq);

    switch (frame->type) {

    /* ── ACK ── */
    case PROTO_SENSOR_REPORT_ACK:
    case PROTO_ALARM_EVENT_ACK:
        if (frame->len >= 2) {
            uint16_t acked = (uint16_t)frame->payload[0]
                           | ((uint16_t)frame->payload[1] << 8);
            ProtoTx_OnAck(acked);
        }
        break;

    case PROTO_ACTUATOR_CMD_ACK:
        ProtoTx_OnAck(frame->seq);
        /* 终点：F103 已写完 GPIO 并回 ACK，结算一次端到端延迟样本。 */
        if (s_lat_armed) {
            int64_t dt_us = esp_timer_get_time() - s_lat_start_us;
            uint32_t peer_proc_us = 0;
            if (frame->len >= 7) {
                peer_proc_us = (uint32_t)frame->payload[3]
                             | ((uint32_t)frame->payload[4] << 8)
                             | ((uint32_t)frame->payload[5] << 16)
                             | ((uint32_t)frame->payload[6] << 24);
            }
            lat_record(frame->len >= 1 ? frame->payload[0] : 0U, dt_us, peer_proc_us);
            s_lat_armed = false;
        }
        if (frame->len >= 2) {
            uint8_t dev_id = frame->payload[0];
            uint8_t result = frame->payload[1];
            s_last_actuator_feedback.dev_id = dev_id;
            s_last_actuator_feedback.result = result;
            s_last_actuator_feedback.state = frame->len >= 3 ? frame->payload[2] : 0U;
            s_last_actuator_feedback.last_update_us = esp_timer_get_time();
            ESP_LOGI(TAG, "ACTUATOR_ACK  dev=0x%02X result=%u state=%u",
                     (unsigned)dev_id, (unsigned)result,
                     frame->len >= 3 ? (unsigned)frame->payload[2] : 0U);
            if (result == PROTO_RESULT_OK && frame->len >= 3) {
                /* ACK state is the F103 read-back state, not the requested action. */
                shadow_update_actuator(dev_id, frame->payload[2]);
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

    /* ── PING / PONG ── */
    case PROTO_PING:
        ProtoUart_SendFrame(PROTO_PONG, frame->seq, NULL, 0);
        break;

    case PROTO_PONG:
        break;

    /* ── HELLO ── */
    case PROTO_HELLO_REQ:
        /* F103 请求协商（通常不会） */
        {
            uint8_t hello[24];
            memset(hello, 0, sizeof(hello));
            hello[0] = PROTO_VER;
            hello[1] = (uint8_t)(P4_CAPS & 0xFF);
            hello[2] = (uint8_t)((P4_CAPS >> 8) & 0xFF);
            memcpy(&hello[3], P4_DEV_NAME,
                   strlen(P4_DEV_NAME) < 16 ? strlen(P4_DEV_NAME) : 16);
            ProtoUart_SendFrame(PROTO_HELLO_RSP, frame->seq,
                                hello, sizeof(hello));
        }
        break;

    case PROTO_HELLO_RSP:
        handle_hello_rsp(frame);
        break;

    /* ── 数据上报 ── */
    case PROTO_SENSOR_REPORT:
        handle_sensor_report(frame);
        /* 回 ACK */
        {
            uint8_t ack[2];
            ack[0] = (uint8_t)(frame->seq & 0xFF);
            ack[1] = (uint8_t)((frame->seq >> 8) & 0xFF);
            ProtoUart_SendFrame(PROTO_SENSOR_REPORT_ACK,
                                ProtoTx_NextSeq(), ack, sizeof(ack));
        }
        break;

    case PROTO_ALARM_EVENT:
        handle_alarm_event(frame);
        /* 回 ACK（载荷 acked_seq，与 SENSOR_REPORT_ACK 同构） */
        {
            uint8_t ack[2];
            ack[0] = (uint8_t)(frame->seq & 0xFF);
            ack[1] = (uint8_t)((frame->seq >> 8) & 0xFF);
            ProtoUart_SendFrame(PROTO_ALARM_EVENT_ACK,
                                ProtoTx_NextSeq(), ack, sizeof(ack));
        }
        break;

    case PROTO_ACTUATOR_STATUS:
        /* Payload: count, then (dev_id, state) pairs. */
        if (frame->len >= 1) {
            uint8_t count = frame->payload[0];
            if (count > 4) count = 4;
            if (frame->len >= (uint16_t)(1U + count * 2U)) {
                for (uint8_t i = 0; i < count; i++) {
                    shadow_update_actuator(frame->payload[1U + i * 2U],
                                           frame->payload[2U + i * 2U] ? 1U : 0U);
                }
            }
        }
        ProtoTx_OnAck(frame->seq);
        break;

    case PROTO_STATS_DATA:
        ProtoTx_OnAck(frame->seq);
        break;

    /* ── F103 IAP 结果（进度/最终状态）── */
    case PROTO_OTA_RESULT:
        if (s_ota_result_cb) {
            s_ota_result_cb(frame);
        }
        break;

    default:
        break;
    }
}

/* ════════════════════════════════════════════════════
 *  初始化
 *  ════════════════════════════════════════════════════ */

static void proto_poll_task(void *arg)
{
    (void)arg;
    int64_t last_stats_us = 0;
    while (1) {
        AppProto_Poll();

        int64_t now = esp_timer_get_time();
        if (now - last_stats_us > 5000000) {
            last_stats_us = now;
            proto_stats_t st;
            ProtoUart_GetStats(&st);
            ESP_LOGI(TAG, "STATS  tx_frames=%lu tx_bytes=%lu "
                     "rx_frames=%lu rx_bytes=%lu crc_err=%lu",
                     (unsigned long)st.tx_frames,
                     (unsigned long)st.tx_bytes,
                     (unsigned long)st.rx_frames,
                     (unsigned long)st.rx_bytes,
                     (unsigned long)st.rx_crc_err);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void AppProto_SetOtaResultHandler(app_proto_ota_result_cb_t cb)
{
    s_ota_result_cb = cb;
}

void AppProto_Init(void)
{
    ESP_LOGI(TAG, "Initializing P4 protocol stack...");

    ProtoUart_Init();
    ProtoUart_RegisterRxCb(app_proto_rx_cb);

    memset(&s_link_stats, 0, sizeof(s_link_stats));
    s_link_enter_us = esp_timer_get_time();

    memset(s_sensor_shadow, 0, sizeof(s_sensor_shadow));
    s_sensor_count = 0;

    memset(s_actuator_shadow, 0, sizeof(s_actuator_shadow));
    s_actuator_count = 0;

    s_hello_done = false;

    /* 延迟打点状态复位 */
    s_lat_armed       = false;
    s_lat_start_us    = 0;
    s_lat_samples     = 0;
    s_lat_sum_us      = 0;
    s_lat_min_us      = 0;
    s_lat_min_set     = false;
    s_lat_max_us      = 0;
    s_lat_over_budget = 0;

    /* 启动独立轮询任务，不依赖主循环（避免被 LCD/WiFi 初始化阻塞） */
    xTaskCreate(proto_poll_task, "proto_poll", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Protocol stack ready. UART%d TX=GPIO%d RX=GPIO%d",
             CONFIG_SMART_HOME_PROTO_UART_NUM,
             CONFIG_SMART_HOME_PROTO_UART_TX,
             CONFIG_SMART_HOME_PROTO_UART_RX);
}

/* ════════════════════════════════════════════════════
 *  周期轮询
 *  ════════════════════════════════════════════════════ */

/* HELLO 重试间隔 */
#define HELLO_RETRY_MS  2000U

static int64_t s_last_hello_us;

void AppProto_Poll(void)
{
    /* 收包处理 */
    ProtoUart_Poll();

    /* TX 超时重传 */
    ProtoTx_Poll();

    /* 链路状态机 */
    link_poll();

    /* 延迟打点超时复位：1s 内未收到 ACK 视为无效样本 */
    if (s_lat_armed &&
        (esp_timer_get_time() - s_lat_start_us) > LAT_ARM_TIMEOUT_US) {
        s_lat_armed = false;
        ESP_LOGW(TAG, "LATENCY  no ACK within 1s — sample dropped");
    }

    /* HELLO 协商（首次或重试，直到成功。不依赖链路状态——HELLO 正是建立链路的手段） */
    if (!s_hello_done) {
        int64_t now = esp_timer_get_time();
        if (s_last_hello_us == 0 ||
            (now - s_last_hello_us) > (int64_t)HELLO_RETRY_MS * 1000) {
            s_last_hello_us = now;
            AppProto_RequestHello();
        }
    } else {
        /* HELLO 成功后发 PING 保活，避免 F103 因收不到帧而断开链路。
         * F103 的链路状态机依赖收到帧来维持 LINK_UP，如果主控不发任何帧，
         * F103 会在 3s 后进入 DEGRADED → 6s 后 DOWN → 停止上报传感器数据。
         * 保活周期 2s < F103 的 DEGRADED 阈值 3s，确保链路持续存活。 */
        int64_t now = esp_timer_get_time();
        if (s_last_hello_us == 0 ||
            (now - s_last_hello_us) > (int64_t)HELLO_RETRY_MS * 1000) {
            s_last_hello_us = now;
            ESP_LOGI(TAG, "PING keep-alive");
            ProtoUart_SendFrame(PROTO_PING, ProtoTx_NextSeq(), NULL, 0);
        }
    }
}

/* ════════════════════════════════════════════════════
 *  查询接口
 *  ════════════════════════════════════════════════════ */

link_state_t AppProto_GetLinkState(void)
{
    return s_link_state;
}

void AppProto_GetLinkStats(link_stats_t *out)
{
    if (out) memcpy(out, &s_link_stats, sizeof(s_link_stats));
}

bool AppProto_GetPeerInfo(app_proto_peer_info_t *out)
{
    if (!out) return false;

    memset(out, 0, sizeof(*out));
    taskENTER_CRITICAL(&s_peer_mux);
    out->valid     = s_hello_done;
    out->proto_ver = s_peer_ver;
    out->caps      = (uint16_t)s_peer_caps[0] | ((uint16_t)s_peer_caps[1] << 8);
    memcpy(out->name, s_peer_name, sizeof(out->name) - 1);
    taskEXIT_CRITICAL(&s_peer_mux);
    out->name[sizeof(out->name) - 1] = '\0';
    return out->valid;
}

int AppProto_GetSensorShadow(sensor_shadow_entry_t *out, int max_entries)
{
    int n = (s_sensor_count < max_entries) ? s_sensor_count : max_entries;
    if (out && n > 0) {
        memcpy(out, s_sensor_shadow, (size_t)n * sizeof(sensor_shadow_entry_t));
    }
    return n;
}

bool AppProto_GetActiveAlarm(alarm_shadow_t *out)
{
    if (!s_alarm.active) return false;
    if (out) *out = s_alarm;
    return true;
}

void AppProto_GetAlarmShadow(alarm_shadow_t *out)
{
    /* 与 GetActiveAlarm 的区别：无论是否仍在告警都拷贝。
     * 面板需要"已解除"这一态（active=false 但保留最近事件），
     * 而 GetActiveAlarm 在 false 时不写 out —— 调用方若直接读会拿到未初始化值。 */
    if (out) *out = s_alarm;
}

int AppProto_GetActuatorShadow(actuator_shadow_entry_t *out, int max_entries)
{
    int n = (s_actuator_count < max_entries) ? s_actuator_count : max_entries;
    if (out && n > 0) {
        memcpy(out, s_actuator_shadow,
               (size_t)n * sizeof(actuator_shadow_entry_t));
    }
    return n;
}

bool AppProto_GetLastActuatorFeedback(actuator_feedback_t *out)
{
    if (!out || s_last_actuator_feedback.last_update_us <= 0) return false;
    *out = s_last_actuator_feedback;
    return true;
}

float AppProto_GetSensorValue(uint8_t dev_id)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensor_shadow[i].dev_id == dev_id) {
            return s_sensor_shadow[i].value;
        }
    }
    return NAN;
}

void AppProto_SetLocalSensorValue(uint8_t dev_id, float value)
{
    shadow_update_sensor(dev_id, PROTO_QUAL_VALID, value);
}
