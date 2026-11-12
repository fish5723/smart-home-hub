/**
 * @file app_monitor.c
 * @brief OLED 本地显示实现（200ms 刷新，5s 翻页）
 *
 * 来源：前序工程 APP/app_monitor.c（原为 200ms 整屏刷新 + 任务与页面函数各刷一次）
 * 本工程修改：
 *   - 阶段 1（T-11）：去掉重复刷屏（UpdateScreen 全工程只调一次）+ 加快照脏标记
 *   - 阶段 1 验收整改（报告 §2.2 第 7 项）：原实现每次刷新前都 ssd1306_Fill(Black)，
 *     而 Fill 会置 SSD1306_ForceFull，使 BSP 层的脏页增量机制永远走全刷分支
 *     （每次推满 1024B / 8 页，约 25ms 阻塞）。现改为**定宽写屏**
 *     （所有字段按固定宽度补空格），同页内刷新即可完整覆盖旧内容，
 *     于是只在「首次」与「翻页」时清屏，脏页比对真正生效，
 *     每次只发变化的 1~3 页（约 3~9ms）
 */
//
// Created by A on 2026/5/27.
//
// 阶段 1（T-11）修改：
//   - 仅当数据变化时才 ssd1306_Fill + UpdateScreen，静态画面跳过 I2C
//   - 页面切换时强制全量刷新
//

#include "display/app_monitor.h"
#include "bsp_ssd1306.h"
#include "stdio.h"
#include "sensor/app_air.h"
#include "sensor/app_soil.h"
#include "sensor/app_mq4.h"
#include "bsp_ssd1306_fonts.h"
#include "app_data.h"
#include "sensor/app_dht11.h"
#include <string.h>  /* memcmp */

static monitor_page_t g_monitor_page = MONITOR_PAGE_ENV;
//执行周期
#define MONITOR_PERIOD_MS 200U
//页面切换间隔(毫秒)
#define PAGE_SWITCH_INTERVAL_MS 5000U

/* T-11 整改：快照存"**将要显示的整数**"，而不是原始浮点值 ——
 * 原实现存 float pressure，而 BMP280 每秒更新且带噪声，memcmp 几乎每秒
 * 都不相等 → 脏标记形同虚设，实际退化成"每秒必刷"（验收报告 §2.3 T-11）。
 * 存显示值之后，只有"屏幕上真的会变"时才触发刷新。 */
typedef struct {
    uint8_t  soil;
    uint8_t  soil_usable;
    uint8_t  gas;
    uint8_t  gas_usable;
    uint8_t  air;
    uint8_t  air_usable;
    int16_t  temperature;      /* 与显示一致：取整 */
    int16_t  humidity;
    uint8_t  dht11_valid;
    uint8_t  rain;
    uint8_t  rain_usable;
    int32_t  pressure_kpa;     /* 与显示一致：显示的就是 kPa 整数 */
    uint8_t  pump_on;
    uint8_t  pump_alarm;
    uint8_t  lock_unlocked;
    monitor_page_t page;
} monitor_snapshot_t;

static monitor_snapshot_t s_last_snap;
static uint8_t s_snap_valid = 0;  /* 首次上电强制刷新 */

/** 某路数据当前是否"可用"（valid 且 quality==VALID）—— 不可用时显示 "--" */
static uint8_t usable(uint8_t valid, uint8_t quality)
{
    return (valid && quality == (uint8_t)SENSOR_QUAL_VALID) ? 1U : 0U;
}

/**
 * @brief 判断显示内容是否需要刷新
 * @return 1=需要刷新（数据或页面变化），0=不需要
 */
static uint8_t monitor_needs_update(void)
{
    monitor_snapshot_t now;
    memset(&now, 0, sizeof(now));

    now.soil          = g_sensor_data.soil;
    now.soil_usable   = usable(g_sensor_data.soil_valid, g_sensor_data.soil_quality);
    now.gas           = g_sensor_data.gas;
    now.gas_usable    = usable(g_sensor_data.gas_valid, g_sensor_data.gas_quality);
    now.air           = g_sensor_data.air;
    now.air_usable    = usable(g_sensor_data.air_valid, g_sensor_data.air_quality);
    now.temperature   = (int16_t)g_sensor_data.temperature;
    now.humidity      = (int16_t)g_sensor_data.humidity;
    now.dht11_valid   = g_sensor_data.dht11_valid;
    now.rain          = g_sensor_data.rain;
    now.rain_usable   = usable(g_sensor_data.rain_valid, g_sensor_data.rain_quality);
    now.pressure_kpa  = (int32_t)(g_sensor_data.pressure / 1000.0f);
    now.pump_on       = g_sensor_data.pump_on;
    now.pump_alarm    = g_sensor_data.pump_alarm;
    now.lock_unlocked = g_sensor_data.lock_unlocked;
    now.page          = g_monitor_page;

    if (!s_snap_valid) {
        s_last_snap = now;
        s_snap_valid = 1;
        return 1;
    }

    if (memcmp(&s_last_snap, &now, sizeof(now)) != 0) {
        s_last_snap = now;
        return 1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════
 *  定宽写屏（T-11 整改）
 *
 *  原实现每次刷新前都 ssd1306_Fill(Black)，而 Fill 会置 SSD1306_ForceFull，
 *  使 BSP 层精心写的脏页增量机制（SSD1306_LastSent 逐页比对）永远走全刷分支
 *  —— 每次推满 1024 B / 8 页，I2C 400kHz 下约 25 ms 阻塞主循环。
 *
 *  整改做法：把所有字段按固定宽度（不足补空格）写屏。这样同页内刷新时
 *  新内容会完整覆盖旧内容，**不需要清屏**；不清屏就不置 ForceFull，
 *  脏页比对随即生效，每次只发真正变化的那几页（通常 1~3 页，约 3~9 ms）。
 *
 *  清屏只在「首次」与「翻页」时做（翻页内容差异大，必须整体擦除）。
 * ════════════════════════════════════════════════════ */
#define MONITOR_COL_L_X   0U    /* 左列起点 */
#define MONITOR_COL_L_W   12U   /* 左列宽度（受 "TEMP: 25℃" 的 3 字节 UTF-8 影响需留 12） */
#define MONITOR_COL_R_X   62U   /* 右列起点（原为 70，"RAIN:100%" 9 字符在 70 处会溢出到 133px） */
#define MONITOR_COL_R_W   9U    /* 右列宽度 */

static void monitor_puts(uint8_t x, uint8_t y, const char *text, uint8_t width)
{
    char padded[24];
    size_t n = strlen(text);
    if (n > width) n = width;               /* 超宽则截断，绝不越界 */
    memcpy(padded, text, n);
    for (size_t i = n; i < width; i++) padded[i] = ' ';
    padded[width] = '\0';
    ssd1306_SetCursor(x, y);
    ssd1306_WriteString(padded, Font_7x10, White);
}

void APP_MONITOR_Task(void)
{
    static uint32_t last_tick = 0;
    static uint32_t last_switch_tick = 0;
    static uint8_t page_switch_inited = 0;
    static uint8_t need_clear = 1;      /* 首次与翻页后需要整屏清除 */

    if(HAL_GetTick() - last_tick < MONITOR_PERIOD_MS)
    {
        return;
    }
    last_tick = HAL_GetTick();

    /* 首次进入任务时对齐页面切换计时器，避免 HAL_GetTick() 远大于 0 导致立即切页 */
    if (!page_switch_inited) {
        last_switch_tick = HAL_GetTick();
        page_switch_inited = 1;
        printf("[OLED] init page=%d  tick=%lu\r\n",
               (int)g_monitor_page, (unsigned long)last_switch_tick);
    }

    //检查是否需要切换页面
    if(HAL_GetTick() - last_switch_tick >= PAGE_SWITCH_INTERVAL_MS)
    {
        APP_MONITOR_NextPage();
        last_switch_tick = HAL_GetTick();
        need_clear = 1;                 /* 翻页内容差异大，必须整屏清除 */
        printf("[OLED] switch to page=%d\r\n", (int)g_monitor_page);
    }

    /* T-11 整改：只有「数据/页面变化」或「需要清屏」时才动 I2C。
     * 不再每次刷新都打印（原为 1Hz 调试残留，见验收报告 T-14）。 */
    if (!monitor_needs_update() && !need_clear) {
        return;
    }

    if (need_clear) {
        ssd1306_Fill(Black);            /* 仅此处清屏：会置 ForceFull → 全刷一次 */
        need_clear = 0;
    }

    switch(g_monitor_page)
    {
        case MONITOR_PAGE_ENV:
        {
            APP_MONITOR_ShowEnvPage();
        }
            break;

        case MONITOR_PAGE_SYS:
        {
            APP_MONITOR_ShowSysPage();
        }
            break;

        default:
            break;
    }

    ssd1306_UpdateScreen();
}
void APP_MONITOR_NextPage(void)
{
    g_monitor_page++;

    if(g_monitor_page >= MONITOR_PAGE_MAX)
    {
        g_monitor_page =
                MONITOR_PAGE_ENV;
    }
}
 void APP_MONITOR_ShowEnvPage(void)
{
    char buf[32];

    /** 土壤 —— 不可用时显示 "--"：不要把超量程的 0% 当成"极干"展示给用户 */
    if (usable(g_sensor_data.soil_valid, g_sensor_data.soil_quality))
        snprintf(buf, sizeof(buf), "SOIL:%3u%%", (unsigned)g_sensor_data.soil);
    else
        snprintf(buf, sizeof(buf), "SOIL: -- ");
    monitor_puts(MONITOR_COL_L_X, 0, buf, MONITOR_COL_L_W);

    /** MQ-4 —— 标定完成前 quality=WARMUP，显示 "--" 而不是假浓度 */
    if (usable(g_sensor_data.gas_valid, g_sensor_data.gas_quality))
        snprintf(buf, sizeof(buf), "GAS :%3u%%", (unsigned)g_sensor_data.gas);
    else
        snprintf(buf, sizeof(buf), "GAS : -- ");
    monitor_puts(MONITOR_COL_L_X, 11, buf, MONITOR_COL_L_W);

    /** MQ-135 */
    if (usable(g_sensor_data.air_valid, g_sensor_data.air_quality))
        snprintf(buf, sizeof(buf), "AIR :%3u%%", (unsigned)g_sensor_data.air);
    else
        snprintf(buf, sizeof(buf), "AIR : -- ");
    monitor_puts(MONITOR_COL_L_X, 22, buf, MONITOR_COL_L_W);

    /** DHT11 */
    if (g_sensor_data.dht11_valid)
    {
        snprintf(buf, sizeof(buf), "TEMP:%3d℃", (int)g_sensor_data.temperature);
        monitor_puts(MONITOR_COL_L_X, 33, buf, MONITOR_COL_L_W);

        snprintf(buf, sizeof(buf), "HUMI:%3d%%", (int)g_sensor_data.humidity);
        monitor_puts(MONITOR_COL_L_X, 44, buf, MONITOR_COL_L_W);
    }
    else
    {
        monitor_puts(MONITOR_COL_L_X, 33, "TEMP:--", MONITOR_COL_L_W);
        monitor_puts(MONITOR_COL_L_X, 44, "HUMI:--", MONITOR_COL_L_W);
    }

    /** 雨滴 */
    if (usable(g_sensor_data.rain_valid, g_sensor_data.rain_quality))
        snprintf(buf, sizeof(buf), "RAIN:%3u%%", (unsigned)g_sensor_data.rain);
    else
        snprintf(buf, sizeof(buf), "RAIN: -- ");
    monitor_puts(MONITOR_COL_R_X, 0, buf, MONITOR_COL_R_W);

    /** 气压 */
    if (g_sensor_data.bmp280_valid)
        snprintf(buf, sizeof(buf), "P:%4dkPa", (int)(g_sensor_data.pressure / 1000.0f));
    else
        snprintf(buf, sizeof(buf), "P:  --  ");
    monitor_puts(MONITOR_COL_R_X, 11, buf, MONITOR_COL_R_W);
}
 void APP_MONITOR_ShowSysPage(void)
{
    char buf[32];

    /** WIFI*/
    monitor_puts(MONITOR_COL_L_X, 0, "WIFI:OFF", MONITOR_COL_L_W);

    /** CLOUD*/
    monitor_puts(MONITOR_COL_L_X, 22, "CLOUD:OFF", MONITOR_COL_L_W);

    /** SYS*/
    monitor_puts(MONITOR_COL_L_X, 44, "SYS :OK", MONITOR_COL_L_W);

    /** PUMP：优先显示锁存的干烧/超时告警（T-18 后告警需灌溉需求消失才解除）*/
    snprintf(buf, sizeof(buf), "PUMP:%s", g_sensor_data.pump_alarm ? "ALM"
                                       : (g_sensor_data.pump_on ? "ON" : "OFF"));
    monitor_puts(MONITOR_COL_R_X, 0, buf, MONITOR_COL_R_W);

    /** LOCK*/
    snprintf(buf, sizeof(buf), "LOCK:%s", g_sensor_data.lock_unlocked ? "OPEN" : "LOCK");
    monitor_puts(MONITOR_COL_R_X, 22, buf, MONITOR_COL_R_W);
}