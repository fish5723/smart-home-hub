/**
 * @file iap_layout.h
 * @brief F103 IAP 内部 Flash / W25Q64 布局与元数据定义（app 与 bootloader 共用）
 *
 * 内部 Flash：
 *   0x08000000-0x08001FFF  bootloader (8KB)
 *   0x08002000-0x0800FFFF  app        (56KB)
 *
 * W25Q64（实测 1MB，沿用前 192KB）：
 *   0x000000-0x000FFF 元数据副本 A
 *   0x001000-0x001FFF 元数据副本 B
 *   0x010000-0x01FFFF 新固件暂存区
 *   0x020000-0x02FFFF 旧 app 全槽备份
 */
#ifndef IAP_LAYOUT_H
#define IAP_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_APP_BASE            0x08002000UL
#define IAP_APP_END             0x08010000UL
#define IAP_APP_SLOT_SIZE       (IAP_APP_END - IAP_APP_BASE)   /* 56KB */
#define IAP_FLASH_PAGE_SIZE     1024UL
#define IAP_FLASH_PAGE_COUNT    (IAP_APP_SLOT_SIZE / IAP_FLASH_PAGE_SIZE)

#define IAP_RAM_BASE            0x20000000UL
#define IAP_RAM_END             0x20005000UL

#define IAP_W25_META_A_ADDR     0x000000UL
#define IAP_W25_META_B_ADDR     0x001000UL
#define IAP_W25_META_SECTOR     0x1000UL
#define IAP_W25_STAGING_ADDR    0x010000UL
#define IAP_W25_STAGING_SIZE    0x10000UL
#define IAP_W25_BACKUP_ADDR     0x020000UL
#define IAP_W25_BACKUP_SIZE     0x10000UL

#define IAP_META_MAGIC          0x49415031UL   /* "IAP1" */
#define IAP_META_VERSION        1U
#define IAP_MAX_BOOT_ATTEMPTS   3U

typedef enum {
    IAP_STATE_IDLE         = 0,
    IAP_STATE_PENDING      = 1,   /* 暂存镜像已完整，等 bootloader 搬运 */
    IAP_STATE_TESTING      = 2,   /* 新 app 已写入，等待启动确认 */
    IAP_STATE_CONFIRMED    = 3,   /* 新 app 已确认 */
    IAP_STATE_ROLLING_BACK = 4,   /* 正在从备份恢复旧 app，可重复执行 */
    IAP_STATE_ROLLED_BACK  = 5,   /* 已恢复旧 app */
    IAP_STATE_ERROR        = 6,   /* 更新失败，保留旧 app */
} iap_state_t;

#define IAP_FLAG_BACKUP_VALID   0x01U

/* 28 bytes；meta_crc16 之前 24 bytes 参与 CRC16。 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  boot_attempts;
    uint8_t  reserved0;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t backup_crc32;
    uint16_t meta_crc16;
    uint16_t reserved1;
} iap_meta_t;

#define IAP_META_CRC_LEN        offsetof(iap_meta_t, meta_crc16)

#ifdef __cplusplus
}
#endif

#endif /* IAP_LAYOUT_H */
