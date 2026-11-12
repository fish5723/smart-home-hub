/**
 * @file proto_tx.c
 * @brief 发送管理层 — ACK 等待 / 超时重传 / 去重判定
 *
 * 与 F103 侧 proto_tx.c 接口兼容，独立实现。
 * 使用 esp_timer_get_time() 获取高精度时间戳。
 */
#include "proto_tx.h"
#include "proto_uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "proto_tx";

/* ════════════════════════════════════════════════════
 *  待确认帧
 *  ════════════════════════════════════════════════════ */

typedef struct {
    bool     pending;
    uint8_t  type;
    uint16_t seq;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
    uint16_t len;
    int64_t  send_time_us;
    uint8_t  retry_count;
} pending_tx_t;

static pending_tx_t s_pending;
static uint16_t     s_next_seq;
static uint32_t     s_tx_sent;
static uint32_t     s_tx_acked;
static uint32_t     s_tx_dropped;
static uint32_t     s_tx_retrans;

/* ════════════════════════════════════════════════════
 *  去重表
 *  ════════════════════════════════════════════════════ */

static uint16_t s_dedup_seq[PROTO_DEDUP_SIZE];
static uint8_t  s_dedup_head;

/* ════════════════════════════════════════════════════
 *  序号管理
 *  ════════════════════════════════════════════════════ */

uint16_t ProtoTx_NextSeq(void)
{
    return s_next_seq++;
}

/* ════════════════════════════════════════════════════
 *  发送
 *  ════════════════════════════════════════════════════ */

void ProtoTx_SendWithAck(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (len > PROTO_MAX_PAYLOAD) len = PROTO_MAX_PAYLOAD;

    if (s_pending.pending) {
        s_tx_dropped++;
        ESP_LOGW(TAG, "TX BUSY: drop type=0x%02X len=%u (pending type=0x%02X seq=%u)",
                 (unsigned)type, (unsigned)len,
                 (unsigned)s_pending.type, (unsigned)s_pending.seq);
        return;
    }

    uint16_t seq = ProtoTx_NextSeq();
    ProtoUart_SendFrame(type, seq, payload, len);

    s_pending.pending     = true;
    s_pending.type        = type;
    s_pending.seq         = seq;
    s_pending.len         = len;
    s_pending.send_time_us = esp_timer_get_time();
    s_pending.retry_count = 0;
    if (len > 0 && payload != NULL) {
        memcpy(s_pending.payload, payload, len);
    }
    s_tx_sent++;
    ESP_LOGI(TAG, "TX reliable type=0x%02X seq=%u len=%u",
             (unsigned)type, (unsigned)seq, (unsigned)len);
}

void ProtoTx_SendNoAck(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (len > PROTO_MAX_PAYLOAD) len = PROTO_MAX_PAYLOAD;
    ProtoUart_SendFrame(type, ProtoTx_NextSeq(), payload, len);
    s_tx_sent++;
}

bool ProtoTx_IsBusy(void)
{
    return s_pending.pending;
}

/* ════════════════════════════════════════════════════
 *  ACK / NACK 处理
 *  ════════════════════════════════════════════════════ */

void ProtoTx_OnAck(uint16_t seq)
{
    if (s_pending.pending && s_pending.seq == seq) {
        s_pending.pending = false;
        s_tx_acked++;
    }
}

void ProtoTx_OnNack(uint16_t seq, uint8_t error_code)
{
    (void)error_code;
    if (s_pending.pending && s_pending.seq == seq) {
        s_pending.pending = false;
        s_tx_dropped++;
    }
}

/* ════════════════════════════════════════════════════
 *  去重
 *  ════════════════════════════════════════════════════ */

bool ProtoTx_IsDuplicate(uint16_t seq)
{
    for (int i = 0; i < PROTO_DEDUP_SIZE; i++) {
        if (s_dedup_seq[i] == seq) return true;
    }
    return false;
}

void ProtoTx_RecordSeq(uint16_t seq)
{
    s_dedup_seq[s_dedup_head % PROTO_DEDUP_SIZE] = seq;
    s_dedup_head++;
}

/* ════════════════════════════════════════════════════
 *  超时重传轮询
 *  ════════════════════════════════════════════════════ */

void ProtoTx_Poll(void)
{
    if (!s_pending.pending) return;

    int64_t now = esp_timer_get_time();
    int64_t elapsed_us = now - s_pending.send_time_us;
    int64_t timeout_us = (int64_t)PROTO_ACK_TIMEOUT_MS * 1000;

    if (elapsed_us < timeout_us) return;

    if (s_pending.retry_count >= PROTO_MAX_RETRANS) {
        s_pending.pending = false;
        s_tx_dropped++;
        return;
    }

    s_pending.retry_count++;
    s_tx_retrans++;
    s_pending.send_time_us = now;
    ProtoUart_SendFrame(s_pending.type, s_pending.seq,
                        s_pending.payload, s_pending.len);
}
