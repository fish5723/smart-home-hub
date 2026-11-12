/**
 * @file boot_flash.c
 * @brief STM32F103 内部 Flash 编程（裸机 FPEC，例程全部放 .RamFunc）
 *
 * F1 在擦除/编程期间会阻塞对 Flash 的取指；把本文件的关键例程放进 RAM，
 * 避免在擦写 app 区时取指停顿导致时序不确定。
 */
#include "boot_flash.h"
#include "iap_layout.h"
#include "stm32f1xx.h"

#define FLASH_ERR_BITS (FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR)

__attribute__((section(".RamFunc")))
static int flash_wait_not_busy(void)
{
    uint32_t guard = 200000U;
    while (FLASH->SR & FLASH_SR_BSY) {
        if (guard-- == 0U) return 0;
    }
    return 1;
}

__attribute__((section(".RamFunc")))
static int flash_check_and_clear(void)
{
    uint32_t sr = FLASH->SR;
    int ok = (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) == 0U;
    FLASH->SR = FLASH_ERR_BITS;   /* rc_w1 */
    return ok;
}

__attribute__((section(".RamFunc")))
void boot_flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

void boot_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

__attribute__((section(".RamFunc")))
int boot_flash_erase_page(uint32_t addr)
{
    int ok;
    uint32_t primask;

    if (addr < IAP_APP_BASE || addr >= IAP_APP_END) return 0;
    if ((addr & (IAP_FLASH_PAGE_SIZE - 1UL)) != 0U) return 0;

    boot_flash_unlock();
    primask = __get_PRIMASK();
    __disable_irq();

    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = addr;
    FLASH->CR |= FLASH_CR_STRT;

    ok = flash_wait_not_busy();
    ok = ok && flash_check_and_clear();
    FLASH->CR &= ~FLASH_CR_PER;

    __set_PRIMASK(primask);
    return ok;
}

__attribute__((section(".RamFunc")))
int boot_flash_program_halfword(uint32_t addr, uint16_t value)
{
    int ok;
    uint32_t primask;

    if (addr < IAP_APP_BASE || addr >= IAP_APP_END) return 0;
    if ((addr & 1U) != 0U) return 0;

    boot_flash_unlock();
    primask = __get_PRIMASK();
    __disable_irq();

    FLASH->CR &= ~FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint16_t *)addr = value;
    ok = flash_wait_not_busy();
    ok = ok && flash_check_and_clear();
    FLASH->CR &= ~FLASH_CR_PG;

    __set_PRIMASK(primask);
    return ok;
}

__attribute__((section(".RamFunc")))
int boot_flash_program_bytes(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (!buf || !len) return 0;
    if (addr < IAP_APP_BASE || (addr + len) > IAP_APP_END) return -1;
    if ((addr & 1U) != 0U) return -1;

    for (i = 0; i < len; i += 2U) {
        uint16_t value = (uint16_t)buf[i];
        if ((i + 1U) < len) {
            value |= (uint16_t)((uint16_t)buf[i + 1U] << 8);
        } else {
            value |= 0xFF00U;
        }
        if (!boot_flash_program_halfword(addr + i, value)) return -1;
    }
    return (int)len;
}
