/*
 * SPDX-FileCopyrightText: 2025 Smart Home Hub
 * SPDX-License-Identifier: Apache-2.0
 *
 * weather_service.c — SNTP 授时 + uapis.cn 天气 HTTPS + 缓存 + 场景映射
 *
 * 天气源：uapis.cn（UApiPro）单端点 GET /api/v1/misc/weather（ADR-020）。
 *  - 认证：Authorization: Bearer <wx_key>；未配置 Key 时只授时、不请求天气。
 *  - 定位：默认按来源 IP 自动定位；可选 ?adcode= / ?city=（来自 NVS 'wx_loc'）。
 *  - forecast=true 附带当日日出日落 → 昼夜判定；缺失时回退本地时段。
 *  - weather_icon 与和风 icon 码同体系，场景映射表原样复用。
 *
 * 设计要点：
 *  - 全部网络/解析动作在这个模块自己的任务里做，绝不阻塞 LVGL 任务。
 *  - Wi-Fi 状态用轮询获取（wifi_is_connected()），不占用 wifi.c 的单槽回调。
 *  - 凭据来源：NVS(storage) → Kconfig 空默认值；无 Key = 不请求（延续 ADR-016 凭据纪律）。
 *  - 任何失败都只降级：保留缓存/本地时间，指数退避重试，不阻塞、不重启。
 */

#include "weather_service.h"
#include "storage.h"
#include "wifi.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "cJSON.h"

static const char *TAG = "WEATHER";

/* 天气源固定为 uapis.cn（UApiPro，ADR-020）：单端点、图标码与和风 icon 同体系 */
#define WEATHER_API_HOST "uapis.cn"
#define WEATHER_API_PATH "/api/v1/misc/weather"

#ifndef CONFIG_SMART_HOME_WEATHER_KEY
#define CONFIG_SMART_HOME_WEATHER_KEY ""
#endif
#ifndef CONFIG_SMART_HOME_WEATHER_LOCATION
#define CONFIG_SMART_HOME_WEATHER_LOCATION ""
#endif
#ifndef CONFIG_SMART_HOME_WEATHER_REFRESH_SEC
#define CONFIG_SMART_HOME_WEATHER_REFRESH_SEC 900
#endif

#define NVS_KEY_API   "wx_key"    /* Bearer Token（必需；未配置则只授时、不请求） */
#define NVS_KEY_LOC   "wx_loc"    /* 可选：空 = 按 IP 自动定位；或 adcode / 城市名 */
#define NVS_KEY_CACHE "wx_cache"

#define TASK_STACK      8192
#define TASK_PRIO       3
#define HTTP_TIMEOUT_MS 10000
#define RESP_CAP        4096   /* forecast=true 实测约 2.2 KB */
#define BACKOFF_MIN_S   60
#define BACKOFF_MAX_S   900
#define SNTP_WAIT_MAX_S 20

/* ══════════════════════════════════════════════════════════════════
   纯函数：场景映射（始终编译）
   待机层的兜底配色、壁纸路径拼装、无天气服务时都依赖这几个函数，
   因此它们不受 SMART_HOME_WEATHER_ENABLED 影响。
   ══════════════════════════════════════════════════════════════════ */
static const struct {
    const char      *name;
    weather_scene_t  scene;
} SCENE_NAMES[] = {
    { "sunny",    WEATHER_SCENE_CLEAR    },
    { "cloudy",   WEATHER_SCENE_CLOUDY   },
    { "overcast", WEATHER_SCENE_OVERCAST },
    { "rain",     WEATHER_SCENE_RAIN     },
    { "storm",    WEATHER_SCENE_THUNDER  },
    { "snow",     WEATHER_SCENE_SNOW     },
    { "fog",      WEATHER_SCENE_FOG      },
    { "haze",     WEATHER_SCENE_HAZE     },
    { "night",    WEATHER_SCENE_NIGHT    },
};

const char *weather_scene_to_string(weather_scene_t scene)
{
    for (size_t i = 0; i < sizeof(SCENE_NAMES) / sizeof(SCENE_NAMES[0]); i++) {
        if (SCENE_NAMES[i].scene == scene) {
            return SCENE_NAMES[i].name;
        }
    }
    return "unknown";
}

weather_scene_t weather_scene_from_string(const char *name)
{
    if (!name || !name[0]) return WEATHER_SCENE_UNKNOWN;
    for (size_t i = 0; i < sizeof(SCENE_NAMES) / sizeof(SCENE_NAMES[0]); i++) {
        if (strcmp(SCENE_NAMES[i].name, name) == 0) {
            return SCENE_NAMES[i].scene;
        }
    }
    return WEATHER_SCENE_UNKNOWN;
}

weather_scene_t weather_scene_from_code(int code, bool is_night)
{
    if (code <= 0) {
        return is_night ? WEATHER_SCENE_NIGHT : WEATHER_SCENE_UNKNOWN;
    }
    if (code >= 200 && code < 300) return WEATHER_SCENE_THUNDER;
    if (code >= 300 && code < 400) return WEATHER_SCENE_RAIN;
    if (code >= 400 && code < 500) return WEATHER_SCENE_SNOW;
    switch (code) {
    case 100: case 150:                            /* 晴 / 晴(夜) */
        return WEATHER_SCENE_CLEAR;
    case 101: case 102: case 103:
    case 151: case 152: case 153:                  /* 多云 / 少云 / 晴间多云 */
        return WEATHER_SCENE_CLOUDY;
    case 104:                                      /* 阴 */
        return WEATHER_SCENE_OVERCAST;
    case 500: case 501: case 509: case 510: case 514: case 515:  /* 薄雾/雾/浓雾 */
        return WEATHER_SCENE_FOG;
    case 502: case 503: case 504: case 507: case 508:            /* 霾/扬沙/浮尘/沙尘暴 */
        return WEATHER_SCENE_HAZE;
    case 900: case 901:                            /* 热 / 冷 */
        return WEATHER_SCENE_UNKNOWN;
    default:
        return is_night ? WEATHER_SCENE_NIGHT : WEATHER_SCENE_UNKNOWN;
    }
}

/* ══════════════════════════════════════════════════════════════════
   网络 / 任务部分 —— 可用 SMART_HOME_WEATHER_ENABLED=n 单独关闭做隔离测试
   ══════════════════════════════════════════════════════════════════ */
#if CONFIG_SMART_HOME_WEATHER_ENABLED

static const char *SNTP_SERVERS[] = { "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org" };

/* NVS 缓存格式（cJSON）: {"t":25,"c":100,"n":0,"u":1696500000,"txt":"晴","cty":"深圳","sr":370,"ss":1108} */
typedef struct {
    bool   have_sun;
    int    sunrise_min;   /* 当日日出（0 点起的分钟数） */
    int    sunset_min;    /* 当日日落 */
    int    sun_ymd;       /* 日出/日落对应的日期 yyyymmdd */
} sun_cache_t;

static TaskHandle_t  s_task;
static volatile bool s_stop;
static volatile bool s_refresh_req;
static SemaphoreHandle_t s_lock;
static weather_state_t s_state;
static sun_cache_t     s_sun;
static bool            s_time_valid;
static bool            s_started;

static char s_api_key[80];
static char s_location[80];
static weather_update_cb_t s_cb;
static void               *s_cb_user;

/* ── 状态快照 ── */
static void state_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void state_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void publish(void)
{
    weather_state_t copy;
    state_lock();
    copy = s_state;
    state_unlock();
    if (s_cb) {
        s_cb(&copy, s_cb_user);
    }
}

void weather_service_get_state(weather_state_t *out)
{
    if (!out) return;
    state_lock();
    *out = s_state;
    state_unlock();
}

void weather_service_register_cb(weather_update_cb_t cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
}

bool weather_service_time_valid(void) { return s_time_valid; }

esp_err_t weather_service_refresh_now(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    s_refresh_req = true;
    return ESP_OK;
}

/* ── 昼/夜判定 ── */
static bool compute_is_night(time_t now)
{
    struct tm tmv;
    localtime_r(&now, &tmv);
    int minute_of_day = tmv.tm_hour * 60 + tmv.tm_min;
    int ymd = (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;

    if (s_sun.have_sun && s_sun.sun_ymd == ymd &&
        s_sun.sunrise_min > 0 && s_sun.sunset_min > s_sun.sunrise_min) {
        return (minute_of_day < s_sun.sunrise_min || minute_of_day >= s_sun.sunset_min);
    }
    /* 无日出日落数据时的保守回退 */
    return (tmv.tm_hour < 6 || tmv.tm_hour >= 18);
}

/* ── HTTP ── */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} http_acc_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_acc_t *acc = (http_acc_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!acc || !acc->buf) break;
        if (acc->len + evt->data_len < acc->cap) {
            memcpy(acc->buf + acc->len, evt->data, evt->data_len);
            acc->len += evt->data_len;
            acc->buf[acc->len] = '\0';
        } else {
            acc->overflow = true;
        }
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "http error event");
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url, const char *bearer, char *out, size_t cap)
{
    if (!url || !out || cap == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    http_acc_t acc = { .buf = out, .cap = cap, .len = 0, .overflow = false };
    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_event_cb,
        .user_data         = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .buffer_size       = 1024,
        .buffer_size_tx    = 512,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "http client init failed");
        return ESP_ERR_NO_MEM;
    }
    /* 仅非空 Key 才发 Bearer（无 Key 时上层不发起请求；发无效 Key 反而 401） */
    if (bearer && bearer[0]) {
        char auth[96];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GET status %d%s", status,
                 status == 401 ? " — Key 无效/过期：更新 NVS 'wx_key' 或 menuconfig" :
                 status == 429 ? " — 触发限流，按退避重试" : "");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (acc.overflow) {
        ESP_LOGW(TAG, "response overflow (%u bytes cap)", (unsigned)cap);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ── 凭据加载 ── */
static void load_credentials(void)
{
    size_t len = sizeof(s_api_key);
    if (storage_get_str(NVS_KEY_API, s_api_key, &len) != ESP_OK) {
        strlcpy(s_api_key, CONFIG_SMART_HOME_WEATHER_KEY, sizeof(s_api_key));
    }
    len = sizeof(s_location);
    if (storage_get_str(NVS_KEY_LOC, s_location, &len) != ESP_OK) {
        strlcpy(s_location, CONFIG_SMART_HOME_WEATHER_LOCATION, sizeof(s_location));
    }

    ESP_LOGI(TAG, "credentials: location=%s key=%s",
             s_location[0] ? s_location : "(IP auto)",
             s_api_key[0] ? "(set)" : "(none)");
    if (s_api_key[0] == '\0') {
        ESP_LOGW(TAG, "未配置天气 Key — 只授时，不请求天气。"
                      "配置方式：NVS 键 '%s' 或 menuconfig（见 main/README.md §5）",
                 NVS_KEY_API);
    }
}

/* ── SNTP ── */
static void start_sntp(void)
{
    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    /* lwip 只保留 CONFIG_LWIP_SNTP_MAX_SERVERS 个服务器（本工程默认 1），
     * 多写的会被 lwip 静默忽略，这里显式按上限截断并打印实际生效数量。 */
    u8_t n = (u8_t)(sizeof(SNTP_SERVERS) / sizeof(SNTP_SERVERS[0]));
#ifndef CONFIG_LWIP_SNTP_MAX_SERVERS
#define CONFIG_LWIP_SNTP_MAX_SERVERS 1
#endif
    if (n > CONFIG_LWIP_SNTP_MAX_SERVERS) {
        n = (u8_t)CONFIG_LWIP_SNTP_MAX_SERVERS;
    }
    for (u8_t i = 0; i < n; i++) {
        esp_sntp_setservername(i, SNTP_SERVERS[i]);
    }
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (%u server(s), primary %s)", (unsigned)n, SNTP_SERVERS[0]);
}

static void wait_for_time(void)
{
    for (int i = 0; i < SNTP_WAIT_MAX_S && !s_stop; i++) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            s_time_valid = true;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "SNTP not synced within %d s — 继续使用本地时间", SNTP_WAIT_MAX_S);
}

/* ── 缓存 ── */
static void cache_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    weather_state_t st;
    state_lock();
    st = s_state;
    state_unlock();

    cJSON_AddNumberToObject(root, "t", st.temperature_c);
    cJSON_AddNumberToObject(root, "c", st.weather_code);
    cJSON_AddNumberToObject(root, "n", st.is_night ? 1 : 0);
    cJSON_AddNumberToObject(root, "u", (double)st.updated_at);
    cJSON_AddStringToObject(root, "txt", st.condition);
    cJSON_AddStringToObject(root, "cty", st.city);
    if (s_sun.have_sun) {
        cJSON_AddNumberToObject(root, "sr", s_sun.sunrise_min);
        cJSON_AddNumberToObject(root, "ss", s_sun.sunset_min);
        cJSON_AddNumberToObject(root, "sd", s_sun.sun_ymd);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;
    storage_set_str(NVS_KEY_CACHE, txt);
    cJSON_free(txt);
}

static bool cache_load(void)
{
    char buf[512];
    size_t len = sizeof(buf);
    if (storage_get_str(NVS_KEY_CACHE, buf, &len) != ESP_OK) {
        return false;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "cache parse failed");
        return false;
    }
    cJSON *t   = cJSON_GetObjectItem(root, "t");
    cJSON *c   = cJSON_GetObjectItem(root, "c");
    cJSON *n   = cJSON_GetObjectItem(root, "n");
    cJSON *u   = cJSON_GetObjectItem(root, "u");
    cJSON *txt = cJSON_GetObjectItem(root, "txt");
    cJSON *cty = cJSON_GetObjectItem(root, "cty");
    cJSON *sr  = cJSON_GetObjectItem(root, "sr");
    cJSON *ss  = cJSON_GetObjectItem(root, "ss");
    cJSON *sd  = cJSON_GetObjectItem(root, "sd");

    state_lock();
    memset(&s_state, 0, sizeof(s_state));
    s_state.temperature_c = t ? (int)t->valuedouble : 0;
    s_state.weather_code  = c ? (int)c->valuedouble : 0;
    s_state.is_night      = n ? (n->valuedouble != 0) : false;
    s_state.updated_at    = u ? (time_t)u->valuedouble : 0;
    if (txt && cJSON_IsString(txt)) {
        strlcpy(s_state.condition, txt->valuestring, sizeof(s_state.condition));
    }
    if (cty && cJSON_IsString(cty)) {
        strlcpy(s_state.city, cty->valuestring, sizeof(s_state.city));
    }
    s_state.scene = weather_scene_from_code(s_state.weather_code, s_state.is_night);
    s_state.valid = true;
    s_state.from_cache = true;
    state_unlock();

    if (sr && ss && sd) {
        s_sun.have_sun    = true;
        s_sun.sunrise_min = (int)sr->valuedouble;
        s_sun.sunset_min  = (int)ss->valuedouble;
        s_sun.sun_ymd     = (int)sd->valuedouble;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "cache loaded: %d°C code=%d night=%d",
             s_state.temperature_c, s_state.weather_code, s_state.is_night);
    return true;
}

/* ── 解析 ── */
static bool parse_hhmm(const char *s, int *minutes)
{
    if (!s || strlen(s) < 4) return false;
    const char *colon = strchr(s, ':');
    if (!colon) return false;
    int h = atoi(s);
    int m = atoi(colon + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *minutes = h * 60 + m;
    return true;
}

/* URL 百分号编码（城市名等 UTF-8 字节必须编码；保留 unreserved 字符） */
static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p && o + 4 <= cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = '\0';
}

/* ── 拉取实况 + 当日日出日落（uapis.cn 单端点，ADR-020） ── */
static esp_err_t fetch_weather(void)
{
    /* 定位：wx_loc 空 = 按来源 IP 自动定位；全数字 = adcode；否则按城市名编码 */
    char url[400];
    int n = snprintf(url, sizeof(url), "https://%s%s", WEATHER_API_HOST, WEATHER_API_PATH);
    if (s_location[0]) {
        if (strspn(s_location, "0123456789") == strlen(s_location)) {
            n += snprintf(url + n, sizeof(url) - n, "?adcode=%s", s_location);
        } else {
            char enc[256];
            url_encode(s_location, enc, sizeof(enc));
            n += snprintf(url + n, sizeof(url) - n, "?city=%s", enc);
        }
    }
    snprintf(url + n, sizeof(url) - n, "%sforecast=true", s_location[0] ? "&" : "?");

    /* Keep the 4 KiB response off the weather task stack.  This task also
     * runs snprintf/cJSON/HTTPS setup in the same call chain; placing the
     * response here previously exhausted the 6144-byte stack after SNTP. */
    char *resp = malloc(RESP_CAP);
    if (!resp) return ESP_ERR_NO_MEM;

    esp_err_t err = http_get(url, s_api_key, resp, RESP_CAP);
    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        free(resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t out = ESP_FAIL;
    cJSON *weather = cJSON_GetObjectItem(root, "weather");       /* 现象文案（中文） */
    cJSON *icon    = cJSON_GetObjectItem(root, "weather_icon");  /* 图标码（和风同体系） */
    cJSON *temp    = cJSON_GetObjectItem(root, "temperature");   /* °C */
    cJSON *city    = cJSON_GetObjectItem(root, "city");
    if (!weather || !cJSON_IsString(weather) || !icon) {
        ESP_LOGW(TAG, "unexpected response: %.160s", resp);
        goto done;
    }

    int code = cJSON_IsString(icon) ? atoi(icon->valuestring)
             : cJSON_IsNumber(icon) ? (int)icon->valuedouble : 0;
    int temp_c = 0;
    if (cJSON_IsNumber(temp)) {
        temp_c = (int)(temp->valuedouble >= 0 ? temp->valuedouble + 0.5
                                              : temp->valuedouble - 0.5);
    } else if (temp && cJSON_IsString(temp)) {
        temp_c = atoi(temp->valuestring);
    }

    /* 当日日出日落取自 forecast[0]（校验日期为今天；缺失时沿用旧数据/本地回退） */
    time_t now_ts = time(NULL);
    struct tm tmv;
    localtime_r(&now_ts, &tmv);

    cJSON *fc = cJSON_GetObjectItem(root, "forecast");
    if (cJSON_IsArray(fc) && cJSON_GetArraySize(fc) > 0) {
        cJSON *d0   = cJSON_GetArrayItem(fc, 0);
        cJSON *date = cJSON_GetObjectItem(d0, "date");
        cJSON *sr   = cJSON_GetObjectItem(d0, "sunrise");
        cJSON *ss   = cJSON_GetObjectItem(d0, "sunset");
        /* date 期望格式 "YYYY-MM-DD"：分段数值比对，不构造字符串（避免截断告警） */
        bool date_ok = true;
        if (date && cJSON_IsString(date) && strlen(date->valuestring) >= 10) {
            date_ok = (atoi(date->valuestring) == tmv.tm_year + 1900 &&
                       atoi(date->valuestring + 5) == tmv.tm_mon + 1 &&
                       atoi(date->valuestring + 8) == tmv.tm_mday);
        }
        int sr_min = 0, ss_min = 0;
        if (date_ok && sr && ss && cJSON_IsString(sr) && cJSON_IsString(ss) &&
            parse_hhmm(sr->valuestring, &sr_min) && parse_hhmm(ss->valuestring, &ss_min)) {
            s_sun.have_sun    = true;
            s_sun.sunrise_min = sr_min;
            s_sun.sunset_min  = ss_min;
            s_sun.sun_ymd     = (tmv.tm_year + 1900) * 10000 +
                                (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
            ESP_LOGI(TAG, "sun: rise %02d:%02d set %02d:%02d",
                     sr_min / 60, sr_min % 60, ss_min / 60, ss_min % 60);
        }
    }

    state_lock();
    s_state.temperature_c = temp_c;
    s_state.weather_code  = code;
    strlcpy(s_state.condition, weather->valuestring, sizeof(s_state.condition));
    if (city && cJSON_IsString(city)) {
        strlcpy(s_state.city, city->valuestring, sizeof(s_state.city));
    }
    s_state.is_night   = compute_is_night(now_ts);
    s_state.scene      = weather_scene_from_code(code, s_state.is_night);
    s_state.valid      = true;
    s_state.from_cache = false;
    s_state.updated_at = now_ts;
    state_unlock();
    out = ESP_OK;

done:
    cJSON_Delete(root);
    free(resp);
    return out;
}

/* ── 任务 ── */
static void weather_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "weather service started (refresh %d s)", CONFIG_SMART_HOME_WEATHER_REFRESH_SEC);

    load_credentials();

    /* 1. 先用缓存点亮界面（无网络也能显示上次数据） */
    if (cache_load()) {
        publish();
    }

    /* 2. 等 Wi-Fi（轮询，不占用 wifi.c 的单槽回调） */
    int waited = 0;
    while (!s_stop && !wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited++;
        if (waited % 40 == 0) {
            ESP_LOGI(TAG, "waiting for Wi-Fi (%d s)...", waited / 2);
        }
    }
    if (s_stop) {
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi up after %d s", waited / 2);
    publish();   /* 让 UI 立刻更新 Wi-Fi 指示 */

    /* 3. 授时 */
    start_sntp();
    wait_for_time();

    /* 4. 循环刷新（指数退避） */
    int backoff = BACKOFF_MIN_S;

    while (!s_stop) {
        bool ok = false;

        if (s_api_key[0] && wifi_is_connected()) {
            esp_err_t e = fetch_weather();
            if (e == ESP_OK) {
                ok = true;
                cache_save();
                publish();
                ESP_LOGI(TAG, "weather updated: %s %d°C code=%d scene=%s night=%d city=%s",
                         s_state.condition, s_state.temperature_c, s_state.weather_code,
                         weather_scene_to_string(s_state.scene), s_state.is_night,
                         s_state.city);
            } else {
                ESP_LOGW(TAG, "weather fetch failed: %s", esp_err_to_name(e));
            }
        } else if (s_api_key[0] == '\0') {
            /* 无 Key：不请求，但仍周期性重算昼夜（本地时间可用时） */
            if (s_time_valid) {
                state_lock();
                s_state.is_night = compute_is_night(time(NULL));
                s_state.scene = weather_scene_from_code(s_state.weather_code, s_state.is_night);
                state_unlock();
                publish();
            }
        } else {
            ESP_LOGW(TAG, "Wi-Fi down, skip weather fetch");
        }

        int delay_s;
        if (ok) {
            backoff = BACKOFF_MIN_S;
            delay_s = CONFIG_SMART_HOME_WEATHER_REFRESH_SEC;
        } else {
            delay_s = backoff;
            backoff = (backoff * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff * 2;
        }

        /* 分段睡眠，便于响应 refresh_now / stop */
        for (int i = 0; i < delay_s * 2 && !s_stop; i++) {
            if (s_refresh_req) {
                s_refresh_req = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "weather service stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ── 公共接口 ── */
esp_err_t weather_service_start(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "already started");
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_sun, 0, sizeof(s_sun));
    s_state.scene = WEATHER_SCENE_UNKNOWN;
    s_stop = false;
    s_refresh_req = false;

    if (xTaskCreate(weather_task, "weather", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void weather_service_stop(void)
{
    if (!s_started) return;
    s_stop = true;
    for (int i = 0; i < 40 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_started = false;
}

#else  /* !CONFIG_SMART_HOME_WEATHER_ENABLED —— 隔离测试：不联网、不建任务 */

esp_err_t weather_service_start(void)
{
    ESP_LOGI(TAG, "weather service disabled by Kconfig — 仅本地时间/渐变背景");
    return ESP_OK;
}

void weather_service_stop(void) {}

void weather_service_get_state(weather_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->scene = WEATHER_SCENE_UNKNOWN;
}

void weather_service_register_cb(weather_update_cb_t cb, void *user)
{
    (void)cb;
    (void)user;
}

bool weather_service_time_valid(void) { return false; }

esp_err_t weather_service_refresh_now(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_SMART_HOME_WEATHER_ENABLED */
