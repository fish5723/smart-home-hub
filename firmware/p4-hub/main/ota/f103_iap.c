/**
 * @file f103_iap.c
 * @brief P4 → F103 IAP 发送器（阶段 8）
 *
 * 关键点：
 *   - 不复用 ProtoTx 的 100ms 单可靠槽；OTA 自己管理 seq/超时/重传，
 *     避免 400+ 块连续发送把 UI 执行器命令挤掉。
 *   - 每块 DATA 等 F103 的 OTA_RESULT（payload 前 2 字节 = acked_seq）。
 *   - 串口日志只在开始/每 10%/结束打印，避免升级期间刷屏拖慢节奏。
 */
#include "f103_iap.h"

#include "sdkconfig.h"
#include "app_proto.h"
#include "proto_uart.h"
#include "proto_tx.h"
#include "sd_card.h"
#include "watchdog.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_random.h"
#include "esp_task_wdt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "F103_IAP";

#ifndef CONFIG_SMART_HOME_F103_IAP_PATH
#define CONFIG_SMART_HOME_F103_IAP_PATH "/sdcard/ota/f103-node.bin"
#endif

#define F103_APP_BASE       0x08002000U
#define F103_APP_END        0x08010000U
#define F103_APP_SLOT_SIZE  0x0000E000U
#define F103_RAM_BASE       0x20000000U
#define F103_RAM_END        0x20005000U

#define OTA_BEGIN_TIMEOUT_MS  5000U
#define OTA_DATA_TIMEOUT_MS   800U
#define OTA_END_TIMEOUT_MS    8000U
#define OTA_RETRIES           3U

typedef struct {
    uint16_t acked_seq;
    uint8_t  status;
    uint8_t  stage;
    uint32_t value;
} ota_result_evt_t;

static QueueHandle_t s_result_q;
static volatile int s_running;
static volatile int s_progress;
static volatile f103_iap_status_t s_status = F103_IAP_IDLE;

static uint16_t s_session;
static uint16_t s_ota_seq;

static uint16_t ota_next_seq(void)
{
    s_ota_seq++;
    if (s_ota_seq == 0U) s_ota_seq = 1U;
    return s_ota_seq;
}

static void set_status(f103_iap_status_t st)
{
    s_status = st;
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void f103_iap_on_result(const proto_frame_t *frame)
{
    if (!s_result_q || frame->len < 8U) return;

    ota_result_evt_t evt;
    evt.acked_seq = rd_u16(&frame->payload[0]);
    evt.status    = frame->payload[2];
    evt.stage     = frame->payload[3];
    evt.value     = rd_u32(&frame->payload[4]);
    (void)xQueueSend(s_result_q, &evt, 0);
}

static int wait_result(uint16_t seq, ota_result_evt_t *out, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        ota_result_evt_t evt;
        if (xQueueReceive(s_result_q, &evt, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (evt.acked_seq == seq) {
                *out = evt;
                return 0;
            }
            /* 上一次重传的迟到 ACK：丢弃，继续等当前 seq */
        }
        wdt_feed();
    }
    return -1;
}

static int send_request(uint8_t type, const uint8_t *payload, uint16_t len,
                        uint32_t timeout_ms, ota_result_evt_t *out)
{
    /* 用 OTA 私有序号，避免与协议栈的 PING/执行器帧共用计数器；
     * F103 对 OTA 帧不做普通去重，重传同 seq 会重新 ACK。 */
    uint16_t seq = ota_next_seq();

    for (uint32_t attempt = 0; attempt < OTA_RETRIES; attempt++) {
        ProtoUart_SendFrame(type, seq, payload, len);
        if (wait_result(seq, out, timeout_ms) == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "timeout type=0x%02X attempt=%u",
                 (unsigned)type, (unsigned)(attempt + 1));
        wdt_feed();
    }
    return -1;
}

static void send_abort(void)
{
    uint8_t p[3];
    p[0] = (uint8_t)(s_session & 0xFFU);
    p[1] = (uint8_t)((s_session >> 8) & 0xFFU);
    p[2] = 0U;
    ProtoUart_SendFrame(PROTO_OTA_ABORT, ota_next_seq(), p, sizeof(p));
}

static int check_vector(const uint8_t *image, uint32_t size)
{
    if (size < 8U || size > F103_APP_SLOT_SIZE) {
        ESP_LOGE(TAG, "image size %u out of range", (unsigned)size);
        return -1;
    }

    uint32_t sp = rd_u32(&image[0]);
    uint32_t reset = rd_u32(&image[4]);

    if (sp < F103_RAM_BASE || sp > F103_RAM_END || (sp & 0x3U)) {
        ESP_LOGE(TAG, "bad initial SP 0x%08X", (unsigned)sp);
        return -1;
    }
    if ((reset & 1U) == 0U || reset < F103_APP_BASE || reset >= F103_APP_END) {
        ESP_LOGE(TAG, "bad reset handler 0x%08X", (unsigned)reset);
        return -1;
    }
    if (image[0] == 0xE9U) {
        ESP_LOGE(TAG, "looks like an ESP image, not F103 app");
        return -1;
    }
    return 0;
}

static int run_update(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open %s failed", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size_l = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size_l <= 0 || (uint32_t)size_l > F103_APP_SLOT_SIZE) {
        ESP_LOGE(TAG, "image size %ld invalid", size_l);
        fclose(fp);
        return -1;
    }
    uint32_t size = (uint32_t)size_l;

    uint8_t *image = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) {
        image = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!image) {
        ESP_LOGE(TAG, "no memory for %u-byte image", (unsigned)size);
        fclose(fp);
        return -1;
    }

    if (fread(image, 1, size, fp) != size) {
        ESP_LOGE(TAG, "read image failed");
        free(image);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (check_vector(image, size) != 0) {
        free(image);
        return -1;
    }

    uint32_t crc = esp_rom_crc32_le(0, image, size);
    ESP_LOGI(TAG, "image=%s size=%u crc32=0x%08X",
             path, (unsigned)size, (unsigned)crc);

    s_session = (uint16_t)(esp_random() & 0xFFFFU);
    if (s_session == 0U) s_session = 1U;

    ota_result_evt_t r = {0};
    uint8_t begin[12];
    begin[0] = (uint8_t)(size & 0xFFU);
    begin[1] = (uint8_t)((size >> 8) & 0xFFU);
    begin[2] = (uint8_t)((size >> 16) & 0xFFU);
    begin[3] = (uint8_t)((size >> 24) & 0xFFU);
    begin[4] = (uint8_t)(crc & 0xFFU);
    begin[5] = (uint8_t)((crc >> 8) & 0xFFU);
    begin[6] = (uint8_t)((crc >> 16) & 0xFFU);
    begin[7] = (uint8_t)((crc >> 24) & 0xFFU);
    begin[8] = (uint8_t)PROTO_OTA_CHUNK_SIZE;
    begin[9] = 0U;
    begin[10] = (uint8_t)(s_session & 0xFFU);
    begin[11] = (uint8_t)((s_session >> 8) & 0xFFU);

    if (send_request(PROTO_OTA_BEGIN, begin, sizeof(begin),
                     OTA_BEGIN_TIMEOUT_MS, &r) != 0 ||
        r.status != PROTO_OTA_OK) {
        ESP_LOGE(TAG, "BEGIN rejected status=%u", (unsigned)r.status);
        send_abort();
        free(image);
        return -1;
    }

    int last_logged = -1;
    for (uint32_t off = 0; off < size; off += PROTO_OTA_CHUNK_SIZE) {
        uint16_t chunk = (uint16_t)((size - off > PROTO_OTA_CHUNK_SIZE)
                                    ? PROTO_OTA_CHUNK_SIZE : (size - off));
        uint8_t data[6 + PROTO_OTA_CHUNK_SIZE];
        data[0] = (uint8_t)(s_session & 0xFFU);
        data[1] = (uint8_t)((s_session >> 8) & 0xFFU);
        data[2] = (uint8_t)(off & 0xFFU);
        data[3] = (uint8_t)((off >> 8) & 0xFFU);
        data[4] = (uint8_t)((off >> 16) & 0xFFU);
        data[5] = (uint8_t)((off >> 24) & 0xFFU);
        memcpy(&data[6], &image[off], chunk);

        if (send_request(PROTO_OTA_DATA, data, (uint16_t)(6U + chunk),
                         OTA_DATA_TIMEOUT_MS, &r) != 0 ||
            r.status != PROTO_OTA_OK) {
            ESP_LOGE(TAG, "DATA offset=%u failed status=%u",
                     (unsigned)off, (unsigned)r.status);
            send_abort();
            free(image);
            return -1;
        }

        uint32_t sent = off + chunk;
        s_progress = (int)((sent * 100U) / size);
        int pct = s_progress / 10;
        if (pct != last_logged) {
            last_logged = pct;
            ESP_LOGI(TAG, "progress %d%% (%u/%u)",
                     s_progress, (unsigned)sent, (unsigned)size);
        }
        wdt_feed();
    }

    uint8_t end[8];
    end[0] = (uint8_t)(s_session & 0xFFU);
    end[1] = (uint8_t)((s_session >> 8) & 0xFFU);
    end[2] = 0U;
    end[3] = 0U;
    end[4] = (uint8_t)(crc & 0xFFU);
    end[5] = (uint8_t)((crc >> 8) & 0xFFU);
    end[6] = (uint8_t)((crc >> 16) & 0xFFU);
    end[7] = (uint8_t)((crc >> 24) & 0xFFU);

    ESP_LOGI(TAG, "sending END (verify + backup + reset on F103)...");
    if (send_request(PROTO_OTA_END, end, sizeof(end),
                     OTA_END_TIMEOUT_MS, &r) != 0 ||
        r.status != PROTO_OTA_OK) {
        ESP_LOGE(TAG, "END failed status=%u", (unsigned)r.status);
        send_abort();
        free(image);
        return -1;
    }

    free(image);
    ESP_LOGI(TAG, "F103 update accepted; device is rebooting");
    return 0;
}

static void f103_iap_task(void *arg)
{
    (void)arg;
    wdt_add_current_task();

    s_running = 1;
    s_progress = 0;
    set_status(F103_IAP_RUNNING);

    if (!sd_card_is_mounted()) {
        ESP_LOGE(TAG, "SD not mounted");
        set_status(F103_IAP_FAILED);
        goto done;
    }
    if (AppProto_GetLinkState() != LINK_UP) {
        ESP_LOGE(TAG, "F103 link not UP");
        set_status(F103_IAP_FAILED);
        goto done;
    }

    if (run_update(CONFIG_SMART_HOME_F103_IAP_PATH) != 0) {
        set_status(F103_IAP_FAILED);
        goto done;
    }

    s_progress = 100;
    set_status(F103_IAP_SUCCESS);

done:
    s_running = 0;
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

esp_err_t f103_iap_init(void)
{
    if (!s_result_q) {
        s_result_q = xQueueCreate(16, sizeof(ota_result_evt_t));
        if (!s_result_q) return ESP_ERR_NO_MEM;
    }
    s_status = F103_IAP_IDLE;
    s_progress = 0;
    s_running = 0;
    return ESP_OK;
}

esp_err_t f103_iap_start(void)
{
    if (!s_result_q) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (AppProto_GetLinkState() != LINK_UP) return ESP_ERR_INVALID_STATE;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    /* 清掉上一次残留的迟到 ACK */
    ota_result_evt_t evt;
    while (xQueueReceive(s_result_q, &evt, 0) == pdTRUE) { }

    s_status = F103_IAP_RUNNING;
    s_progress = 0;
    if (xTaskCreate(f103_iap_task, "f103_iap", 4096, NULL, 5, NULL) != pdPASS) {
        set_status(F103_IAP_FAILED);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

f103_iap_status_t f103_iap_get_status(void)
{
    return s_status;
}

int f103_iap_get_progress(void)
{
    return s_progress;
}

bool f103_iap_is_running(void)
{
    return s_running != 0;
}
