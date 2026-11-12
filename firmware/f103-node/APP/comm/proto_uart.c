/**
 * @file proto_uart.c
 * @brief 协议 UART 传输层 — DMA 环形缓冲 + IDLE 中断 + 纯逻辑解析器接入（USART1）
 *
 * 数据流:
 *   USART1 RX → DMA1_Ch5（循环模式）→ rx_ring[1024]
 *                      ↓
 *        ProtoUart_Poll()（调度器高优先级轮询）
 *           → 模差计算可用字节 → 喂 proto_frame.c 状态机
 *           → CRC 通过的帧投递 proto_rx_callback
 *
 * 职责边界（阶段 2C-1 分层重构）:
 *   - 字节流状态机 / 帧组装在 proto_frame.c（纯逻辑，PC 可单测）
 *   - 本文件只做：DMA 读写指针管理、帧内超时、喂狗、TX、统计汇总
 *
 * 约束:
 *   - ISR 里绝不解析帧、做 CRC、发 ACK、打印日志
 *   - USART1 只跑二进制帧，不出现任何 printf
 *   - TX 走 DMA1_Ch4，失败回退阻塞
 */
#include "proto_uart.h"
#include "proto_frame.h"
#include "usart.h"
#include "dma.h"
#include "crc16.h"
#include "bsp_iwdg.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════
 *  RX 环形缓冲（DMA 循环写入）
 *  ════════════════════════════════════════════════════ */

#define RX_BUF_SIZE         1024U
#define RX_BUF_MASK         (RX_BUF_SIZE - 1U)

static uint8_t              rx_ring[RX_BUF_SIZE];

static proto_rx_callback_t  g_rx_cb;
static proto_parser_t       g_parser;

/* 传输层自己的计数（解析器内部的计数在 g_parser.stats） */
static proto_stats_t        g_stats;

/* ── RX DMA 自愈 ──
 * F1 HAL 把 UART 错误（FE/NE/ORE）在 DMA 接收模式下视为阻塞性错误：
 * UART_EndRxTransfer + HAL_DMA_Abort_IT 会清 CR3.DMAR 并关闭 DMA 通道，
 * 之后 HAL_UART_ErrorCallback 是弱空函数，接收永久停摆，只能复位 MCU。
 * 对端（P4）复位瞬间 TX 线毛刺正好会触发该路径。 */
static volatile uint8_t     g_rx_need_rearm;
static uint32_t             g_rx_rearm_count;

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        g_rx_need_rearm = 1U;   /* 只置标志，重武装放主上下文 */
    }
}

/* ════════════════════════════════════════════════════
 *  环形缓冲可用量
 *  ════════════════════════════════════════════════════ */

/* 消费者位置（模 RX_BUF_SIZE 的环形索引）。
 * ⚠️ 阶段 2 联调教训（rx_frames=0 且 avail=0 的根因）：
 * 原实现维护"绝对位置 = wraps×SIZE + (SIZE-CNDTR)"，其中 wraps 由
 * DMA TCIF 标志的预补偿 + 中断服务双重维护——两者存在经典竞态
 * （补偿与 ISR 各加一次 = 虚高 1024），消费指针与 DMA 写指针从此
 * 永久错位：状态机吃着错位的旧数据，0 帧 0 错误地静默吞掉一切。
 * 修复：只维护模 SIZE 的消费指针，与 DMA 的模 SIZE 写指针做模差，
 * 不再有 wrap 计数，竞态无处发生。缓冲 1024B vs 流量 ~75B/s，
 * 10ms 轮询永不触满（满缓冲歧义需要 1 字节裕量，此处流量下不存在）。 */
static volatile uint16_t    rx_consume_pos;

static uint32_t rx_avail(void)
{
    /* DMA 写指针（模 SIZE）：CNDTR 是剩余计数，写位置 = SIZE - CNDTR */
    uint16_t write_pos = (uint16_t)(RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx));
    /* 模差：环形缓冲中的可用字节数（无符号回绕天然处理跨 0） */
    return (uint32_t)((uint16_t)(write_pos - rx_consume_pos) & RX_BUF_MASK);
}

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

static void parser_dispatch(const proto_frame_t *frame, void *user)
{
    (void)user;
    if (g_rx_cb) {
        g_rx_cb(frame);
    }
}

/* 重新武装 RX DMA（主上下文调用）。
 * HAL 错误路径用 Abort_IT 异步中止，hdma 状态可能仍是 BUSY，
 * 先同步 AbortReceive 补齐状态，否则 Receive_DMA 会返回 HAL_BUSY。 */
static void proto_uart_rx_rearm(void)
{
    /* 读 SR + 读 DR 清 ORE/FE/NE，否则重开 DMA 后错误标志仍在 */
    volatile uint32_t sr = huart1.Instance->SR;
    volatile uint32_t dr = huart1.Instance->DR;
    (void)sr;
    (void)dr;

    (void)HAL_UART_AbortReceive(&huart1);
    /* AbortReceive 在 DMAR 已被 HAL 清掉时不会触碰 DMA，这里强制同步中止，
     * 确保 hdma 状态回到 READY，否则 Receive_DMA 会返回 HAL_BUSY */
    (void)HAL_DMA_Abort(&hdma_usart1_rx);

    rx_consume_pos = 0U;
    proto_parser_init(&g_parser, parser_dispatch, 0);

    if (HAL_UART_Receive_DMA(&huart1, rx_ring, RX_BUF_SIZE) == HAL_OK) {
        __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
        g_rx_rearm_count++;
        printf("[PROTO-UART] RX DMA re-armed (#%lu)\r\n",
               (unsigned long)g_rx_rearm_count);
    } else {
        g_rx_need_rearm = 1U;   /* 本轮失败，下轮重试 */
    }
}

void ProtoUart_Init(void)
{
    crc16_init();

    rx_consume_pos = 0;
    proto_parser_init(&g_parser, parser_dispatch, 0);
    memset(&g_stats, 0, sizeof(g_stats));
    g_rx_cb = 0;

    /* 启动 DMA 循环接收 */
    HAL_UART_Receive_DMA(&huart1, rx_ring, RX_BUF_SIZE);

    /* 启用 IDLE 中断 */
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
}

void ProtoUart_SetRxCallback(proto_rx_callback_t cb)
{
    g_rx_cb = cb;
}

/* ── 由 USART1_IRQHandler 调用（ISR 上下文） ──
 * 职责仅剩清除 IDLE 标志（读 SR + 读 DR 的 F1 清除序列）。
 * 数据的消费不依赖本中断——ProtoUart_Poll 每轮用模差直接
 * 对比 CNDTR 与消费指针，见 rx_avail()。 */
void ProtoUart_NotifyIdle(void)
{
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE)) {
        volatile uint32_t sr = huart1.Instance->SR;
        volatile uint32_t dr = huart1.Instance->DR;
        (void)sr;
        (void)dr;
    }
}

void ProtoUart_Poll(void)
{
    /* RX 通路自检：以硬件位为权威判据，任何原因造成的接收停摆都能发现
     * （HAL 错误中止后 CR3.DMAR 与 DMA_CCR.EN 都会变成 0）。 */
    if (g_rx_need_rearm ||
        (hdma_usart1_rx.Instance->CCR & DMA_CCR_EN) == 0U ||
        (huart1.Instance->CR3 & USART_CR3_DMAR) == 0U) {
        g_rx_need_rearm = 0U;
        proto_uart_rx_rearm();
    }

    /* 帧内超时（规范 §7.4）：状态机停在半帧超过 PROTO_IDLE_TIMEOUT_MS
     * 视为帧中断，安全丢弃。轮询周期 ~10ms，判定粒度足够。 */
    static uint32_t last_byte_tick = 0;
    uint32_t now = HAL_GetTick();
    if (proto_parser_in_frame(&g_parser) &&
        (now - last_byte_tick) > PROTO_IDLE_TIMEOUT_MS) {
        proto_parser_abort(&g_parser);
    }

    /* 消费所有可读字节（模差计算，无 wrap 计数——见 rx_avail 修复注释） */
    uint32_t avail;
    uint32_t byte_cnt = 0;
    while ((avail = rx_avail()) > 0) {
        uint8_t b = rx_ring[rx_consume_pos];
        rx_consume_pos = (uint16_t)((rx_consume_pos + 1) & RX_BUF_MASK);
        proto_parser_feed(&g_parser, b);
        g_stats.drops += (avail > RX_BUF_SIZE / 2) ? 1 : 0;

        /* 处理大批字节时定期喂狗（每 256 字节） */
        if (++byte_cnt >= 256U) {
            byte_cnt = 0;
            BSP_IWDG_Feed();
        }
    }
    if (byte_cnt > 0) {
        last_byte_tick = now;
    }

    /* 诊断：每 5s 打印收包统计 */
    static uint32_t last_diag = 0;
    static uint32_t last_rx_frames = 0;
    if (now - last_diag >= 5000U) {
        uint32_t new_frames = g_parser.stats.rx_frames - last_rx_frames;
        printf("[PROTO-UART] rx_frames=%lu/%lu err_len=%lu "
               "err_crc=%lu drops=%lu resyncs=%lu tx=%lu "
               "dma_pos=%u avail=%lu\r\n",
               (unsigned long)new_frames,
               (unsigned long)g_parser.stats.rx_frames,
               (unsigned long)g_parser.stats.len_errors,
               (unsigned long)g_parser.stats.crc_errors,
               (unsigned long)g_stats.drops,
               (unsigned long)g_parser.stats.resyncs,
               (unsigned long)g_stats.tx_bytes,
               (unsigned)(RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx)),
               (unsigned long)rx_avail());
        last_diag = now;
        last_rx_frames = g_parser.stats.rx_frames;
    }
}

/* ════════════════════════════════════════════════════
 *  发包（DMA，失败回退阻塞；帧组装在 proto_frame.c）
 *  ════════════════════════════════════════════════════ */

int ProtoUart_SendRaw(const uint8_t *buf, uint16_t len)
{
    if (len == 0 || buf == 0) return -1;

    /* Do not start DMA with the caller's buffer: ProtoUart_SendFrame builds
     * frames on its stack, so an asynchronous DMA transfer would read freed
     * or overwritten bytes after this function returns.  At 115200 baud the
     * largest protocol frame occupies only a few milliseconds; blocking TX
     * is the safe path for command ACKs and sensor reports. */
    HAL_StatusTypeDef status = HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, 100);
    if (status != HAL_OK) {
        BSP_IWDG_Feed();
    }
    g_stats.tx_bytes += len;
    return status == HAL_OK ? 0 : -1;
}

int ProtoUart_SendFrame(uint8_t type, uint16_t seq,
                        const uint8_t *payload, uint16_t len)
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t total = proto_frame_build(type, seq, payload, len, buf);
    if (total == 0) return -1;
    return ProtoUart_SendRaw(buf, total);
}

/* ════════════════════════════════════════════════════
 *  统计（解析器 + 传输层汇总，可经 STATS_GET 上报）
 *  ════════════════════════════════════════════════════ */

void ProtoUart_GetStats(proto_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->rx_frames  = g_parser.stats.rx_frames;
    out->crc_errors = g_parser.stats.crc_errors;
    out->len_errors = g_parser.stats.len_errors;
    out->resyncs    = g_parser.stats.resyncs;
    out->drops      = g_stats.drops;
    out->tx_bytes   = g_stats.tx_bytes;
}

void ProtoUart_ResetStats(void)
{
    proto_parser_reset_stats(&g_parser);
    memset(&g_stats, 0, sizeof(g_stats));
}

uint32_t ProtoUart_GetRxDropped(void)
{
    return g_stats.drops;
}
