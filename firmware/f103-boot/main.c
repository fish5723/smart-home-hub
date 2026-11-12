/**
 * @file main.c
 * @brief F103 IAP bootloader — 元数据状态机 / 暂存搬运 / 备份回滚 / 跳转
 *
 * 流程：
 *   上电 → 读 W25Q 双副本元数据 → 按 state 处理：
 *     PENDING     校验暂存镜像与备份 → 擦 app → 搬运 → CRC 校验 → TESTING
 *     TESTING     启动次数 +1；超过 3 次未确认 → ROLLING_BACK
 *     ROLLING_BACK 从备份恢复旧 app（可重复执行）
 *     其它        校验 app 向量后跳转
 */
#include "iap_layout.h"
#include "boot_board.h"
#include "boot_w25q.h"
#include "boot_flash.h"
#include "boot_meta.h"
#include "crc32.h"
#include "stm32f1xx.h"
#include <stdint.h>
#include <string.h>

#define COPY_CHUNK 256U

static int vector_valid(const uint8_t *hdr)
{
    uint32_t sp    = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    uint32_t reset = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                     ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

    if (sp < IAP_RAM_BASE || sp > IAP_RAM_END || (sp & 0x3U) != 0U) return 0;
    if ((reset & 1U) == 0U) return 0;
    if (reset < IAP_APP_BASE || reset >= IAP_APP_END) return 0;
    return 1;
}

static int app_vector_valid(void)
{
    uint8_t hdr[8];
    const volatile uint32_t *p = (const volatile uint32_t *)IAP_APP_BASE;
    uint32_t sp = p[0];
    uint32_t reset = p[1];

    hdr[0] = (uint8_t)sp;         hdr[1] = (uint8_t)(sp >> 8);
    hdr[2] = (uint8_t)(sp >> 16); hdr[3] = (uint8_t)(sp >> 24);
    hdr[4] = (uint8_t)reset;      hdr[5] = (uint8_t)(reset >> 8);
    hdr[6] = (uint8_t)(reset >> 16); hdr[7] = (uint8_t)(reset >> 24);
    return vector_valid(hdr);
}

static uint32_t crc_w25(uint32_t addr, uint32_t len)
{
    uint8_t buf[COPY_CHUNK];
    uint32_t crc = 0;

#ifdef BOOT_TEST_BAD_STAGING
    /* 阶段 8 验收注入（CMake 选项 BOOT_TEST_BAD_STAGING，默认 OFF）：
     * 本次启动第一次 crc_w25 调用（即暂存区校验）时翻转首块 1 bit，
     * 模拟 W25Q 暂存数据损坏，用于验证“校验失败→拒绝搬运、保留旧 app”。
     * 备份校验发生在暂存校验之后，不受注入影响。 */
    static int injected;
#endif

    while (len) {
        uint32_t n = len > COPY_CHUNK ? COPY_CHUNK : len;
        boot_w25q_read(addr, buf, n);
#ifdef BOOT_TEST_BAD_STAGING
        if (!injected) {
            buf[0] ^= 0x01U;
            injected = 1;
        }
#endif
        crc = crc32_update(crc, buf, n);
        addr += n;
        len -= n;
        boot_iwdg_feed();
    }
    return crc;
}

static uint32_t crc_flash(uint32_t addr, uint32_t len)
{
    uint8_t buf[COPY_CHUNK];
    uint32_t crc = 0;

    while (len) {
        uint32_t n = len > COPY_CHUNK ? COPY_CHUNK : len;
        memcpy(buf, (const void *)addr, n);
        crc = crc32_update(crc, buf, n);
        addr += n;
        len -= n;
        boot_iwdg_feed();
    }
    return crc;
}

static int validate_staging(const iap_meta_t *m)
{
    uint8_t hdr[8];

    if (m->image_size < 8U || m->image_size > IAP_APP_SLOT_SIZE) {
        boot_log("IAP: staging size bad\r\n");
        return 0;
    }

    boot_w25q_read(IAP_W25_STAGING_ADDR, hdr, sizeof(hdr));
    if (!vector_valid(hdr)) {
        boot_log("IAP: staging vector bad\r\n");
        return 0;
    }

    uint32_t crc = crc_w25(IAP_W25_STAGING_ADDR, m->image_size);
    if (crc != m->image_crc32) {
        boot_log("IAP: staging crc mismatch\r\n");
        return 0;
    }

    boot_log("IAP: staging ok size=");
    boot_log_u32(m->image_size);
    boot_log("\r\n");
    return 1;
}

static int verify_backup(const iap_meta_t *m)
{
    if ((m->flags & IAP_FLAG_BACKUP_VALID) == 0U) {
        boot_log("IAP: backup not present\r\n");
        return 0;
    }

    uint32_t crc = crc_w25(IAP_W25_BACKUP_ADDR, IAP_APP_SLOT_SIZE);
    if (crc != m->backup_crc32) {
        boot_log("IAP: backup crc mismatch\r\n");
        return 0;
    }

    boot_log("IAP: backup ok\r\n");
    return 1;
}

static int copy_w25_to_flash(uint32_t src, uint32_t dst, uint32_t len)
{
    uint8_t buf[COPY_CHUNK];

    while (len) {
        uint32_t n = len > COPY_CHUNK ? COPY_CHUNK : len;
        boot_w25q_read(src, buf, n);
        if (boot_flash_program_bytes(dst, buf, n) != (int)n) {
            boot_log("IAP: flash write fail\r\n");
            return 0;
        }
        src += n;
        dst += n;
        len -= n;
        boot_iwdg_feed();
    }
    return 1;
}

static int erase_app_region(uint32_t image_size)
{
    uint32_t pages = (image_size + IAP_FLASH_PAGE_SIZE - 1U) / IAP_FLASH_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t addr = IAP_APP_BASE + i * IAP_FLASH_PAGE_SIZE;
        if (!boot_flash_erase_page(addr)) {
            boot_log("IAP: erase fail ");
            boot_log_hex(addr);
            boot_log("\r\n");
            return 0;
        }
        boot_iwdg_feed();
    }
    return 1;
}

static void jump_app(void);

static void rollback(iap_meta_t *m)
{
    boot_log("IAP: rolling back\r\n");

    m->state = IAP_STATE_ROLLING_BACK;
    (void)boot_meta_write(m);

    if (!verify_backup(m)) {
        boot_log("IAP: rollback aborted (backup invalid)\r\n");
        if (app_vector_valid()) jump_app();
        while (1) boot_iwdg_feed();
    }

    if (!erase_app_region(IAP_APP_SLOT_SIZE)) {
        while (1) boot_iwdg_feed();   /* 保持 ROLLING_BACK，重启后重试 */
    }

    if (!copy_w25_to_flash(IAP_W25_BACKUP_ADDR, IAP_APP_BASE, IAP_APP_SLOT_SIZE)) {
        while (1) boot_iwdg_feed();
    }

    uint32_t crc = crc_flash(IAP_APP_BASE, IAP_APP_SLOT_SIZE);
    if (crc != m->backup_crc32 || !app_vector_valid()) {
        boot_log("IAP: rollback verify fail\r\n");
        while (1) boot_iwdg_feed();
    }

    m->state = IAP_STATE_ROLLED_BACK;
    m->boot_attempts = 0U;
    (void)boot_meta_write(m);
    boot_log("IAP: rollback done\r\n");
    jump_app();
}

static void apply_update(iap_meta_t *m)
{
    boot_log("IAP: applying update\r\n");

    if (!validate_staging(m)) {
        m->state = IAP_STATE_ERROR;
        (void)boot_meta_write(m);
        boot_log("IAP: update rejected, keep old app\r\n");
        jump_app();
    }

    /* 先确认备份可用，再擦除唯一的 app 槽 */
    if (!verify_backup(m)) {
        m->state = IAP_STATE_ERROR;
        (void)boot_meta_write(m);
        boot_log("IAP: no valid backup, keep old app\r\n");
        jump_app();
    }

    if (!erase_app_region(m->image_size)) {
        rollback(m);
    }

    if (!copy_w25_to_flash(IAP_W25_STAGING_ADDR, IAP_APP_BASE, m->image_size)) {
        rollback(m);
    }

    uint32_t crc = crc_flash(IAP_APP_BASE, m->image_size);
    if (crc != m->image_crc32 || !app_vector_valid()) {
        boot_log("IAP: new app crc/vector fail\r\n");
        rollback(m);
    }

    m->state = IAP_STATE_TESTING;
    m->boot_attempts = 1U;
    if (!boot_meta_write(m)) {
        boot_log("IAP: testing meta write fail\r\n");
    }
    boot_log("IAP: update copied, boot new app\r\n");
    jump_app();
}

static void testing_state(iap_meta_t *m)
{
    if (!app_vector_valid()) {
        boot_log("IAP: testing app invalid\r\n");
        rollback(m);
    }

    m->boot_attempts++;
    if (m->boot_attempts > IAP_MAX_BOOT_ATTEMPTS) {
        boot_log("IAP: app not confirmed, attempts=");
        boot_log_u32(m->boot_attempts);
        boot_log("\r\n");
        rollback(m);
    }

    if (!boot_meta_write(m)) {
        boot_log("IAP: testing meta write fail\r\n");
    }
    boot_log("IAP: testing boot ");
    boot_log_u32(m->boot_attempts);
    boot_log("\r\n");
    jump_app();
}

static void jump_app(void)
{
    uint32_t sp, reset;

    if (!app_vector_valid()) {
        boot_log("IAP: app vector invalid, halt\r\n");
        while (1) boot_iwdg_feed();
    }

    sp    = *(volatile uint32_t *)IAP_APP_BASE;
    reset = *(volatile uint32_t *)(IAP_APP_BASE + 4U);

    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = IAP_APP_BASE;
    __DSB();
    __ISB();
    __set_PRIMASK(0);

    __set_MSP(sp);
    ((void (*)(void))reset)();
}

int main(void)
{
    iap_meta_t meta;
    int has_meta;

    boot_board_init();
    boot_log("\r\nIAP boot\r\n");

    boot_w25q_init();
    boot_log("IAP: W25Q id=");
    boot_log_hex(boot_w25q_id());
    boot_log(boot_w25q_ready() ? " ok\r\n" : " bad\r\n");

    has_meta = boot_meta_load(&meta);
    if (!has_meta) {
        boot_log("IAP: no meta, boot app\r\n");
        jump_app();
    }

    boot_log("IAP: meta state=");
    boot_log_u32(meta.state);
    boot_log(" seq=");
    boot_log_u32(meta.seq);
    boot_log("\r\n");

    if (!boot_w25q_ready() && meta.state != IAP_STATE_IDLE) {
        boot_log("IAP: storage missing, skip update\r\n");
        jump_app();
    }

    switch (meta.state) {
    case IAP_STATE_PENDING:
        apply_update(&meta);
        break;
    case IAP_STATE_TESTING:
        testing_state(&meta);
        break;
    case IAP_STATE_ROLLING_BACK:
        rollback(&meta);
        break;
    case IAP_STATE_IDLE:
    case IAP_STATE_CONFIRMED:
    case IAP_STATE_ROLLED_BACK:
    case IAP_STATE_ERROR:
    default:
        break;
    }

    jump_app();
    while (1) boot_iwdg_feed();
}
