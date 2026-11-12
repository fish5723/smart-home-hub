/**
 * @file boot_board.c
 * @brief bootloader 板级底座（裸机寄存器，不依赖 HAL/newlib）
 */
#include "boot_board.h"
#include "stm32f1xx.h"

#define BOOT_BAUD 115200UL

static uint32_t s_cpu_hz = 8000000UL;
static uint32_t s_pclk1_hz = 8000000UL;

static uint32_t wait_set(volatile uint32_t *reg, uint32_t mask, uint32_t timeout)
{
    while ((*reg & mask) == 0U) {
        if (timeout-- == 0U) return 0U;
    }
    return 1U;
}

static void clock_init(void)
{
    uint32_t hse_ok;

    RCC->CR |= RCC_CR_HSEON;
    hse_ok = wait_set(&RCC->CR, RCC_CR_HSERDY, 200000U);

    if (hse_ok) {
        /* 72MHz 需要 2 个等待周期 */
        FLASH->ACR = (FLASH->ACR & ~0x7UL) | FLASH_ACR_LATENCY_2 | FLASH_ACR_PRFTBE;

        RCC->CFGR = (RCC->CFGR &
                     ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 |
                       RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL)) |
                    RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
        RCC->CR |= RCC_CR_PLLON;
        if (wait_set(&RCC->CR, RCC_CR_PLLRDY, 200000U)) {
            RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
            if (wait_set(&RCC->CFGR, RCC_CFGR_SWS_PLL, 200000U)) {
                s_cpu_hz   = 72000000UL;
                s_pclk1_hz = 36000000UL;
                return;
            }
        }
    }

    /* HSE/PLL 失败：保持 HSI 8MHz，APB1 = 8MHz */
    s_cpu_hz   = 8000000UL;
    s_pclk1_hz = 8000000UL;
}

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void uart_init(void)
{
    /* PB10 = USART3_TX（AF push-pull 50MHz），PB11 = 浮空输入 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;

    GPIOB->CRH = (GPIOB->CRH & ~(0xFFUL << 8)) |
                 (0xBUL << 8) |   /* PB10: MODE=11, CNF=10 */
                 (0x4UL << 12);   /* PB11: MODE=00, CNF=01 */

    USART3->BRR = (s_pclk1_hz + BOOT_BAUD / 2U) / BOOT_BAUD;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE;
}

void boot_iwdg_init(void)
{
    /* LSI ~40kHz，PR=128（PR=5），RLR=2500 → 标称 8s */
    IWDG->KR  = 0x5555U;
    IWDG->PR  = 5U;
    IWDG->RLR = 2500U;
    IWDG->KR  = 0xAAAAU;
    IWDG->KR  = 0xCCCCU;
}

void boot_iwdg_feed(void)
{
    IWDG->KR = 0xAAAAU;
}

void boot_board_init(void)
{
    clock_init();
    dwt_init();
    boot_iwdg_init();
    uart_init();
}

uint32_t boot_cpu_hz(void)
{
    return s_cpu_hz;
}

void boot_delay_ms(uint32_t ms)
{
    uint32_t cycles = ms * (s_cpu_hz / 1000UL);
    uint32_t start = DWT->CYCCNT;

    while ((DWT->CYCCNT - start) < cycles) {
        boot_iwdg_feed();
    }
}

static void uart_putc(char c)
{
    while ((USART3->SR & USART_SR_TXE) == 0U) { }
    USART3->DR = (uint8_t)c;
}

void boot_log(const char *text)
{
    while (*text) uart_putc(*text++);
}

void boot_log_hex(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    boot_log("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(hex[(value >> (uint32_t)shift) & 0xFU]);
    }
}

void boot_log_u32(uint32_t value)
{
    char buf[11];
    int n = 0;

    if (value == 0U) {
        uart_putc('0');
        return;
    }
    while (value && n < 10) {
        buf[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (n--) uart_putc(buf[n]);
}
