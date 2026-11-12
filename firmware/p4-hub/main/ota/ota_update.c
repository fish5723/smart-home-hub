/**
 * @file ota_update.c
 * @brief P4 双分区 OTA（SD 卡触发 + bootloader 回滚）
 */
#include "ota_update.h"

#include "sdkconfig.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sd_card.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "OTA";

/* ESP 镜像 magic：第一个字节必须是 0xE9，避免把任意文件写进 OTA 分区。 */
#define ESP_IMAGE_MAGIC 0xE9U

/* 写 flash 的缓冲区放内部 RAM：esp_partition_write/esp_flash_write 的
 * 数据源需要 DMA 可访问；PSRAM 在部分配置下不能直接作为 SPI flash 写源。 */
#define OTA_CHUNK_SIZE (16U * 1024U)

#ifndef CONFIG_SMART_HOME_OTA_SD_PATH
#define CONFIG_SMART_HOME_OTA_SD_PATH "/sdcard/ota/p4-hub.bin"
#endif

#define NVS_KEY_PENDING "ota_pending"

static char s_label[24] = "?";
static char s_version[40] = "?";

/* ── NVS pending 标记：区分“新镜像待验证”和“已回滚” ── */
static bool ota_pending_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    (void)nvs_get_u8(h, NVS_KEY_PENDING, &v);
    nvs_close(h);
    return v != 0;
}

static void ota_pending_set(bool pending)
{
    nvs_handle_t h;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (pending) {
        (void)nvs_set_u8(h, NVS_KEY_PENDING, 1);
    } else {
        (void)nvs_erase_key(h, NVS_KEY_PENDING);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

const char *ota_update_running_label(void)   { return s_label; }
const char *ota_update_running_version(void) { return s_version; }

esp_err_t ota_update_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();

    snprintf(s_label, sizeof(s_label), "%s", run ? run->label : "?");
    snprintf(s_version, sizeof(s_version), "%s", app ? app->version : "?");

    ESP_LOGI(TAG, "running partition=%s version=%s project=%s",
             s_label, s_version, app ? app->project_name : "?");

    if (!run) return ESP_OK;

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
        ESP_LOGI(TAG, "image state=%d (0=new 1=pending 2=valid 3=invalid 4=aborted)",
                 (int)st);
        if (ota_pending_get() && st != ESP_OTA_IMG_PENDING_VERIFY) {
            /* 新镜像没走到 mark_valid 就复位了，bootloader 已回滚到旧分区 */
            ESP_LOGW(TAG, "previous OTA did not validate — bootloader rolled back");
            ota_pending_set(false);
        } else if (st == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "OTA pending verify — mark_valid after self-check");
        }
    }
    return ESP_OK;
}

esp_err_t ota_update_mark_valid(void)
{
#if CONFIG_SMART_HOME_OTA_ROLLBACK_SELFTEST
    /* 验收坏固件回滚：故意在 mark_valid 前崩溃，bootloader 应回滚到旧分区。 */
    ESP_LOGE(TAG, "ROLLBACK SELFTEST enabled — abort before mark_valid");
    abort();
#endif
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_INVALID;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "image marked valid — rollback cancelled");
        } else {
            ESP_LOGE(TAG, "mark valid failed: %s", esp_err_to_name(err));
        }
    }
    ota_pending_set(false);
    return ESP_OK;
}

esp_err_t ota_update_check_sd(void)
{
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted — skip OTA check");
        return ESP_ERR_INVALID_STATE;
    }

    const char *path = CONFIG_SMART_HOME_OTA_SD_PATH;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGI(TAG, "no OTA file at %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "no inactive OTA partition (check partitions.csv)");
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)st.st_size > update->size) {
        ESP_LOGE(TAG, "image too large: %ld > %lu bytes", (long)st.st_size,
                 (unsigned long)update->size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t magic = 0;
    if (fread(&magic, 1, 1, f) != 1 || magic != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "%s is not an ESP app image (magic=0x%02X)", path, magic);
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    rewind(f);

    ESP_LOGW(TAG, "SD OTA start: %s size=%ld target=%s",
             path, (long)st.st_size, update->label);

    uint8_t *buf = heap_caps_malloc(OTA_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "OTA buffer alloc failed");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(buf);
        fclose(f);
        return err;
    }

    size_t written = 0;
    while (err == ESP_OK) {
        size_t n = fread(buf, 1, OTA_CHUNK_SIZE, f);
        if (n == 0) break;
        err = esp_ota_write(handle, buf, n);
        if (err == ESP_OK) {
            written += n;
            (void)esp_task_wdt_reset();   /* 主任务已注册 TWDT，写 flash 期间喂狗 */
            vTaskDelay(1);
        }
    }

    bool ended = false;
    if (err == ESP_OK) {
        err = esp_ota_end(handle);
        ended = true;                     /* esp_ota_end 无论成败都会释放 handle */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        }
    }

    if (err == ESP_OK) {
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(update, &desc) == ESP_OK) {
            ESP_LOGI(TAG, "new image: project=%s version=%s", desc.project_name, desc.version);
            const esp_app_desc_t *cur = esp_app_get_description();
            if (cur && strcmp(desc.project_name, cur->project_name) != 0) {
                ESP_LOGE(TAG, "project mismatch: %s vs %s", desc.project_name, cur->project_name);
                err = ESP_ERR_INVALID_ARG;
            }
        } else {
            ESP_LOGW(TAG, "new image description unavailable");
        }
    }

    if (err == ESP_OK) {
        ota_pending_set(true);
        err = esp_ota_set_boot_partition(update);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        }
    }

    if (!ended && handle) {
        (void)esp_ota_abort(handle);
    }
    free(buf);
    fclose(f);

    if (err == ESP_OK) {
        /* 删除文件而不是改名：成功启动后不需要保留；回滚后也不会重复刷同一镜像。 */
        (void)remove(path);
        ESP_LOGW(TAG, "SD OTA success (%u/%ld bytes) — rebooting",
                 (unsigned)written, (long)st.st_size);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    ESP_LOGE(TAG, "SD OTA failed: %s", esp_err_to_name(err));
    return err;
}
