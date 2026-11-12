/**
 * @file proto_frame.c
 * @brief 协议帧纯逻辑层实现（见 proto_frame.h）
 *
 * 行为与阶段 2A/2B 联调验证过的 proto_uart.c 状态机逐字节等价，
 * 仅去掉了对 DMA 环形缓冲与 printf 的依赖：
 *   - 旧 rx_resync() 的"向前扫描 0xAA"与回 IDLE 后逐字节匹配 SOF1 等价，
 *     统一为 reset 到 IDLE（IDLE 天然忽略非 0xAA 字节），少一份状态
 *   - 联调期 CRC 失败字节 dump（T-25）随 printf 移出纯逻辑层；
 *     链路已用 5h 压测验证，传输层 5s 统计行足够定位
 */
#include "proto_frame.h"
#include "crc16.h"
#include <string.h>

/* 状态编号（与 docs/03 §7 图对应） */
enum {
    PSTATE_IDLE = 0,
    PSTATE_SOF2,
    PSTATE_HEADER,
    PSTATE_PAYLOAD,
    PSTATE_CRC,
};

void proto_parser_init(proto_parser_t *p, proto_frame_cb_t cb, void *user)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->on_frame = cb;
    p->user     = user;
}

static void parser_reset(proto_parser_t *p)
{
    p->state       = PSTATE_IDLE;
    p->header_idx  = 0;
    p->payload_len = 0;
    p->payload_idx = 0;
    p->crc_idx     = 0;
}

void proto_parser_abort(proto_parser_t *p)
{
    if (!p || p->state == PSTATE_IDLE) return;
    p->stats.resyncs++;
    parser_reset(p);
}

uint8_t proto_parser_in_frame(const proto_parser_t *p)
{
    return (p && p->state != PSTATE_IDLE) ? 1U : 0U;
}

void proto_parser_reset_stats(proto_parser_t *p)
{
    if (p) memset(&p->stats, 0, sizeof(p->stats));
}

void proto_parser_feed(proto_parser_t *p, uint8_t b)
{
    switch (p->state) {

    case PSTATE_IDLE:
        if (b == PROTO_SOF1) {
            p->state = PSTATE_SOF2;
        }
        break;

    case PSTATE_SOF2:
        if (b == PROTO_SOF2) {
            p->state      = PSTATE_HEADER;
            p->header_idx = 0;
        } else if (b == PROTO_SOF1) {
            /* 连续 0xAA 0xAA：保持等 0x55 */
        } else {
            p->state = PSTATE_IDLE;
        }
        break;

    case PSTATE_HEADER:
        p->header[p->header_idx++] = b;
        if (p->header_idx >= 6) {
            uint8_t  ver = p->header[0];
            uint16_t len = (uint16_t)p->header[4]
                         | ((uint16_t)p->header[5] << 8);

            /* LEN 校验必须早于任何按 LEN 的写入（规范 §7.3，
             * 恶意 LEN 不得写越 256B 载荷缓冲） */
            if (len > PROTO_MAX_PAYLOAD || ver != PROTO_VER) {
                p->stats.len_errors++;
                p->stats.resyncs++;
                parser_reset(p);
                return;
            }

            p->payload_len = len;
            if (len == 0) {
                p->state   = PSTATE_CRC;
                p->crc_idx = 0;
            } else {
                p->state       = PSTATE_PAYLOAD;
                p->payload_idx = 0;
            }
        }
        break;

    case PSTATE_PAYLOAD:
        p->payload[p->payload_idx++] = b;
        if (p->payload_idx >= p->payload_len) {
            p->state   = PSTATE_CRC;
            p->crc_idx = 0;
        }
        break;

    case PSTATE_CRC:
        p->crc_buf[p->crc_idx++] = b;
        if (p->crc_idx >= 2) {
            uint16_t rx_crc = (uint16_t)p->crc_buf[0]
                            | ((uint16_t)p->crc_buf[1] << 8);

            /* CRC 覆盖 VER → PAYLOAD 末尾（SOF 不参与） */
            uint8_t check_buf[6 + PROTO_MAX_PAYLOAD];
            memcpy(check_buf, p->header, 6);
            if (p->payload_len > 0) {
                memcpy(check_buf + 6, p->payload, p->payload_len);
            }
            uint16_t calc_crc = crc16_compute(check_buf,
                                              (uint16_t)(6 + p->payload_len));

            if (calc_crc != rx_crc) {
                p->stats.crc_errors++;
                p->stats.resyncs++;
                parser_reset(p);
                return;
            }

            p->stats.rx_frames++;

            if (p->on_frame) {
                proto_frame_t frame;
                frame.ver  = p->header[0];
                frame.type = p->header[1];
                frame.seq  = (uint16_t)p->header[2]
                           | ((uint16_t)p->header[3] << 8);
                frame.len  = p->payload_len;
                frame.crc  = rx_crc;
                if (p->payload_len > 0) {
                    memcpy(frame.payload, p->payload, p->payload_len);
                }
                p->on_frame(&frame, p->user);
            }

            parser_reset(p);
        }
        break;

    default:
        parser_reset(p);
        break;
    }
}

void proto_parser_feed_buf(proto_parser_t *p, const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        proto_parser_feed(p, buf[i]);
    }
}

uint16_t proto_frame_build(uint8_t type, uint16_t seq,
                           const uint8_t *payload, uint16_t len,
                           uint8_t *out)
{
    if (!out || len > PROTO_MAX_PAYLOAD) return 0;
    if (len > 0 && !payload) return 0;

    uint16_t pos = 0;
    out[pos++] = PROTO_SOF1;
    out[pos++] = PROTO_SOF2;
    out[pos++] = PROTO_VER;
    out[pos++] = type;
    out[pos++] = (uint8_t)(seq & 0xFF);
    out[pos++] = (uint8_t)((seq >> 8) & 0xFF);
    out[pos++] = (uint8_t)(len & 0xFF);
    out[pos++] = (uint8_t)((len >> 8) & 0xFF);

    if (len > 0) {
        memcpy(out + pos, payload, len);
        pos = (uint16_t)(pos + len);
    }

    uint16_t crc = crc16_compute(out + 2, (uint16_t)(pos - 2));
    out[pos++] = (uint8_t)(crc & 0xFF);
    out[pos++] = (uint8_t)((crc >> 8) & 0xFF);
    return pos;
}
