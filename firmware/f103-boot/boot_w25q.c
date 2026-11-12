/**
 * @file boot_w25q.c
 * @brief bootloader 裸机 SPI1 + W25Q 系列 SPI Flash
 *
 * 引脚：PA5/PA6/PA7 = SPI1 SCK/MISO/MOSI，PA4 = CS（软件 NSS）。
 * SPI1 时钟 72MHz / 16 = 4.5MHz，Mode 0，MSB first。
 */
#include "boot_w25q.h"
#include "boot_board.h"
#include "iap_layout.h"
#include "stm32f1xx.h"

#define W25_CMD_WRITE_ENABLE  0x06U
#define W25_CMD_READ_STATUS1  0x05U
#define W25_CMD_PAGE_PROGRAM  0x02U
#define W25_CMD_READ_DATA     0x03U
#define W25_CMD_SECTOR_ERASE  0x20U
#define W25_CMD_BLOCK_ERASE   0xD8U
#define W25_CMD_JEDEC_ID      0x9FU

#define W25_SR_BUSY           0x01U
#define W25_SR_WEL            0x02U

#define W25_PAGE_SIZE         256U
#define W25_SECTOR_SIZE       4096U
#define W25_BLOCK_SIZE        65536U

#define W25_TIMEOUT_SHORT_MS  50U
#define W25_TIMEOUT_ERASE_MS  3000U

static uint32_t s_id;
static int s_ready;

static void cs_low(void)
{
    GPIOA->BSRR = (1UL << 4) << 16;   /* PA4 = 0 */
}

static void cs_high(void)
{
    GPIOA->BSRR = (1UL << 4);         /* PA4 = 1 */
}

static uint8_t spi_xfer(uint8_t tx)
{
    uint32_t guard = 100000U;
    while ((SPI1->SR & SPI_SR_TXE) == 0U) {
        if (guard-- == 0U) return 0xFFU;
    }
    SPI1->DR = tx;
    while ((SPI1->SR & SPI_SR_RXNE) == 0U) {
        if (guard-- == 0U) return 0xFFU;
    }
    return (uint8_t)SPI1->DR;
}

static void spi_idle(void)
{
    while (SPI1->SR & SPI_SR_BSY) { }
}

static uint8_t read_status(void)
{
    uint8_t sr;
    cs_low();
    (void)spi_xfer(W25_CMD_READ_STATUS1);
    sr = spi_xfer(0xFFU);
    cs_high();
    spi_idle();
    return sr;
}

static int wait_busy(uint32_t timeout_ms)
{
    uint32_t cycles = timeout_ms * (boot_cpu_hz() / 1000UL);
    uint32_t start = DWT->CYCCNT;

    while (read_status() & W25_SR_BUSY) {
        boot_iwdg_feed();
        if ((DWT->CYCCNT - start) >= cycles) return 0;
    }
    return 1;
}

static int write_enable(void)
{
    cs_low();
    (void)spi_xfer(W25_CMD_WRITE_ENABLE);
    cs_high();
    spi_idle();
    return (read_status() & W25_SR_WEL) ? 1 : 0;
}

void boot_w25q_init(void)
{
    /* GPIOA + SPI1 时钟 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_SPI1EN;

    /* PA4 输出推挽 50MHz，默认 CS 高 */
    GPIOA->CRL = (GPIOA->CRL & ~(0xFUL << 16)) | (0x3UL << 16);
    GPIOA->BSRR = (1UL << 4);

    /* PA5/6/7 复用推挽 50MHz */
    GPIOA->CRL = (GPIOA->CRL & ~(0xFFFUL << 20)) | (0xBBBUL << 20);

    /* SPI1: master, software NSS, mode 0, /16, MSB first */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                (3UL << 3) | SPI_CR1_SPE;
    SPI1->CR2 = 0U;

    s_id = 0U;
    cs_low();
    (void)spi_xfer(W25_CMD_JEDEC_ID);
    uint8_t id0 = spi_xfer(0xFFU);
    uint8_t id1 = spi_xfer(0xFFU);
    uint8_t id2 = spi_xfer(0xFFU);
    cs_high();
    spi_idle();

    s_id = ((uint32_t)id0 << 16) | ((uint32_t)id1 << 8) | id2;
    s_ready = ((s_id & 0xFFFF00UL) == 0xEF4000UL) &&
              ((s_id & 0xFFU) >= 0x14U);
}

uint32_t boot_w25q_id(void)
{
    return s_id;
}

int boot_w25q_ready(void)
{
    return s_ready;
}

void boot_w25q_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!buf || !len || addr >= IAP_W25_BACKUP_ADDR + IAP_W25_BACKUP_SIZE) return;

    cs_low();
    (void)spi_xfer(W25_CMD_READ_DATA);
    (void)spi_xfer((uint8_t)(addr >> 16));
    (void)spi_xfer((uint8_t)(addr >> 8));
    (void)spi_xfer((uint8_t)addr);
    while (len--) {
        *buf++ = spi_xfer(0xFFU);
    }
    cs_high();
    spi_idle();
}

int boot_w25q_write_page(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (!buf || !len || len > W25_PAGE_SIZE) return 0;
    if ((addr % W25_PAGE_SIZE) + len > W25_PAGE_SIZE) return 0;

    if (!wait_busy(W25_TIMEOUT_SHORT_MS)) return 0;
    if (!write_enable()) return 0;

    cs_low();
    (void)spi_xfer(W25_CMD_PAGE_PROGRAM);
    (void)spi_xfer((uint8_t)(addr >> 16));
    (void)spi_xfer((uint8_t)(addr >> 8));
    (void)spi_xfer((uint8_t)addr);
    for (uint32_t i = 0; i < len; i++) {
        (void)spi_xfer(buf[i]);
    }
    cs_high();
    spi_idle();

    return wait_busy(W25_TIMEOUT_SHORT_MS);
}

int boot_w25q_erase_sector(uint32_t addr)
{
    addr &= ~(W25_SECTOR_SIZE - 1UL);

    if (!wait_busy(W25_TIMEOUT_SHORT_MS)) return 0;
    if (!write_enable()) return 0;

    cs_low();
    (void)spi_xfer(W25_CMD_SECTOR_ERASE);
    (void)spi_xfer((uint8_t)(addr >> 16));
    (void)spi_xfer((uint8_t)(addr >> 8));
    (void)spi_xfer((uint8_t)addr);
    cs_high();
    spi_idle();

    return wait_busy(W25_TIMEOUT_ERASE_MS);
}

int boot_w25q_erase_block(uint32_t addr)
{
    addr &= ~(W25_BLOCK_SIZE - 1UL);

    if (!wait_busy(W25_TIMEOUT_SHORT_MS)) return 0;
    if (!write_enable()) return 0;

    cs_low();
    (void)spi_xfer(W25_CMD_BLOCK_ERASE);
    (void)spi_xfer((uint8_t)(addr >> 16));
    (void)spi_xfer((uint8_t)(addr >> 8));
    (void)spi_xfer((uint8_t)addr);
    cs_high();
    spi_idle();

    return wait_busy(W25_TIMEOUT_ERASE_MS);
}
