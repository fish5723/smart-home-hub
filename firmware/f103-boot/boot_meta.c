/**
 * @file boot_meta.c
 * @brief IAP 元数据双副本（A/B 交替写 + seq + CRC16）
 */
#include "boot_meta.h"
#include "boot_w25q.h"
#include "boot_board.h"
#include "crc16.h"
#include <string.h>

static int meta_valid(const iap_meta_t *m)
{
    if (m->magic != IAP_META_MAGIC) return 0;
    uint16_t crc = crc16_compute((const uint8_t *)m, (uint16_t)IAP_META_CRC_LEN);
    return crc == m->meta_crc16;
}

static void meta_read(uint32_t addr, iap_meta_t *out)
{
    boot_w25q_read(addr, (uint8_t *)out, sizeof(*out));
}

int boot_meta_load(iap_meta_t *out)
{
    iap_meta_t a, b;
    int va, vb;

    meta_read(IAP_W25_META_A_ADDR, &a);
    meta_read(IAP_W25_META_B_ADDR, &b);

    va = meta_valid(&a);
    vb = meta_valid(&b);

    if (va && (!vb || a.seq >= b.seq)) {
        *out = a;
        return 1;
    }
    if (vb) {
        *out = b;
        return 1;
    }
    return 0;
}

int boot_meta_write(const iap_meta_t *meta)
{
    iap_meta_t cur, next;
    uint32_t target;
    int has_cur = boot_meta_load(&cur);

    memset(&next, 0, sizeof(next));
    next.magic         = IAP_META_MAGIC;
    next.seq           = has_cur ? (cur.seq + 1U) : 1U;
    next.state         = meta->state;
    next.flags         = meta->flags;
    next.boot_attempts = meta->boot_attempts;
    next.image_size    = meta->image_size;
    next.image_crc32   = meta->image_crc32;
    next.backup_crc32  = meta->backup_crc32;
    next.meta_crc16    = crc16_compute((const uint8_t *)&next,
                                       (uint16_t)IAP_META_CRC_LEN);

    /* seq 奇偶决定副本位置：奇→A，偶→B。相邻两次写必落在不同扇区。 */
    target = (next.seq & 1U) ? IAP_W25_META_A_ADDR : IAP_W25_META_B_ADDR;

    if (!boot_w25q_erase_sector(target)) {
        boot_log("IAP: meta erase fail\r\n");
        return 0;
    }
    if (!boot_w25q_write_page(target, (const uint8_t *)&next, sizeof(next))) {
        boot_log("IAP: meta write fail\r\n");
        return 0;
    }

    iap_meta_t check;
    meta_read(target, &check);
    if (!meta_valid(&check) || check.seq != next.seq) {
        boot_log("IAP: meta verify fail\r\n");
        return 0;
    }
    return 1;
}
