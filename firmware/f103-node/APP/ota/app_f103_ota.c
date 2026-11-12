/**
 * @file app_f103_ota.c
 * @brief F103 IAP 接收端 — 暂存、校验、备份、元数据、重启
 *
 * 流程（P4 主动）：
 *   OTA_BEGIN → 擦暂存块
 *   OTA_DATA  → 128B/块写 W25Q64，累加 CRC32
 *   OTA_END   → 回读校验 → 备份当前 app 全 56KB → 写 PENDING → 重启
 *   OTA_ABORT → 丢弃会话（不写元数据）
 *
 * 断电语义：只有 OTA_END 全部成功后才写 PENDING；之前任何失败/断电，
 * bootloader 都会继续启动旧 app。
 */
#include "app_f103_ota.h"
#include "iap_layout.h"
#include "crc32.h"
#include "crc16.h"
#include "bsp_w25q64.h"
#include "bsp_iwdg.h"
#include "proto_uart.h"
#include "proto_tx.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* 实测本板 JEDEC ID = 0xEF4014（Winbond W25Q80，1MB），与文档标注的
 * W25Q64（0xEF4017）不符，但 1MB 对本 IAP 布局（前 192KB）完全够用。
 * 同时接受 W25Q16/32/64/128，避免换料后再次误判。 */
#define W25Q_JEDEC_MANUF_TYPE(id)  ((id) & 0xFFFF00UL)
#define W25Q_JEDEC_MANUF_TYPE_WB   0xEF4000UL
#define W25Q_JEDEC_MIN_CAPACITY    0x14U   /* 0x14 = 1MB */

#define OTA_RX_TIMEOUT_MS      30000U
#define OTA_CONFIRM_DELAY_MS   10000U

typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
    OTA_VERIFYING,
} ota_state_t;

/* volatile + .noinit：bring-up 时可被 ST-Link 在复位后直接读取（复位不清 RAM），
 * 也不被启动代码清零。首次上电可能是随机值，写入后即为真实 JEDEC ID。 */
__attribute__((section(".noinit")))
volatile uint32_t g_f103_ota_storage_id;

static int s_storage_ready;

static ota_state_t s_state;
static uint16_t s_session;
static uint32_t s_image_size;
static uint32_t s_image_crc_expected;
static uint32_t s_received;
static uint32_t s_crc;
static uint32_t s_last_rx_ms;

static uint32_t s_confirm_at_ms;
static int      s_confirm_done;

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void send_result(uint16_t acked_seq, uint8_t status,
                        uint8_t stage, uint32_t value)
{
    uint8_t p[8];
    p[0] = (uint8_t)(acked_seq & 0xFFU);
    p[1] = (uint8_t)((acked_seq >> 8) & 0xFFU);
    p[2] = status;
    p[3] = stage;
    p[4] = (uint8_t)(value & 0xFFU);
    p[5] = (uint8_t)((value >> 8) & 0xFFU);
    p[6] = (uint8_t)((value >> 16) & 0xFFU);
    p[7] = (uint8_t)((value >> 24) & 0xFFU);
    ProtoUart_SendFrame(PROTO_OTA_RESULT, ProtoTx_NextSeq(), p, sizeof(p));
}

static int meta_valid(const iap_meta_t *m)
{
    if (m->magic != IAP_META_MAGIC) return 0;
    return crc16_compute((const uint8_t *)m, (uint16_t)IAP_META_CRC_LEN) ==
           m->meta_crc16;
}

static int meta_load(iap_meta_t *out)
{
    iap_meta_t a, b;
    int va, vb;

    BSP_W25Q64_Read(IAP_W25_META_A_ADDR, (uint8_t *)&a, sizeof(a));
    BSP_W25Q64_Read(IAP_W25_META_B_ADDR, (uint8_t *)&b, sizeof(b));

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

static int meta_write(const iap_meta_t *meta)
{
    iap_meta_t cur, next;
    uint32_t target;
    int has_cur = meta_load(&cur);

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

    target = (next.seq & 1U) ? IAP_W25_META_A_ADDR : IAP_W25_META_B_ADDR;

    if (!BSP_W25Q64_EraseSector(target)) return 0;
    if (!BSP_W25Q64_Write(target, (uint8_t *)&next, sizeof(next))) return 0;

    iap_meta_t check;
    BSP_W25Q64_Read(target, (uint8_t *)&check, sizeof(check));
    return meta_valid(&check) && check.seq == next.seq;
}

static uint32_t crc_w25_region(uint32_t addr, uint32_t len)
{
    uint8_t buf[256];
    uint32_t crc = 0;

    while (len) {
        uint32_t n = len > sizeof(buf) ? (uint32_t)sizeof(buf) : len;
        BSP_W25Q64_Read(addr, buf, (uint16_t)n);
        crc = crc32_update(crc, buf, n);
        addr += n;
        len -= n;
        BSP_IWDG_Feed();
    }
    return crc;
}

static int backup_app_slot(uint32_t *out_crc)
{
    uint8_t buf[256];
    uint32_t crc = 0;

    if (!BSP_W25Q64_EraseBlock(IAP_W25_BACKUP_ADDR)) return 0;

    for (uint32_t off = 0; off < IAP_APP_SLOT_SIZE; off += sizeof(buf)) {
        memcpy(buf, (const void *)(IAP_APP_BASE + off), sizeof(buf));
        crc = crc32_update(crc, buf, sizeof(buf));
        if (!BSP_W25Q64_Write(IAP_W25_BACKUP_ADDR + off, buf, sizeof(buf))) {
            return 0;
        }
        BSP_IWDG_Feed();
    }

    /* 回读校验：确认备份真的可用于回滚 */
    if (crc_w25_region(IAP_W25_BACKUP_ADDR, IAP_APP_SLOT_SIZE) != crc) return 0;

    *out_crc = crc;
    return 1;
}

void AppF103Ota_Init(void)
{
    BSP_IWDG_Feed();
    BSP_W25Q64_Init();

    g_f103_ota_storage_id = BSP_W25Q64_ReadID();
    s_storage_ready = (W25Q_JEDEC_MANUF_TYPE(g_f103_ota_storage_id) ==
                       W25Q_JEDEC_MANUF_TYPE_WB) &&
                      ((g_f103_ota_storage_id & 0xFFU) >= W25Q_JEDEC_MIN_CAPACITY);

    printf("[IAP] W25Q64 JEDEC ID = 0x%06lX  %s\r\n",
           (unsigned long)g_f103_ota_storage_id,
           s_storage_ready ? "OK" : "UNSUPPORTED");
    BSP_IWDG_Feed();

    s_state = OTA_IDLE;
    s_confirm_at_ms = HAL_GetTick() + OTA_CONFIRM_DELAY_MS;
    s_confirm_done = 0;
}

uint32_t AppF103Ota_GetStorageId(void)
{
    return g_f103_ota_storage_id;
}

int AppF103Ota_StorageReady(void)
{
    return s_storage_ready;
}

int AppF103Ota_IsActive(void)
{
    return s_state != OTA_IDLE;
}

static int ota_begin(const proto_frame_t *frame)
{
    if (frame->len < 12U) {
        send_result(frame->seq, PROTO_OTA_ERR_PARAM, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }

    uint32_t size    = rd_u32(&frame->payload[0]);
    uint32_t crc     = rd_u32(&frame->payload[4]);
    uint16_t chunk   = rd_u16(&frame->payload[8]);
    uint16_t session = rd_u16(&frame->payload[10]);

    /* P4 的 BEGIN ACK 若丢失会原样重传：同一 session 已进入接收态时直接重 ACK，
     * 不能回 BUSY，否则一次丢包就会让整次升级失败。 */
    if (s_state == OTA_RECEIVING && session == s_session) {
        send_result(frame->seq, PROTO_OTA_OK, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }
    if (s_state != OTA_IDLE) {
        send_result(frame->seq, PROTO_OTA_ERR_BUSY, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }
    if (!s_storage_ready) {
        send_result(frame->seq, PROTO_OTA_ERR_STORAGE, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }
    if (size < 8U || size > IAP_APP_SLOT_SIZE) {
        send_result(frame->seq, PROTO_OTA_ERR_SIZE, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }
    if (chunk != PROTO_OTA_CHUNK_SIZE || session == 0U) {
        send_result(frame->seq, PROTO_OTA_ERR_PARAM, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }

    if (!BSP_W25Q64_EraseBlock(IAP_W25_STAGING_ADDR)) {
        send_result(frame->seq, PROTO_OTA_ERR_STORAGE, PROTO_OTA_STAGE_BEGIN, 0);
        return 1;
    }

    s_session        = session;
    s_image_size     = size;
    s_image_crc_expected = crc;
    s_received       = 0;
    s_crc            = 0;
    s_last_rx_ms     = HAL_GetTick();
    s_state          = OTA_RECEIVING;

    printf("[IAP] BEGIN size=%lu crc=0x%08lX session=%u\r\n",
           (unsigned long)size, (unsigned long)crc, (unsigned)session);
    send_result(frame->seq, PROTO_OTA_OK, PROTO_OTA_STAGE_BEGIN, 0);
    return 1;
}

static int ota_data(const proto_frame_t *frame)
{
    if (frame->len < 6U) {
        send_result(frame->seq, PROTO_OTA_ERR_PARAM, PROTO_OTA_STAGE_DATA, 0);
        return 1;
    }

    uint16_t session = rd_u16(&frame->payload[0]);
    uint32_t offset  = rd_u32(&frame->payload[2]);
    const uint8_t *data = &frame->payload[6];
    uint32_t len = (uint32_t)frame->len - 6U;

    if (s_state != OTA_RECEIVING || session != s_session) {
        send_result(frame->seq, PROTO_OTA_ERR_STATE, PROTO_OTA_STAGE_DATA, 0);
        return 1;
    }
    if (len == 0U || len > PROTO_OTA_CHUNK_SIZE ||
        offset > s_image_size || len > (s_image_size - offset)) {
        send_result(frame->seq, PROTO_OTA_ERR_PARAM, PROTO_OTA_STAGE_DATA, 0);
        return 1;
    }

    if (offset == s_received) {
        if (!BSP_W25Q64_Write(IAP_W25_STAGING_ADDR + offset,
                              (uint8_t *)data, (uint16_t)len)) {
            send_result(frame->seq, PROTO_OTA_ERR_STORAGE,
                        PROTO_OTA_STAGE_DATA, s_received);
            return 1;
        }
        s_crc = crc32_update(s_crc, data, len);
        s_received += len;
    } else if (offset + len > s_received) {
        /* 既不是下一个块，也不是已收到的重复块 */
        send_result(frame->seq, PROTO_OTA_ERR_STATE, PROTO_OTA_STAGE_DATA,
                    s_received);
        return 1;
    }
    /* else：重复帧（重传），已经写过，直接重新 ACK */

    s_last_rx_ms = HAL_GetTick();
    BSP_IWDG_Feed();
    send_result(frame->seq, PROTO_OTA_OK, PROTO_OTA_STAGE_DATA, s_received);
    return 1;
}

static int ota_end(const proto_frame_t *frame)
{
    if (frame->len < 8U) {
        send_result(frame->seq, PROTO_OTA_ERR_PARAM, PROTO_OTA_STAGE_END, 0);
        return 1;
    }

    uint16_t session = rd_u16(&frame->payload[0]);
    uint32_t crc     = rd_u32(&frame->payload[4]);

    if (s_state != OTA_RECEIVING || session != s_session) {
        send_result(frame->seq, PROTO_OTA_ERR_STATE, PROTO_OTA_STAGE_END, 0);
        return 1;
    }
    if (s_received != s_image_size) {
        send_result(frame->seq, PROTO_OTA_ERR_SIZE, PROTO_OTA_STAGE_END,
                    s_received);
        return 1;
    }
    if (s_crc != s_image_crc_expected || crc != s_image_crc_expected) {
        s_state = OTA_IDLE;
        send_result(frame->seq, PROTO_OTA_ERR_CRC, PROTO_OTA_STAGE_END, 0);
        return 1;
    }

    s_state = OTA_VERIFYING;

    /* 回读暂存区，确认 W25Q64 写入真的正确 */
    if (crc_w25_region(IAP_W25_STAGING_ADDR, s_image_size) !=
        s_image_crc_expected) {
        s_state = OTA_IDLE;
        printf("[IAP] staging verify failed\r\n");
        send_result(frame->seq, PROTO_OTA_ERR_VERIFY, PROTO_OTA_STAGE_END, 0);
        return 1;
    }

    printf("[IAP] staging verified, backing up app...\r\n");
    uint32_t backup_crc = 0;
    if (!backup_app_slot(&backup_crc)) {
        s_state = OTA_IDLE;
        printf("[IAP] backup failed\r\n");
        send_result(frame->seq, PROTO_OTA_ERR_STORAGE, PROTO_OTA_STAGE_END, 0);
        return 1;
    }

    iap_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.state         = IAP_STATE_PENDING;
    meta.flags         = IAP_FLAG_BACKUP_VALID;
    meta.boot_attempts = 0;
    meta.image_size    = s_image_size;
    meta.image_crc32   = s_image_crc_expected;
    meta.backup_crc32  = backup_crc;

    if (!meta_write(&meta)) {
        s_state = OTA_IDLE;
        printf("[IAP] meta write failed\r\n");
        send_result(frame->seq, PROTO_OTA_ERR_STORAGE, PROTO_OTA_STAGE_END, 0);
        return 1;
    }

    printf("[IAP] PENDING written, resetting to bootloader\r\n");
    send_result(frame->seq, PROTO_OTA_OK, PROTO_OTA_STAGE_END, s_image_size);
    BSP_IWDG_Feed();
    HAL_Delay(100);
    NVIC_SystemReset();
    return 1;
}

static int ota_abort(const proto_frame_t *frame)
{
    if (frame->len >= 2U) {
        uint16_t session = rd_u16(&frame->payload[0]);
        if (s_state != OTA_IDLE && session != s_session) {
            send_result(frame->seq, PROTO_OTA_ERR_STATE,
                        PROTO_OTA_STAGE_ABORT, 0);
            return 1;
        }
    }
    if (s_state != OTA_IDLE) {
        s_state = OTA_IDLE;
        printf("[IAP] aborted\r\n");
    }
    send_result(frame->seq, PROTO_OTA_OK, PROTO_OTA_STAGE_ABORT, 0);
    return 1;
}

int AppF103Ota_HandleFrame(const proto_frame_t *frame)
{
    switch (frame->type) {
    case PROTO_OTA_BEGIN: return ota_begin(frame);
    case PROTO_OTA_DATA:  return ota_data(frame);
    case PROTO_OTA_END:   return ota_end(frame);
    case PROTO_OTA_ABORT: return ota_abort(frame);
    default:              return 0;
    }
}

void AppF103Ota_Poll(void)
{
    uint32_t now = HAL_GetTick();

    if (s_state == OTA_RECEIVING && (now - s_last_rx_ms) > OTA_RX_TIMEOUT_MS) {
        s_state = OTA_IDLE;
        printf("[IAP] RX timeout, session dropped\r\n");
    }

    if (s_confirm_done || (int32_t)(now - s_confirm_at_ms) < 0) return;

    iap_meta_t meta;
    if (!meta_load(&meta)) {
        s_confirm_done = 1;
        return;
    }

    if (meta.state != IAP_STATE_TESTING) {
        s_confirm_done = 1;
        return;
    }

    meta.state = IAP_STATE_CONFIRMED;
    meta.boot_attempts = 0;
    if (meta_write(&meta)) {
        printf("[IAP] new app CONFIRMED\r\n");
        s_confirm_done = 1;
    } else {
        printf("[IAP] CONFIRMED write failed, retry\r\n");
        s_confirm_at_ms = now + 5000U;
    }
}
