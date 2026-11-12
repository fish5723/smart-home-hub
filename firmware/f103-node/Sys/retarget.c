/**
 * @file retarget.c
 * @brief printf 重定向实现 — 环形缓冲 + DMA（USART3）
 *
 * 来源：前序工程 USER/Retarget/retarget.c（原为阻塞单字节发送）
 * 本工程修改：
 *   - 阶段 1：改为 1KB 环形缓冲 + DMA1_Ch2 后台发送，失败回退阻塞发送。
 *   - 这是本阶段质量最好的改动之一 —— 原实现每 100 字节阻塞 8.7ms
 */
//
// Created by A on 2026/3/22.
//
// 阶段 1 优化：UART DMA 环形缓冲 — 混合策略
//   正常路径：环形缓冲 1KB → DMA1 CH2 (USART3_TX) 后台发送
//   回退路径：DMA 失败/超时 → 阻塞 HAL_UART_Transmit
//
#include <_ansi.h>
#include <_syslist.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/times.h>
#include "retarget.h"
#include "usart.h"
#include <stdint.h>
#if !defined(OS_USE_SEMIHOSTING)

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ════════════════════════════════════════════════════
 *  DMA 环形缓冲（TX）
 *  ════════════════════════════════════════════════════ */
#define TX_RING_SIZE  1024U
#define TX_RING_MASK  (TX_RING_SIZE - 1U)
#define DMA_TIMEOUT   7200000UL  /* 约 100ms @ 72MHz */

static UART_HandleTypeDef  *gHuart;

static uint8_t              tx_ring[TX_RING_SIZE];
static volatile uint16_t    tx_head;        /* _write 写入位置 */
static volatile uint16_t    tx_tail;        /* 已发送确认位置  */
static volatile uint8_t     tx_dma_busy;    /* DMA 传输中标记  */

/* DMA 健康追踪 */
static volatile uint32_t    tx_fail_cnt;    /* 累计失败次数    */
static          uint8_t     tx_dma_ok;      /* DMA 当前可用    */

/* ── 阻塞 fallback（清理 DMA 残留后发） ── */
static void tx_flush_blocking(void)
{
    uint16_t used;

    CLEAR_BIT(gHuart->Instance->CR3, USART_CR3_DMAT);
    __HAL_UART_DISABLE_IT(gHuart, UART_IT_TC);
    gHuart->gState = HAL_UART_STATE_READY;

    while ((used = (tx_head - tx_tail) & TX_RING_MASK) != 0) {
        uint16_t chunk = used;
        uint16_t until_wrap = TX_RING_SIZE - tx_tail;
        if (chunk > until_wrap) chunk = until_wrap;
        HAL_UART_Transmit(gHuart, &tx_ring[tx_tail], chunk, 100);
        tx_tail = (tx_tail + chunk) & TX_RING_MASK;
    }
    tx_dma_busy = 0;
}

static void tx_dma_kick(void)
{
    if (!tx_dma_ok) {
        tx_flush_blocking();
        return;
    }
    if (tx_dma_busy) return;

    uint16_t used = (tx_head - tx_tail) & TX_RING_MASK;
    if (used == 0) return;

    uint16_t chunk = used;
    uint16_t until_wrap = TX_RING_SIZE - tx_tail;
    if (chunk > until_wrap) chunk = until_wrap;

    tx_dma_busy = 1;
    if (HAL_UART_Transmit_DMA(gHuart, &tx_ring[tx_tail], chunk) != HAL_OK) {
        tx_dma_busy = 0;
        tx_fail_cnt++;
        if (tx_fail_cnt >= 3) tx_dma_ok = 0;
        tx_flush_blocking();
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == gHuart) {
        tx_tail = (tx_tail + gHuart->TxXferSize) & TX_RING_MASK;
        tx_dma_busy = 0;
        tx_dma_ok = 1;
        tx_fail_cnt = 0;
        tx_dma_kick();
    }
}

void RetargetInit(UART_HandleTypeDef *huart)
{
    gHuart   = huart;
    tx_head  = 0;
    tx_tail  = 0;
    tx_dma_busy = 0;
    tx_fail_cnt = 0;
    tx_dma_ok   = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
}

void RetargetFlush(void)
{
    while (tx_head != tx_tail) {
        if (tx_dma_ok) tx_dma_kick();
        else           tx_flush_blocking();
    }
    while (tx_dma_busy) {}
}

int _isatty(int fd)
{
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return 1;
    errno = EBADF;
    return 0;
}

int _write(int fd, char *ptr, int len)
{
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        for (int i = 0; i < len; i++) {
            uint32_t timeout = DMA_TIMEOUT;
            while (((tx_head + 1) & TX_RING_MASK) == tx_tail) {
                if (--timeout == 0) {
                    tx_dma_ok = 0;
                    tx_flush_blocking();
                    break;
                }
            }
            tx_ring[tx_head] = (uint8_t)ptr[i];
            tx_head = (tx_head + 1) & TX_RING_MASK;
        }
        tx_dma_kick();
        return len;
    }
    errno = EBADF;
    return -1;
}

int _close(int fd)
{
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return 0;
    errno = EBADF;
    return -1;
}

int _lseek(int fd, int ptr, int dir)
{
    (void)fd; (void)ptr; (void)dir;
    errno = EBADF;
    return -1;
}

int _read(int fd, char *ptr, int len)
{
    if (fd == STDIN_FILENO) {
        if (HAL_UART_Receive(gHuart, (uint8_t *)ptr, 1, 100U) == HAL_OK)
            return 1;
        errno = EIO;
        return -1;
    }
    errno = EBADF;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    errno = EBADF;
    return 0;
}

#endif