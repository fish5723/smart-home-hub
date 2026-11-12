/**
 * @file ble_svc.c
 * @brief NimBLE GATT Server（阶段 6）
 *
 * 服务/特征（16 位自定义 UUID，方便在 nRF Connect 里直接认出来）：
 *   FFF0  主服务
 *     FFF1  Notify + Read  完整协议帧（10~12 字节，见 frame.h）
 *     FFF2  Write          1 字节，非 0 点亮板载 LED（GPIO8，低有效）
 *   FFF3  配网服务（写操作要求加密链路）
 *     FFF4  Write          文本 "ssid=xxx" / "pass=xxx"（TEXT 模式即可，无需 HEX）
 *     FFF5  Write          写 "1"（或 0x01）提交：落 NVS 并立即重连
 *     FFF6  Read + Notify  2 字节：配网状态、最近断开原因
 *
 * 连接参数：连接建立后主动请求 30ms 连接间隔（24 × 1.25ms），
 * 用于后续测「采样 → Notify 发出」的时延，也是低功耗路线的真实参数。
 */
#include "ble_svc.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "net_link.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_store.h"

/* NimBLE 移植层没有公开该函数的头文件，IDF 官方示例同样是本地声明 */
void ble_store_config_init(void);

static const char *TAG = "BLE";

/* C3 SuperMini 板载 LED：GPIO8，低电平点亮 */
#define BLE_LED_GPIO        8
#define BLE_LED_ON_LEVEL    0

#define BLE_ADV_ITVL_MIN    0x20   /* 20ms */
#define BLE_ADV_ITVL_MAX    0x30   /* 30ms */

/* 30ms 连接间隔 = 24 × 1.25ms；supervision timeout 400 × 10ms = 4s */
#define BLE_CONN_ITVL       24
#define BLE_CONN_TIMEOUT    400

static const ble_uuid16_t s_svc_uuid        = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t s_chr_notify_uuid = BLE_UUID16_INIT(0xFFF1);
static const ble_uuid16_t s_chr_cmd_uuid    = BLE_UUID16_INIT(0xFFF2);
static const ble_uuid16_t s_prov_svc_uuid   = BLE_UUID16_INIT(0xFFF3);
static const ble_uuid16_t s_prov_cred_uuid  = BLE_UUID16_INIT(0xFFF4);
static const ble_uuid16_t s_prov_commit_uuid= BLE_UUID16_INIT(0xFFF5);
static const ble_uuid16_t s_prov_stat_uuid  = BLE_UUID16_INIT(0xFFF6);

static uint8_t  s_own_addr_type;
static uint16_t s_notify_handle;
static uint16_t s_cmd_handle;
static uint16_t s_stat_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_frame[16];
static uint8_t  s_frame_len;
static uint8_t  s_prov_payload[2];
static uint8_t  s_subscribed;
static uint8_t  s_prov_subscribed;
static uint8_t  s_led_on;

/* 配网暂存：两项都写齐后由 FFF5 一并提交 */
static char    s_stage_ssid[NET_LINK_SSID_MAX + 1];
static char    s_stage_pass[NET_LINK_PASS_MAX + 1];
static uint8_t s_stage_got_ssid;

static ble_svc_stats_t s_stats;

/* ─────────────────────────── GATT ─────────────────────────── */

/* 配网写操作必须在加密链路上；未加密返回 ATT 错误，手机端会先发起配对 */
static int prov_write_guard(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!desc.sec_state.encrypted) {
        ESP_LOGW(TAG, "prov write rejected: link not encrypted");
        ble_gap_security_initiate(conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    return 0;
}

static int prov_write_cred(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt)
{
    (void)conn_handle;

    /* 文本协议，手机端任何 App 的 TEXT 模式都能写，不需要 HEX：
     *   "ssid=xxx"  设置 SSID
     *   "pass=xxx"  设置密码（可省略，表示开放网络）
     * key/len 的二进制格式在 nRF Connect 里需要 HEX 输入，实测多数手机端不支持。 */
    uint8_t buf[NET_LINK_PASS_MAX + 8];
    uint16_t len = 0;

    if (OS_MBUF_PKTLEN(ctxt->om) < 5 || OS_MBUF_PKTLEN(ctxt->om) >= sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[len] = '\0';

    if (strncmp((const char *)buf, "ssid=", 5) == 0) {
        const char *v = (const char *)buf + 5;
        const size_t n = strlen(v);
        if (n > NET_LINK_SSID_MAX) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        memcpy(s_stage_ssid, v, n);
        s_stage_ssid[n] = '\0';
        s_stage_got_ssid = 1;
        ESP_LOGI(TAG, "prov: ssid='%s'", s_stage_ssid);
        return 0;
    }

    if (strncmp((const char *)buf, "pass=", 5) == 0) {
        const char *v = (const char *)buf + 5;
        const size_t n = strlen(v);
        if (n > NET_LINK_PASS_MAX) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        memcpy(s_stage_pass, v, n);
        s_stage_pass[n] = '\0';
        ESP_LOGI(TAG, "prov: password len=%u", (unsigned)n);
        return 0;
    }

    ESP_LOGW(TAG, "prov: bad format, expect 'ssid=xxx' or 'pass=xxx'");
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
}

static int prov_commit(uint16_t conn_handle)
{
    (void)conn_handle;

    if (!s_stage_got_ssid) {
        ESP_LOGW(TAG, "prov commit rejected: ssid not written yet");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    esp_err_t err = net_link_set_credentials(s_stage_ssid, s_stage_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov commit failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_stage_got_ssid = 0;
    s_stage_ssid[0] = '\0';
    s_stage_pass[0] = '\0';
    ESP_LOGI(TAG, "prov committed, connecting");
    return 0;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t *src = NULL;
        size_t src_len = 0;

        if (attr_handle == s_notify_handle) {
            src = s_frame;
            src_len = s_frame_len;
        } else if (attr_handle == s_stat_handle) {
            src = s_prov_payload;
            src_len = sizeof(s_prov_payload);
        } else {
            return BLE_ATT_ERR_UNLIKELY;
        }

        int rc = os_mbuf_append(ctxt->om, src, src_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* 只有 FFF2（控制）用这个回调，配网的两个写特征各有自己的包装函数 */
        if (attr_handle != s_cmd_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (OS_MBUF_PKTLEN(ctxt->om) < 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t cmd = 0;
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, &cmd, sizeof(cmd), &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        s_led_on = (cmd != 0) ? 1U : 0U;
        gpio_set_level(BLE_LED_GPIO, s_led_on ? BLE_LED_ON_LEVEL : !BLE_LED_ON_LEVEL);
        ESP_LOGI(TAG, "write cmd=%u -> LED %s", (unsigned)cmd, s_led_on ? "ON" : "OFF");
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* FFF4（凭据）与 FFF5（提交）各自绑定一个包装函数：
 * NimBLE 的回调里拿不到“当前是哪个特征”，只能用不同的 access_cb 区分。 */
static int gatt_write_cred(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    int guard = prov_write_guard(conn_handle);
    if (guard != 0) {
        return guard;
    }
    return prov_write_cred(conn_handle, ctxt);
}

static int gatt_write_commit(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    int guard = prov_write_guard(conn_handle);
    if (guard != 0) {
        return guard;
    }

    if (OS_MBUF_PKTLEN(ctxt->om) < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[8];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* 手机端 TEXT 模式写 "1"（0x31）即提交；HEX/二进制写 0x01 也接受 */
    if (buf[0] != 1 && buf[0] != '1') {
        ESP_LOGW(TAG, "prov commit value=0x%02x (expected '1' or 0x01)", buf[0]);
        return 0;
    }
    return prov_commit(conn_handle);
}

static int gatt_read_prov(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    int rc = os_mbuf_append(ctxt->om, s_prov_payload, sizeof(s_prov_payload));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_chr_notify_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_notify_handle,
            }, {
                .uuid = &s_chr_cmd_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_cmd_handle,
            }, {
                0,
            }
        },
    }, {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_prov_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_prov_cred_uuid.u,
                .access_cb = gatt_write_cred,
                .flags = BLE_GATT_CHR_F_WRITE,
            }, {
                .uuid = &s_prov_commit_uuid.u,
                .access_cb = gatt_write_commit,
                .flags = BLE_GATT_CHR_F_WRITE,
            }, {
                .uuid = &s_prov_stat_uuid.u,
                .access_cb = gatt_read_prov,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_stat_handle,
            }, {
                0,
            }
        },
    }, {
        0,
    }
};

/* ──────────────────────── 广播与连接 ──────────────────────── */

static void ble_advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg);

static void request_fast_conn(uint16_t conn_handle)
{
    struct ble_gap_upd_params params = {
        .itvl_min = BLE_CONN_ITVL,
        .itvl_max = BLE_CONN_ITVL,
        .latency = 0,
        .supervision_timeout = BLE_CONN_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "conn update request rc=%d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_stats.conn_count++;
            s_stats.connected = 1;
            ESP_LOGI(TAG, "connected handle=%d (total=%lu)",
                     s_conn_handle, (unsigned long)s_stats.conn_count);
            request_fast_conn(s_conn_handle);
            /* 主动发起配对，手机弹出 Just Works 确认后配网写入才被允许 */
            ble_gap_security_initiate(s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_stats.disc_count++;
        s_stats.connected = 0;
        s_subscribed = 0;
        s_prov_subscribed = 0;
        s_stats.subscribed = 0;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGW(TAG, "disconnected reason=%d (total=%lu)",
                 event->disconnect.reason, (unsigned long)s_stats.disc_count);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "advertising complete reason=%d", event->adv_complete.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_notify_handle) {
            s_subscribed = event->subscribe.cur_notify ? 1U : 0U;
            s_stats.subscribed = s_subscribed;
            ESP_LOGI(TAG, "notify %s", s_subscribed ? "ON" : "OFF");
        } else if (event->subscribe.attr_handle == s_stat_handle) {
            s_prov_subscribed = event->subscribe.cur_notify ? 1U : 0U;
            ESP_LOGI(TAG, "prov status notify %s", s_prov_subscribed ? "ON" : "OFF");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_stats.mtu = event->mtu.value;
        ESP_LOGI(TAG, "mtu=%u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "conn update: itvl=%d latency=%d timeout=%d",
                         desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
            }
        } else {
            ESP_LOGW(TAG, "conn update failed status=%d", event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc change status=%d", event->enc_change.status);
        return 0;

    default:
        return 0;
    }
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    static const ble_uuid16_t adv_uuids[] = {
        BLE_UUID16_INIT(0xFFF0),
        BLE_UUID16_INIT(0xFFF3),
    };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_SVC_NAME;
    fields.name_len = (uint8_t)strlen(BLE_SVC_NAME);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t *)adv_uuids;
    fields.num_uuids16 = 2;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_ADV_ITVL_MIN;
    params.itvl_max = BLE_ADV_ITVL_MAX;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising name=%s uuid=FFF0/FFF3", BLE_SVC_NAME);
    }
}

/* ───────────────────────── 主机回调 ───────────────────────── */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    ble_advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ───────────────────────── 对外接口 ───────────────────────── */

esp_err_t ble_svc_init(void)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << BLE_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    gpio_set_level(BLE_LED_GPIO, !BLE_LED_ON_LEVEL);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works：无显示无按键，靠手机确认；bonding 让凭据写入需要加密链路 */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_att_set_preferred_mtu(247);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_SVC_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "gap name rc=%d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "GATT server up, name=%s", BLE_SVC_NAME);
    return ESP_OK;
}

void ble_svc_notify_frame(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > sizeof(s_frame)) {
        return;
    }

    memcpy(s_frame, data, len);
    s_frame_len = (uint8_t)len;

    if (!s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_frame, len);
    if (om == NULL) {
        s_stats.notify_fail++;
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_notify_handle, om);
    if (rc != 0) {
        s_stats.notify_fail++;
    }
}

void ble_svc_prov_notify(uint8_t status, uint8_t reason)
{
    s_prov_payload[0] = status;
    s_prov_payload[1] = reason;

    if (!s_prov_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_prov_payload, sizeof(s_prov_payload));
    if (om == NULL) {
        s_stats.notify_fail++;
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_stat_handle, om);
    if (rc != 0) {
        s_stats.notify_fail++;
    }
}

void ble_svc_get_stats(ble_svc_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
    out->led_on = s_led_on;
}
