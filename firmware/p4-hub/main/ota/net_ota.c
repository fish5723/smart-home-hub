/**
 * @file net_ota.c
 * @brief P4 网络 OTA 实现（HTTP/HTTPS manifest + SHA256 + 防降级 + F103 固件缓存）
 *
 * 设计要点：
 *   - 不使用 esp_https_ota 黑盒：改为 esp_http_client 流式读取 + esp_ota_write，
 *     这样才能在下载过程中同步计算 SHA256，并与 manifest 校验；
 *   - 只做“版本更新”不做“同版本重刷”（防回滚/防误刷）；
 *   - F103 镜像只落盘不代刷：面板现有升级流程继续负责 UART 传输与断电保护；
 *   - 网络失败不改变任何分区状态，设备保持当前固件。
 */
#include "net_ota.h"
#include "ota_manifest.h"

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/build_info.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "sd_card.h"
#include "storage.h"
#include "watchdog.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "NET_OTA";

#ifndef CONFIG_SMART_HOME_NET_OTA_MANIFEST_URL
#define CONFIG_SMART_HOME_NET_OTA_MANIFEST_URL ""
#endif
#ifndef CONFIG_SMART_HOME_NET_OTA_PERIOD_S
#define CONFIG_SMART_HOME_NET_OTA_PERIOD_S 21600
#endif
#ifndef CONFIG_SMART_HOME_NET_OTA_BOOT_CHECK
#define CONFIG_SMART_HOME_NET_OTA_BOOT_CHECK 1
#endif

/* Kconfig bool 关闭时宏不存在；这里转换成 0/1，避免 #ifndef 兜底把开关又打开 */
#ifdef CONFIG_SMART_HOME_NET_OTA_REQUIRE_SIGNATURE
#define NET_OTA_REQUIRE_SIGNATURE 1
#else
#define NET_OTA_REQUIRE_SIGNATURE 0
#endif
#ifndef CONFIG_SMART_HOME_NET_OTA_PUBKEY_HEX
#define CONFIG_SMART_HOME_NET_OTA_PUBKEY_HEX ""
#endif

#ifndef CONFIG_SMART_HOME_F103_IAP_PATH
#define CONFIG_SMART_HOME_F103_IAP_PATH "/sdcard/ota/f103-node.bin"
#endif

/* 与 ota_update.c 共用同一个 pending 标记，保证两种来源的 OTA 走同一套回滚语义 */
#define NVS_KEY_OTA_PENDING "ota_pending"

#define NET_OTA_BOOT_DELAY_MS   30000
#define NET_OTA_HTTP_TIMEOUT_MS 15000
#define NET_OTA_CHUNK_SIZE      (16U * 1024U)
#define NET_OTA_IMG_MAX         (8U * 1024U * 1024U)   /* 合理性上限，防止 manifest 写错 */
#define F103_APP_BASE           0x08002000U
#define F103_APP_END            0x08010000U
#define F103_APP_SLOT_SIZE      0x0000E000U
#define F103_RAM_BASE           0x20000000U
#define F103_RAM_END            0x20005000U

static volatile net_ota_status_t s_status = NET_OTA_IDLE;
static volatile int  s_progress;
static volatile bool s_busy;
static QueueHandle_t s_cmd_q;
static char s_manifest_url[192];

static net_ota_result_t s_last_result = { .status = NET_OTA_IDLE };
static net_ota_result_cb_t s_result_cb;

/* ════════════════════════════════════════════════════
 *  工具
 * ════════════════════════════════════════════════════ */

static void set_status(net_ota_status_t st)
{
    s_status = st;
}

static bool is_https_url(const char *url)
{
    return strncmp(url, "https://", 8) == 0;
}

static void nvs_set_pending(bool pending)
{
    nvs_handle_t h;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (pending) {
        (void)nvs_set_u8(h, NVS_KEY_OTA_PENDING, 1);
    } else {
        (void)nvs_erase_key(h, NVS_KEY_OTA_PENDING);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

static void sha_to_hex(const unsigned char digest[32], char out[65])
{
    for (int i = 0; i < 32; i++) {
        snprintf(&out[i * 2], 3, "%02x", digest[i]);
    }
    out[64] = '\0';
}

static bool sha_equal(const char *a, const char *b)
{
    if (!a || !b || strlen(a) != 64 || strlen(b) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'F') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

static esp_err_t compute_file_sha256(const char *path, uint32_t *out_size, char out_sha[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t *buf = heap_caps_malloc(NET_OTA_CHUNK_SIZE,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, NET_OTA_CHUNK_SIZE, f)) > 0) {
        if (psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            free(buf);
            fclose(f);
            return ESP_FAIL;
        }
        total += (uint32_t)n;
    }
    unsigned char digest[32];
    size_t digest_len = 0;
    psa_status_t st = psa_hash_finish(&op, digest, sizeof(digest), &digest_len);
    free(buf);
    fclose(f);
    if (st != PSA_SUCCESS || digest_len != 32) return ESP_FAIL;

    sha_to_hex(digest, out_sha);
    if (out_size) *out_size = total;
    return ESP_OK;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** 严格解析定长 hex（长度必须正好 len 个字符）。 */
static bool hex_to_bytes(const char *hex, size_t len, uint8_t *out, size_t out_len)
{
    if (!hex || strlen(hex) != len || len != out_len * 2U) return false;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val(hex[i * 2U]);
        int lo = hex_val(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void current_version(char *out, size_t n)
{
    const esp_app_desc_t *cur = esp_app_get_description();
    snprintf(out, n, "%s", (cur && cur->version[0]) ? cur->version : "0.0.0");
}

/* ── 检查结果快照（供 MQTT 上报）── */
static void report_result(net_ota_status_t status, const char *current,
                          const char *target, const char *sha, const char *error)
{
    s_last_result.status = status;
    snprintf(s_last_result.current, sizeof(s_last_result.current), "%s",
             current ? current : "");
    snprintf(s_last_result.target, sizeof(s_last_result.target), "%s",
             target ? target : "");
    snprintf(s_last_result.sha256, sizeof(s_last_result.sha256), "%s",
             sha ? sha : "");
    snprintf(s_last_result.error, sizeof(s_last_result.error), "%s",
             error ? error : "");
    s_last_result.ts_us = esp_timer_get_time();

    if (s_result_cb) s_result_cb(&s_last_result);
}

/* ── manifest ECDSA-P256 签名 ──
 * 签名对象是规范化元数据：
 *   "shh-ota-v1|<p4|f103>|<version>|<size>|<sha256 小写>"
 * manifest 的 sig 为 raw r||s（各 32 字节）的 128 hex。 */
static uint8_t s_pubkey[65];   /* 0x04 || X || Y */
static bool s_pubkey_ok;

static void load_pubkey(void)
{
    const char *hex = CONFIG_SMART_HOME_NET_OTA_PUBKEY_HEX;
    s_pubkey[0] = 0x04;
    s_pubkey_ok = hex_to_bytes(hex, 128, s_pubkey + 1, 64);
    if (!s_pubkey_ok) {
        ESP_LOGE(TAG, "OTA public key missing/invalid (expect 128 hex chars)");
    }
}

static bool verify_meta_signature(const char *component, const image_meta_t *meta)
{
    if (!meta->has_sig) {
#if NET_OTA_REQUIRE_SIGNATURE
        ESP_LOGE(TAG, "%s: sig missing (signature required)", component);
        return false;
#else
        ESP_LOGW(TAG, "%s: unsigned image accepted (signature check disabled)",
                 component);
        return true;
#endif
    }
    if (!s_pubkey_ok) {
        ESP_LOGE(TAG, "%s: no valid OTA public key", component);
        return false;
    }

    uint8_t sig[64];
    if (!hex_to_bytes(meta->sig, 128, sig, sizeof(sig))) {
        ESP_LOGE(TAG, "%s: sig is not 128 hex chars", component);
        return false;
    }

    char sha_lc[65];
    for (int i = 0; i < 64; i++) {
        char c = meta->sha256[i];
        sha_lc[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    sha_lc[64] = '\0';

    char canonical[192];
    snprintf(canonical, sizeof(canonical), "shh-ota-v1|%s|%s|%lu|%s",
             component, meta->version, (unsigned long)meta->size, sha_lc);

    unsigned char digest[32];
    size_t digest_len = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&op, (const uint8_t *)canonical, strlen(canonical)) != PSA_SUCCESS ||
        psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != 32) {
        psa_hash_abort(&op);
        ESP_LOGE(TAG, "%s: canonical hash failed", component);
        return false;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, s_pubkey, sizeof(s_pubkey), &key);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "%s: import public key failed (psa=%ld)", component, (long)st);
        return false;
    }

    st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                         digest, sizeof(digest), sig, sizeof(sig));
    psa_destroy_key(key);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "%s: signature invalid (psa=%ld)", component, (long)st);
        return false;
    }

    ESP_LOGI(TAG, "%s: signature OK (v%s)", component, meta->version);
    return true;
}

/* ════════════════════════════════════════════════════
 *  HTTP
 * ════════════════════════════════════════════════════ */

static esp_http_client_config_t make_http_cfg(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = NET_OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = false,
    };
    if (is_https_url(url)) {
#if CONFIG_SMART_HOME_NET_OTA_ALLOW_INSECURE_TLS
        /* 仅用于内网自签证书联调；正式固件必须关闭。 */
        cfg.crt_bundle_attach = NULL;
        cfg.skip_cert_common_name_check = true;
#else
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }
    return cfg;
}

/** 读取整段文本（manifest），最多 cap-1 字节。
 *  手机热点/ESP-Hosted 链路偶发秒级停顿，manifest 很小，失败重试 3 次。 */
static esp_err_t http_get_text_once(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = make_http_cfg(url);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || content_len >= (int64_t)cap) {
        ESP_LOGE(TAG, "manifest HTTP %d", status);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t used = 0;
    int n = 0;
    while (used < cap - 1 &&
           (n = esp_http_client_read(cli, buf + used, (int)(cap - 1 - used))) > 0) {
        used += (size_t)n;
        wdt_feed();
    }
    buf[used] = '\0';

    if (used == cap - 1) {
        char extra;
        n = esp_http_client_read(cli, &extra, 1);
        if (n > 0) {
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return (used && n >= 0 && (content_len < 0 || used == (size_t)content_len))
               ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t http_get_text(const char *url, char *buf, size_t cap)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = http_get_text_once(url, buf, cap);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "manifest fetch attempt %d/3 failed: %s",
                 attempt, esp_err_to_name(err));
        wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    return err;
}

/* ════════════════════════════════════════════════════
 *  P4 自身升级
 * ════════════════════════════════════════════════════ */

static esp_err_t download_p4_image(const image_meta_t *meta)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "no inactive OTA partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (meta->size == 0 || meta->size > update->size || meta->size > NET_OTA_IMG_MAX) {
        ESP_LOGE(TAG, "image size %u invalid (partition %lu)",
                 (unsigned)meta->size, (unsigned long)update->size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(NET_OTA_CHUNK_SIZE,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = make_http_cfg(meta->url);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(buf); return ESP_ERR_NO_MEM; }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", meta->url, esp_err_to_name(err));
        goto fail_cli;
    }

    int64_t content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || content_len < 0 ||
        (uint32_t)content_len != meta->size) {
        ESP_LOGE(TAG, "image HTTP %d len=%lld (expect %u)",
                 status, content_len, (unsigned)meta->size);
        err = ESP_ERR_INVALID_SIZE;
        goto fail_cli;
    }

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        goto fail_cli;
    }

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa hash setup failed");
        esp_ota_abort(handle);
        err = ESP_ERR_INVALID_STATE;
        goto fail_cli;
    }

    uint32_t written = 0;
    int n;
    while ((n = esp_http_client_read(cli, (char *)buf, (int)NET_OTA_CHUNK_SIZE)) > 0) {
        err = esp_ota_write(handle, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            psa_hash_abort(&op);
            esp_ota_abort(handle);
            goto fail_cli;
        }
        if (psa_hash_update(&op, buf, (size_t)n) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa hash update failed");
            psa_hash_abort(&op);
            esp_ota_abort(handle);
            err = ESP_FAIL;
            goto fail_cli;
        }
        written += (uint32_t)n;
        s_progress = (int)((written * 100U) / meta->size);
        wdt_feed();
        vTaskDelay(1);
    }
    if (n < 0) {
        ESP_LOGE(TAG, "image read error");
        psa_hash_abort(&op);
        esp_ota_abort(handle);
        err = ESP_FAIL;
        goto fail_cli;
    }

    unsigned char digest[32];
    size_t digest_len = 0;
    if (psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != 32) {
        ESP_LOGE(TAG, "psa hash finish failed");
        esp_ota_abort(handle);
        err = ESP_FAIL;
        goto fail_cli;
    }

    char sha[65];
    sha_to_hex(digest, sha);
    if (written != meta->size || !sha_equal(sha, meta->sha256)) {
        ESP_LOGE(TAG, "image verify failed: %u/%u sha=%s", (unsigned)written,
                 (unsigned)meta->size, sha);
        esp_ota_abort(handle);
        err = ESP_ERR_INVALID_CRC;
        goto fail_cli;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        goto fail_cli;
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(update, &desc) == ESP_OK) {
        const esp_app_desc_t *cur = esp_app_get_description();
        if (cur && strcmp(desc.project_name, cur->project_name) != 0) {
            ESP_LOGE(TAG, "project mismatch: %s vs %s",
                     desc.project_name, cur->project_name);
            err = ESP_ERR_INVALID_ARG;
            goto fail_cli;
        }
        ESP_LOGI(TAG, "new image: project=%s version=%s",
                 desc.project_name, desc.version);
    }

    nvs_set_pending(true);
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition: %s", esp_err_to_name(err));
        goto fail_cli;
    }

    ESP_LOGW(TAG, "P4 OTA verified (%u bytes) — rebooting", (unsigned)written);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(buf);
    return ESP_OK;

fail_cli:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(buf);
    return err;
}

/* ════════════════════════════════════════════════════
 *  F103 固件缓存（只落盘）
 * ════════════════════════════════════════════════════ */

static bool f103_vector_ok(const uint8_t *img, uint32_t size)
{
    if (size < 8U || size > F103_APP_SLOT_SIZE) return false;
    uint32_t sp = (uint32_t)img[0] | ((uint32_t)img[1] << 8) |
                  ((uint32_t)img[2] << 16) | ((uint32_t)img[3] << 24);
    uint32_t reset = (uint32_t)img[4] | ((uint32_t)img[5] << 8) |
                     ((uint32_t)img[6] << 16) | ((uint32_t)img[7] << 24);
    if (sp < F103_RAM_BASE || sp > F103_RAM_END || (sp & 0x3U)) return false;
    if ((reset & 1U) == 0U || reset < F103_APP_BASE || reset >= F103_APP_END) return false;
    if (img[0] == 0xE9U) return false;
    return true;
}

static esp_err_t sync_f103_image(const image_meta_t *meta)
{
    const char *mount = sd_card_get_mount_point();
    if (!mount || strlen(mount) == 0) return ESP_ERR_INVALID_STATE;

    char path[160];
    char tmp[168];
    snprintf(path, sizeof(path), "%s", CONFIG_SMART_HOME_F103_IAP_PATH);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* 目标已是最新则直接跳过 */
    uint32_t local_size = 0;
    char local_sha[65];
    if (compute_file_sha256(path, &local_size, local_sha) == ESP_OK &&
        local_size == meta->size && sha_equal(local_sha, meta->sha256)) {
        ESP_LOGI(TAG, "F103 image already current (%u bytes)", (unsigned)local_size);
        return ESP_OK;
    }

    (void)mkdir("/sdcard/ota", 0777);

    /* 先把整幅镜像读进内存（F103 app ≤56KB），避免 Wi-Fi 读取与 SD 写入
     * 交叉进行时被 FATFS 或网络任一方的超时打断。 */
    uint8_t *img = heap_caps_malloc(meta->size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!img) img = heap_caps_malloc(meta->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = make_http_cfg(meta->url);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(img); return ESP_ERR_NO_MEM; }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", meta->url, esp_err_to_name(err));
        goto fail_cli;
    }
    int64_t content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || content_len < 0 || (uint32_t)content_len != meta->size) {
        ESP_LOGE(TAG, "f103 HTTP %d len=%lld (expect %u)",
                 status, content_len, (unsigned)meta->size);
        err = ESP_ERR_INVALID_SIZE;
        goto fail_cli;
    }

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        err = ESP_ERR_INVALID_STATE;
        goto fail_cli;
    }

    uint32_t got = 0;
    int n = 0;
    while (got < meta->size) {
        n = esp_http_client_read(cli, (char *)(img + got), (int)(meta->size - got));
        if (n <= 0) break;
        if (psa_hash_update(&op, img + got, (size_t)n) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa hash update failed");
            err = ESP_FAIL;
            break;
        }
        got += (uint32_t)n;
        s_progress = (int)((got * 100U) / meta->size);
        wdt_feed();
        vTaskDelay(1);
    }
    if (n < 0) {
        ESP_LOGE(TAG, "f103 read error: n=%d got=%u errno=%d",
                 n, (unsigned)got, esp_http_client_get_errno(cli));
    }

    unsigned char digest[32];
    size_t digest_len = 0;
    if (psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != 32) {
        if (err == ESP_OK) err = ESP_FAIL;
        psa_hash_abort(&op);
    }

    if (err != ESP_OK || n < 0 || got != meta->size) {
        if (err == ESP_OK) err = ESP_FAIL;
        goto fail_cli;
    }

    char sha[65];
    sha_to_hex(digest, sha);
    if (!sha_equal(sha, meta->sha256)) {
        ESP_LOGE(TAG, "f103 verify failed: %u/%u", (unsigned)got, (unsigned)meta->size);
        err = ESP_ERR_INVALID_CRC;
        goto fail_cli;
    }

    /* 向量校验：拒绝把 ESP 镜像或坏偏移的文件交给面板 */
    if (!f103_vector_ok(img, meta->size)) {
        ESP_LOGE(TAG, "f103 image vector invalid");
        err = ESP_ERR_INVALID_ARG;
        goto fail_cli;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "open %s failed", tmp);
        err = ESP_ERR_NOT_FOUND;
        goto fail_cli;
    }
    bool wok = fwrite(img, 1, meta->size, fp) == meta->size;
    fclose(fp);
    if (!wok) {
        ESP_LOGE(TAG, "SD write failed");
        remove(tmp);
        err = ESP_FAIL;
        goto fail_cli;
    }

    remove(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename temp failed");
        remove(tmp);
        err = ESP_FAIL;
        goto fail_cli;
    }

    ESP_LOGI(TAG, "F103 image cached: %s (%u bytes, v%s)",
             path, (unsigned)got, meta->version);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(img);
    return ESP_OK;

fail_cli:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(img);
    return err;
}

/* ════════════════════════════════════════════════════
 *  主流程
 * ════════════════════════════════════════════════════ */

static esp_err_t do_check(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;

    char cur_ver[40];
    current_version(cur_ver, sizeof(cur_ver));

    if (s_manifest_url[0] == '\0') {
        ESP_LOGW(TAG, "manifest URL not configured (Kconfig)");
        set_status(NET_OTA_IDLE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi not connected");
        set_status(NET_OTA_OFFLINE);
        report_result(NET_OTA_OFFLINE, cur_ver, "", "", "WIFI_OFFLINE");
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    s_progress = 0;
    set_status(NET_OTA_CHECKING);

    char ip[16] = {0}, gw[16] = {0}, mask[16] = {0};
    (void)wifi_get_ip_info(ip, gw, mask, sizeof(ip));
    ESP_LOGI(TAG, "checking %s (local ip=%s gw=%s)",
             s_manifest_url, ip[0] ? ip : "?", gw[0] ? gw : "?");

    char *text = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!text) {
        s_busy = false;
        set_status(NET_OTA_FAILED);
        report_result(NET_OTA_FAILED, cur_ver, "", "", "NO_MEM");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = http_get_text(s_manifest_url, text, 4096);
    if (err != ESP_OK) {
        free(text);
        s_busy = false;
        set_status(NET_OTA_OFFLINE);
        report_result(NET_OTA_OFFLINE, cur_ver, "", "", "SERVER_UNREACHABLE");
        return err;
    }

    cJSON *root = cJSON_ParseWithOpts(text, NULL, true);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "manifest JSON parse failed");
        s_busy = false;
        set_status(NET_OTA_FAILED);
        report_result(NET_OTA_FAILED, cur_ver, "", "", "MANIFEST_PARSE");
        return ESP_ERR_INVALID_ARG;
    }

    image_meta_t p4 = {0}, f103 = {0};
    bool p4_present = cJSON_GetObjectItemCaseSensitive(root, "p4") != NULL;
    bool f103_present = cJSON_GetObjectItemCaseSensitive(root, "f103") != NULL;
    bool has_p4 = p4_present && ota_parse_image_meta(root, "p4", &p4);
    bool has_f103 = f103_present && ota_parse_image_meta(root, "f103", &f103);

    if ((p4_present && !has_p4) || (f103_present && !has_f103)) {
        ESP_LOGE(TAG, "manifest object invalid (p4=%d f103=%d)",
                 (int)has_p4, (int)has_f103);
        cJSON_Delete(root);
        s_busy = false;
        set_status(NET_OTA_FAILED);
        report_result(NET_OTA_FAILED, cur_ver, "", "", "MANIFEST_INVALID");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "manifest: p4=%s (%u bytes) f103=%s (%u bytes), current=%s",
             has_p4 ? p4.version : "-", (unsigned)p4.size,
             has_f103 ? f103.version : "-", (unsigned)f103.size, cur_ver);

    /* 版本号同样来自不可信 manifest：先验签，再作任何版本比较。 */
    if (has_p4 && !verify_meta_signature("p4", &p4)) {
        cJSON_Delete(root);
        s_busy = false;
        set_status(NET_OTA_FAILED);
        report_result(NET_OTA_FAILED, cur_ver, p4.version, p4.sha256,
                      "INVALID_SIGNATURE");
        return ESP_ERR_INVALID_CRC;
    }
    if (has_f103 && !verify_meta_signature("f103", &f103)) {
        cJSON_Delete(root);
        s_busy = false;
        set_status(NET_OTA_FAILED);
        report_result(NET_OTA_FAILED, cur_ver, f103.version, f103.sha256,
                      "INVALID_SIGNATURE");
        return ESP_ERR_INVALID_CRC;
    }

    if (has_p4 && ota_version_is_newer(p4.version, cur_ver)) {

        set_status(NET_OTA_DOWNLOADING);
        err = download_p4_image(&p4);
        if (err == ESP_OK) {
            set_status(NET_OTA_SUCCESS);
            /* 先让 MQTT 结果发出去，再重启 */
            report_result(NET_OTA_SUCCESS, cur_ver, p4.version, p4.sha256, "");
            cJSON_Delete(root);
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();
        }
        ESP_LOGE(TAG, "P4 network OTA failed: %s", esp_err_to_name(err));
        s_busy = false;
        set_status(NET_OTA_FAILED);
        report_result(NET_OTA_FAILED, cur_ver, p4.version, p4.sha256,
                      esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    if (has_f103 && sd_card_is_mounted()) {
        s_progress = 0;
        set_status(NET_OTA_DOWNLOADING);
        err = sync_f103_image(&f103);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "F103 image sync failed: %s", esp_err_to_name(err));
            s_busy = false;
            set_status(NET_OTA_FAILED);
            report_result(NET_OTA_FAILED, cur_ver, f103.version, f103.sha256,
                          esp_err_to_name(err));
            cJSON_Delete(root);
            return err;
        }
    }

    cJSON_Delete(root);
    s_busy = false;
    s_progress = 0;
    set_status(NET_OTA_UP_TO_DATE);
    report_result(NET_OTA_UP_TO_DATE, cur_ver, cur_ver, "", "");
    return ESP_OK;
}

static void net_ota_task(void *arg)
{
    (void)arg;
    wdt_add_current_task();

    int64_t next_periodic_us = 0;
    int64_t boot_check_us = esp_timer_get_time() +
                            (int64_t)NET_OTA_BOOT_DELAY_MS * 1000;

#if CONFIG_SMART_HOME_NET_OTA_BOOT_CHECK
    bool boot_check_done = false;
#else
    bool boot_check_done = true;
#endif

    int64_t now = esp_timer_get_time();
    next_periodic_us = now + (int64_t)CONFIG_SMART_HOME_NET_OTA_PERIOD_S * 1000000;

    while (1) {
        uint8_t cmd = 0;
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            (void)do_check();
            next_periodic_us = esp_timer_get_time() +
                               (int64_t)CONFIG_SMART_HOME_NET_OTA_PERIOD_S * 1000000;
        } else if (!boot_check_done && esp_timer_get_time() >= boot_check_us) {
            if (wifi_is_connected()) {
                boot_check_done = true;
                (void)do_check();
                next_periodic_us = esp_timer_get_time() +
                                   (int64_t)CONFIG_SMART_HOME_NET_OTA_PERIOD_S * 1000000;
            } else if (esp_timer_get_time() >= boot_check_us + 120000000LL) {
                /* 等 2 分钟仍无 Wi-Fi：放弃本次开机检查，留给定时任务 */
                boot_check_done = true;
            }
        } else if (esp_timer_get_time() >= next_periodic_us) {
            next_periodic_us = esp_timer_get_time() +
                               (int64_t)CONFIG_SMART_HOME_NET_OTA_PERIOD_S * 1000000;
            if (wifi_is_connected() && !s_busy) {
                (void)do_check();
            }
        }
        wdt_feed();
    }
}

esp_err_t net_ota_init(void)
{
    snprintf(s_manifest_url, sizeof(s_manifest_url), "%s",
             CONFIG_SMART_HOME_NET_OTA_MANIFEST_URL);

    if (!s_cmd_q) {
        s_cmd_q = xQueueCreate(1, sizeof(uint8_t));
        if (!s_cmd_q) return ESP_ERR_NO_MEM;
    }
    s_status = NET_OTA_IDLE;
    s_progress = 0;
    s_busy = false;

    /* PSA crypto 需要显式初始化一次，SHA256 流式哈希与 ECDSA 验签依赖它。 */
    (void)psa_crypto_init();
    load_pubkey();

    current_version(s_last_result.current, sizeof(s_last_result.current));
    s_last_result.status = NET_OTA_IDLE;
    ESP_LOGI(TAG, "signature verification: %s (pubkey %s)",
             NET_OTA_REQUIRE_SIGNATURE ? "required" : "optional",
             s_pubkey_ok ? "loaded" : "MISSING");

    if (s_manifest_url[0] == '\0') {
        ESP_LOGW(TAG, "network OTA disabled: URL empty");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "network OTA ready: %s (period %ds)",
             s_manifest_url, CONFIG_SMART_HOME_NET_OTA_PERIOD_S);

    if (xTaskCreate(net_ota_task, "net_ota", 10240, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t net_ota_check_now(void)
{
    if (!s_cmd_q) return ESP_ERR_INVALID_STATE;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    uint8_t cmd = 1;
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

net_ota_status_t net_ota_get_status(void)
{
    return s_status;
}

int net_ota_get_progress(void)
{
    return s_progress;
}

bool net_ota_is_busy(void)
{
    return s_busy;
}

void net_ota_set_result_cb(net_ota_result_cb_t cb)
{
    s_result_cb = cb;
}

const net_ota_result_t *net_ota_get_last_result(void)
{
    return &s_last_result;
}
