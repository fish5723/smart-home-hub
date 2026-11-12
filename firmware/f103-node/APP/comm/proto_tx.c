/**
 * @file proto_tx.c
 * @brief 协议发送管理 — ACK 超时 / 重传 / 去重表
 *
 * 设计要点:
 *   - 单待确认帧模型: 同一时刻只有 1 个帧等待 ACK
 *   - 重传: 超时后原样重发，最多 3 次，超限后通知上层放弃
 *   - 去重表: 8 项环形，收到重复 SEQ → 丢弃载荷但重新回 ACK
 *   - 不需要 ACK 的类型按 fire-and-forget 发送
 */
#include "proto_tx.h"
#include "proto_uart.h"
#include "main.h"
#include <string.h>

/* ════════════════════════════════════════════════════
 *  待确认帧
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  active;
    uint8_t  type;
    uint16_t seq;
    uint16_t len;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
    uint32_t send_time_ms;
    uint8_t  retry_count;
} proto_pending_t;

static proto_pending_t s_pending;

/* ════════════════════════════════════════════════════
 *  去重表（接收侧）
 *  ════════════════════════════════════════════════════ */

typedef struct {
    uint16_t seq;
    uint8_t  valid;
} proto_dedup_t;

static proto_dedup_t s_dedup[PROTO_DEDUP_SIZE];
static uint8_t       s_dedup_idx;

/* ════════════════════════════════════════════════════
 *  序列号
 *  ════════════════════════════════════════════════════ */

static uint16_t s_next_seq = 1;

/* ════════════════════════════════════════════════════
 *  统计
 *  ════════════════════════════════════════════════════ */

static proto_tx_stats_t s_stats;

/* ════════════════════════════════════════════════════
 *  回调（超限通知）
 *  ════════════════════════════════════════════════════ */

static void (*s_on_dropped)(uint8_t type, uint16_t seq);

/* ════════════════════════════════════════════════════
 *  辅助: 判断类型是否需要 ACK
 *  ════════════════════════════════════════════════════ */

static int type_needs_ack(uint8_t type) __attribute__((unused));
static int type_needs_ack(uint8_t type)
{
    switch (type) {
    case PROTO_SENSOR_REPORT:
    case PROTO_ALARM_EVENT:
    case PROTO_ACTUATOR_CMD:
    case PROTO_CONFIG_GET:
    case PROTO_CONFIG_SET:
    case PROTO_OTA_BEGIN:
    case PROTO_OTA_DATA:
    case PROTO_OTA_END:
        return 1;
    default:
        return 0;
    }
}

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

void ProtoTx_Init(void)
{
    memset((void *)&s_pending, 0, sizeof(s_pending));
    memset((void *)s_dedup, 0, sizeof(s_dedup));
    memset((void *)&s_stats, 0, sizeof(s_stats));
    s_dedup_idx = 0;
    s_next_seq  = 1;
}

uint16_t ProtoTx_NextSeq(void)
{
    uint16_t seq = s_next_seq++;
    if (s_next_seq == 0) s_next_seq = 1; /* 0 保留 */
    return seq;
}

int ProtoTx_SendFireAndForget(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint16_t seq = ProtoTx_NextSeq();
    int ret = ProtoUart_SendFrame(type, seq, payload, len);
    if (ret == 0) s_stats.tx_frames++;
    return ret;
}

uint16_t ProtoTx_SendWithAck(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (s_pending.active) {
        return 0; /* 上一帧还在等待 ACK，拒绝新请求 */
    }

    uint16_t seq = ProtoTx_NextSeq();
    if (len > PROTO_MAX_PAYLOAD) return 0;

    int ret = ProtoUart_SendFrame(type, seq, payload, len);
    if (ret != 0) return 0;

    s_pending.active        = 1;
    s_pending.type          = type;
    s_pending.seq           = seq;
    s_pending.len           = len;
    s_pending.retry_count   = 0;
    s_pending.send_time_ms  = HAL_GetTick();
    if (len > 0 && payload) {
        memcpy(s_pending.payload, payload, len);
    }

    s_stats.tx_frames++;
    return seq;
}

void ProtoTx_Poll(void)
{
    if (!s_pending.active) return;

    uint32_t now = HAL_GetTick();
    uint32_t elapsed;

    /* 处理 tick 回绕 */
    if (now >= s_pending.send_time_ms) {
        elapsed = now - s_pending.send_time_ms;
    } else {
        elapsed = (0xFFFFFFFFUL - s_pending.send_time_ms) + now + 1;
    }

    if (elapsed < PROTO_ACK_TIMEOUT_MS) return;

    /* 超时 */
    s_stats.tx_timeouts++;

    if (s_pending.retry_count >= PROTO_MAX_RETRANS) {
        /* 超过最大重传次数，放弃 */
        s_stats.tx_dropped++;
        if (s_on_dropped) {
            s_on_dropped(s_pending.type, s_pending.seq);
        }
        s_pending.active = 0;
        return;
    }

    /* 重传 */
    s_pending.retry_count++;
    s_stats.tx_retrans++;

    ProtoUart_SendFrame(s_pending.type, s_pending.seq,
                        s_pending.payload, s_pending.len);
    s_pending.send_time_ms = HAL_GetTick();
}

void ProtoTx_OnAck(uint16_t acked_seq)
{
    if (s_pending.active && s_pending.seq == acked_seq) {
        s_pending.active = 0;
        s_stats.tx_ack_ok++;
    }
}

void ProtoTx_OnNack(uint16_t nacked_seq, uint8_t err_code)
{
    (void)err_code;
    if (s_pending.active && s_pending.seq == nacked_seq) {
        /* NACK = 对端明确拒绝，不重传 */
        s_pending.active = 0;
        s_stats.tx_dropped++;
    }
}

/* ════════════════════════════════════════════════════
 *  去重表
 *  ════════════════════════════════════════════════════ */

int ProtoTx_IsDuplicate(uint16_t seq)
{
    for (uint8_t i = 0; i < PROTO_DEDUP_SIZE; i++) {
        if (s_dedup[i].valid && s_dedup[i].seq == seq) {
            s_stats.rx_dedup_hits++;
            return 1;
        }
    }
    return 0;
}

void ProtoTx_RecordSeq(uint16_t seq)
{
    s_dedup[s_dedup_idx].seq   = seq;
    s_dedup[s_dedup_idx].valid = 1;
    s_dedup_idx = (s_dedup_idx + 1) % PROTO_DEDUP_SIZE;
}

/* ════════════════════════════════════════════════════
 *  统计
 *  ════════════════════════════════════════════════════ */

void ProtoTx_GetStats(proto_tx_stats_t *out)
{
    if (out) memcpy(out, &s_stats, sizeof(proto_tx_stats_t));
}