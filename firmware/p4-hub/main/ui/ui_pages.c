/*
 * ui_pages.c — 六页硬件式 UI + Product Rail 导航（LVGL）
 *
 * 与 ui/prototype/stage4-ui.html（v2 状态矩阵版）1:1 对应：
 *   rail(18,18,88,564) + screen(106,18,900,564)；六个 page 与两个 subpage
 *   都是 screen 的子对象，页面内坐标为【面板内】相对坐标。
 *
 * 三条纪律（对应阶段四"不要空有皮囊"的要求）：
 *   1. 文案走 copy 表（zh/en 双份），屏上每个词都能被语言开关整体切换；
 *   2. 显示内容由数据状态决定：链路三态 × 传感器质量五态 × 等级阶梯 × 房间画像
 *      → 词 / 颜色 / 可点性，全部收敛在 live_refresh_cb 的矩阵里；
 *   3. 没有数据源就不编数值：离线/陈旧/无快照一律显示词或 "--"。
 *
 * 中文字形由本文件与 standby_ui.c 的字符串字面量自动派生：
 * 新增文案后必须重跑 node tools/generate_runtime_fonts.mjs 再上板。
 */
#include "ui_pages.h"
#include "ui_tokens.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "font/lv_binfont_loader.h"
#include "images/reicon_nav_assets.h"
#include "app_proto.h"
#include "rule_runtime.h"
#include "protocol.h"
#include "esp_timer.h"
#include "storage.h"
#include "wifi.h"
#include "net/remote_node.h"
#include "ota/f103_iap.h"
#include "ota/net_ota.h"
LV_FONT_DECLARE(ui_font_cjk_18)
LV_FONT_DECLARE(ui_font_cjk_24)
LV_FONT_DECLARE(ui_font_cjk_48)

enum { PAGE_OVERVIEW, PAGE_ENVIRONMENT, PAGE_DEVICES, PAGE_AUTOMATION, PAGE_ALERTS, PAGE_SETTINGS, PAGE_COUNT };
enum { SUB_NETWORK, SUB_DISPLAY, SUB_DEVICES, SUB_COUNT };

/* ── 调色板（与 ui/prototype CSS 变量一致） ── */
static const uint32_t RAIL=0x20201E, SURFACE=0xE9E5DC, ALT=0xDED8CE, INK=0x262522, MUTED=0x777269, MDARK=0xAAA39A, CREAM=0xF3EFE7, METAL=0xC69A59, HAIR=0xC9C1B5;
/* 等级胶囊底色 = 半透明强调色叠在 ALT 上的近似实色 */
static const uint32_t TINT_OK_BG=0xBCD5BA, TINT_MID_BG=0xD9CCB7, TINT_BAD_BG=0xE1C0B8;
static const uint32_t FG_OK=0x1E6B3A, FG_MID=0x8A6420, FG_BAD=0x96231F;
static const uint32_t DANGER=0xEF4444, DARK_HAIR=0x56534D, ALERT_BG=0x2A1517, NOTE_BAD=0x8A3B36;
static const uint32_t CHART_GRID=0x3A3A38;   /* 原型 #3A3A38 */
static const uint32_t SOIL_INK=0xA87637;     /* 原型土壤值强调色 */

/* 温度趋势环缓冲：24 点 × 30s ≈ 最近 12 分钟（本机无持久历史，故不写"24 小时"） */
#define TREND_POINTS   24
#define TREND_PERIOD_S 30
/* 网络扫描列表行池：与扫描容量一致，一次建好，靠容器滚动浏览 */
#define WF_ROWS        WIFI_SCAN_MAX_AP

/* ── 控件指针 ── */
typedef struct { lv_obj_t *root, *name, *desc, *state; int key; } device_view_t;
static device_view_t device_views[8];
static unsigned device_view_count;

static lv_obj_t *root, *screen, *rail, *pages[PAGE_COUNT], *subs[SUB_COUNT], *nav[PAGE_COUNT];
static lv_obj_t *modal, *modal_title, *modal_body, *toast, *lang_switch;
static lv_obj_t *scene_names[3], *scene_descs[3], *scene_states[3], *scene_switches[3];
static lv_obj_t *wf_list;
static lv_obj_t *wf_row_box[WF_ROWS], *wf_row_ssid[WF_ROWS], *wf_row_state[WF_ROWS], *wf_row_note[WF_ROWS];
static lv_obj_t *wf_status, *wf_scan_btn, *wf_scan_label;
static lv_obj_t *dp_idle_value, *dp_mute_switch, *dp_bright_value, *dp_vol_value;
static lv_obj_t *live_status;
static lv_obj_t *live_temp, *live_hum, *live_press;
static lv_obj_t *live_sig_hist[3], *live_sig_dot[3];
static lv_obj_t *live_env[5], *live_ind;
static lv_obj_t *live_profile, *live_profile_title;
static lv_obj_t *live_alert_main, *live_alert_sub, *live_alert_count, *live_alert_banner;
static lv_obj_t *live_soil_alert, *live_soil_alert_value;
static lv_obj_t *live_setting_network, *live_setting_devices, *live_system_health;
static lv_obj_t *live_f103_iap_status;
static lv_obj_t *live_net_ota_status;
static lv_obj_t *net_ota_btn, *net_ota_btn_label;
static lv_obj_t *live_rule_status;
static lv_obj_t *live_chart; static lv_chart_series_t *live_chart_ser;
static lv_obj_t *live_chart_window;

static float s_trend[TREND_POINTS];
static int   s_trend_n;
static int   s_trend_tick;

static const lv_font_t *F10,*F12,*F14,*F18,*F28,*F48;
static int page=0, sub=-1;
static bool dev_on[3]={false,false,false}, dev_known[3]={false,false,false};
static bool scene_on[3]={false,false,false};
static const lv_font_t *runtime_cn14, *runtime_cn16, *runtime_cn18, *runtime_cn24, *runtime_cn32, *runtime_cn48, *runtime_latin;
static uint8_t *runtime_font_data[6];
static lv_font_t *runtime_font_obj[6];
static const char *TAG = "UI_FONT";
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
extern const lv_font_t lv_font_source_han_sans_sc_14_cjk;
#endif
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
extern const lv_font_t lv_font_source_han_sans_sc_16_cjk;
#endif

/* ════════════════════════════════════════════════════
 *  1. 文案表（zh / en）—— 屏上文字的唯一来源
 * ════════════════════════════════════════════════════ */
typedef struct { const char *zh, *en; } cpy_t;
enum {
    K_OV_EYEBROW,K_OV_TITLE,K_OV_NODES,K_OV_CLIMATE,K_OV_TEMPNOTE,K_OV_HUM,K_OV_PRESS,
    K_OV_SIGNALS,K_OV_SIGTITLE,K_OV_ONETOUCH,K_DEV_LIGHT,K_DEV_PUMP,
    K_SIG_AIR,K_SIG_GAS,K_SIG_HUMAN,
    K_EN_EYEBROW,K_EN_TITLE,K_EN_SUB,K_EN_PROFILE,K_EN_NODATA,K_EN_TREND,K_EN_WIN_A,K_EN_WIN_B,K_EN_READINGS,K_EN_LIVE,
    K_S_TEMP,K_S_HUM,K_S_PRESS,K_S_SOIL,K_S_AIR,
    K_PF_COMFORT,K_PF_DRY,K_PF_HUMID,K_PF_HOT,K_PF_COLD,K_PF_MUGGY,K_PF_NODATA,
    K_FA_COMFORT,K_FA_DRY,K_FA_HUMID,K_FA_HOT,K_FA_COLD,K_FA_MUGGY,K_FA_NODATA,
    K_DV_EYEBROW,K_DV_TITLE,K_DV_SUB,K_DV_COUNT,K_DV_LIGHT_NOTE,K_DV_PUMP_NOTE,K_DV_SAFETY,
    K_DV_LANG,K_DV_LANG_NOTE,K_SF_LOCK,K_SF_BEEP,K_SF_GUARD,K_SV_NA,K_SV_MUTE,K_SV_ARMED,K_SV_CONFIRM,
    K_AU_EYEBROW,K_AU_TITLE,K_AU_ADD,K_AU_ENGINE,K_AU_PENDING,
    K_RULE1,K_RULE1D,K_RULE2,K_RULE2D,K_RULE3,K_RULE3D,
    K_RS_ACTIVE,K_RS_PAUSED,K_RM_WAIT,K_RM_ACTIVE,K_RM_OFF,K_RM_PAUSED,
    K_AL_EYEBROW,K_AL_TITLE,K_AL_SUB,K_AL_VIEW,K_AL_SOIL,K_AL_PRIV,K_AL_INFO,K_AL_NOTE,
    K_AC_NONE,K_AC_ALERT,K_AC_CLEAR,K_AL_NONE,K_AL_NONE_SUB,K_AL_KIND_GAS,K_AL_CLEARED,K_AL_TRIGGER,K_AL_THR,
    K_LV_URGENT,K_LV_WARN,K_LV_HINT,
    K_ST_EYEBROW,K_ST_TITLE,K_ST_SUB,K_ST_PREFS,K_SET1,K_SET2,K_SET3,K_SET4,K_SET2_DESC,K_SET4_DESC,K_SH_OK,K_SH_DEG,K_SH_OFF,K_PG_BACK,
    K_WF_EYEBROW,K_WF_TITLE,K_WF_HINT,K_WF_FEAS,K_WF_SCAN,K_WF_SAVED,K_WF_CONN,K_WF_FAIL,K_WF_BARS,
    K_WF_CUR_OK,K_WF_CUR_NONE,K_WF_OPEN,K_WF_LOCKED,K_WF_SCANNING,K_WF_NOAP,K_WF_TAP_HINT,
    K_WF_SIG_STRONG,K_WF_SIG_GOOD,K_WF_SIG_FAIR,K_WF_SIG_WEAK,
    K_DP_EYEBROW,K_DP_TITLE,K_DP_SUB,K_DP_FEAS,K_DP_BRIGHT,K_DP_BRIGHT_NOTE,K_DP_BRIGHT_VAL,
    K_DP_VOL,K_DP_VOL_NOTE,K_DP_MUTE,K_DP_MUTE_NOTE,K_DP_IDLE,K_DP_IDLE_NOTE,K_DP_SEC,K_DP_CYCLE,
    K_TO_SENT,K_TO_OFF,K_TO_BUSY,K_TO_RO,K_TO_SAVED_ON,K_TO_SAVED_OFF,K_TO_SAVE_FAIL,K_TO_SAVED_PAUSED,
    K_W_LINK_UP,K_W_LINK_DEG,K_W_LINK_DOWN,
    K_W_Q_WARMUP,K_W_Q_OVER,K_W_Q_FAULT,K_W_Q_STALE,
    K_W_ACT_ON,K_W_ACT_OFF,K_W_ACT_UNKNOWN,K_W_ACT_GUARD,
    K_W_A_OK,K_W_A_MID,K_W_A_LOW,K_W_A_BAD,
    K_W_H_YES,K_W_H_NO,K_W_RAIN_YES,K_W_RAIN_NO,K_W_PUMP_ALARM,K_W_PUMP_OK,
    K_MD_TITLE,K_MD_OK,K_MD_CANCEL,K_MD_NONE,K_MD_GAS,K_MD_RULE_TITLE,K_MD_RULE,
    K_SD_EYEBROW,K_SD_TITLE,K_SD_SUB,K_SD_FW_TITLE,K_SD_FW_DESC,K_SD_FW_UPGRADE,
    K_SD_FW_RUNNING,K_SD_FW_SUCCESS,K_SD_FW_FAILED,K_SD_FW_OFFLINE,K_SD_FW_READY,
    K_SD_FW_CONFIRM_TITLE,K_SD_FW_CONFIRM_BODY,
    K_SD_NET_TITLE,K_SD_NET_DESC,K_SD_NET_CHECK,K_SD_NET_CHECKING,K_SD_NET_DOWNLOADING,
    K_SD_NET_UP_TO_DATE,K_SD_NET_FAILED,K_SD_NET_OFFLINE,K_COUNT
};
static const cpy_t s_copy[K_COUNT] = {
    {"SMART HOME / 概览","SMART HOME / OVERVIEW"},{"家 / 今日","Home / Today"},{"P4 / F103 / C3","P4 / F103 / C3"},
    {"室内气候","INDOOR CLIMATE"},{"室内温度 / BMP280","Indoor temp / BMP280"},{"湿度","Humidity"},{"气压 hPa","Pressure hPa"},
    {"信号 / 03","SIGNALS / 03"},{"室内信号","Indoor signals"},{"设备控制","ACTUATORS"},{"灯光","Light"},{"水泵","Pump"},
    {"空气质量","Air quality"},{"可燃气","Combustible gas"},{"人体","Presence"},
    {"SMART HOME / 环境","SMART HOME / ENVIRONMENT"},{"环境 · 曲线","Environment · Trends"},
    {"读数、来源、趋势，保持同一视觉节奏","Readings, sources and trends in one rhythm"},
    {"房间画像 / 客厅","ROOM PROFILE / LIVING"},{"数据不足","Not enough data"},{"温度趋势","TEMP TREND"},
    {"近","last"},{"分","min"},{"实时读数 / 07","SENSOR READINGS / 07"},{"实时读数","Live readings"},
    {"温度","Temp"},{"湿度","Humidity"},{"气压","Pressure"},{"土壤","Soil"},{"空气 / 可燃气 / 雨滴","Air / Gas / Rain"},
    {"舒适","Comfort"},{"偏干","Dry"},{"偏湿","Humid"},{"偏热","Warm"},{"偏冷","Cool"},{"闷热","Muggy"},{"数据不足","Not enough data"},
    {"温湿度都在舒适区，无需处理","Temperature and humidity are in the comfort zone"},
    {"湿度偏低 · 建议加湿","Low humidity · consider humidifying"},
    {"湿度偏高 · 建议除湿或通风","High humidity · dehumidify or ventilate"},
    {"温度偏高 · 建议通风降温","Warm · ventilate to cool down"},
    {"温度偏低 · 建议提高室温","Cool · raise the room temperature"},
    {"又热又湿 · 建议开窗通风","Hot and humid · open a window"},
    {"数据不可用 · 等待传感器恢复","Data unavailable · wait for sensors"},
    {"SMART HOME / 设备","SMART HOME / DEVICES"},{"设备 · 触感","Devices · Touch"},
    {"命令不是状态，回读确认才是状态","A command is not a state — read-back is"},{"3 路执行器","3 ACTUATORS"},
    {"继电器 1 · 已回读","Relay 1 · read back"},{"继电器 2 · 安全上限 10 秒","Relay 2 · 10 s safety limit"},{"安全 / 03","SAFETY / 03"},
    {"界面语言","Interface language"},{"中文 / English（随系统持久化）","Chinese / English (persisted)"},
    {"门锁（未接入）","Lock (not fitted)"},{"蜂鸣器","Buzzer"},{"执行保护","Execution guard"},
    {"不可用","N/A"},{"静音","Mute"},{"开启","Armed"},{"二次确认","Confirm"},
    {"SMART HOME / 自动化","SMART HOME / AUTOMATION"},{"自动化 / 安静地运行","Automation · Runs quietly"},{"+ 规则","+ Rule"},
    {"本地规则引擎","LOCAL RULE ENGINE"},
    {"本页保持只读 + 开/关；规则编辑走 P4 自带本地配置端点（HTTP/WS）直接写 NVS 双槽 r0/r1，协议层无需扩展。",
     "This page stays read-only + on/off; rule editing uses P4's own local config endpoint (HTTP/WS) writing the NVS slots r0/r1 — no protocol change."},
    {"回家","Arrive"},{"PIR + 人脸确认 / 开灯","PIR + face check / light on"},
    {"智能灌溉","Smart watering"},{"土壤 < 35% 且未下雨 / 水泵 6 秒","Soil < 35% and dry / pump 6 s"},
    {"夜间守护","Quiet night"},{"本地静音保护 / 关闭蜂鸣器","Local mute / buzzer off"},
    {"规则运行中 / 本地执行","RULES ACTIVE / LOCAL"},{"规则暂停 / 链路离线","RULES PAUSED / OFFLINE"},
    {"等待触发","Standing by"},{"运行中","Active"},{"已停用","Off"},{"已暂停","Paused"},
    {"SMART HOME / 告警","SMART HOME / ALERTS"},{"告警 · 需要你知道的事","Alerts · What matters"},
    {"少打扰，但不漏掉关键风险","Few nudges, no missed risk"},{"查看","Detail"},
    {"土壤湿度 / 灌溉判定","Soil moisture / irrigation"},{"摄像头隐私模式已开启","Camera privacy mode on"},{"信息","Info"},
    {"告警语义：安全动作（蜂鸣/关断）在 F103 本地完成，本页只反映 ALARM_EVENT 与数据恢复。",
     "Semantics: safety actions run locally on F103; this page only reflects ALARM_EVENT and data recovery."},
    {"正常","NORMAL"},{"1 条告警","1 ALERT"},{"已恢复","RECOVERED"},
    {"当前无告警","NO ACTIVE ALERT"},{"可燃气 / 空气质量均在阈值内","Gas and air quality within limits"},
    {"可燃气超标 · MQ-4","Gas above limit · MQ-4"},{"告警已解除 · 数值已恢复","Alert cleared · value recovered"},
    {"触发值","value"},{"阈值","threshold"},
    {"紧急","Urgent"},{"警告","Warning"},{"提示","Hint"},
    {"SMART HOME / 系统","SMART HOME / SYSTEM"},{"设置 · 系统偏好","Settings · Preferences"},
    {"像调节一件实体产品，而不是维护后台","Tune it like a product, not a backend"},{"偏好","PREFERENCES"},
    {"网络连接","Network"},{"屏幕与声音","Display & sound"},{"设备与标定","Devices & calibration"},{"隐私与摄像头","Privacy & camera"},
    {"面板背光 · 待机超时","Backlight · standby timeout"},{"端侧处理 · 默认不上云","On-device · no cloud"},
    {"系统健康 · 正常","SYSTEM HEALTH · OK"},{"系统健康 · 链路降级","SYSTEM HEALTH · DEGRADED"},{"系统健康 · 链路离线","SYSTEM HEALTH · LINK DOWN"},
    {"返回","Back"},
    {"设置 / 网络连接","SETTINGS / NETWORK"},{"网络连接","Network"},{"点击网络输入密码","Tap a network to enter the password"},
    {"可行性：列表实时来自 esp_wifi 扫描；开放网络可直接连接，加密网络沿用网页配网输入密码，屏上不内置键盘。",
     "Feasibility: the list comes from a live esp_wifi scan; open networks connect here, secured ones use the web provisioning path (no on-screen keyboard)."},
    {"重新扫描","Rescan"},{"已保存","Saved"},{"连接中","Connecting"},{"连接失败","Failed"},{"信号","Signal"},
    {"已连接","Connected"},{"未连接","Not connected"},{"开放","Open"},{"需密码","Secured"},{"扫描中","Scanning"},
    {"未发现网络","No network found"},{"点击开放网络可直接连接","Tap an open network to connect"},
    {"强","Strong"},{"良好","Good"},{"一般","Fair"},{"弱","Weak"},
    {"设置 / 屏幕与声音","SETTINGS / DISPLAY & SOUND"},{"屏幕与声音","Display & sound"},
    {"亮度、音量、待机超时——都是实机可调的项","Brightness, volume and standby timeout are all real device settings"},
    {"可行性：亮度/音量取决于硬件能力（见下）；待机超时写 NVS 后由待机层读取生效。",
     "Feasibility: brightness/volume depend on hardware (see below); the standby timeout is written to NVS and honoured by the standby layer."},
    {"面板背光","Panel backlight"},{"硬件仅支持开/关，无调光 PWM","Hardware is on/off only — no dimming PWM"},{"常亮","Always on"},
    {"音量","Volume"},{"无外放音频硬件","No audio hardware fitted"},
    {"蜂鸣器提示音","Buzzer prompts"},{"与自动化「夜间守护」同一项偏好","Same preference as automation Quiet night"},
    {"待机超时","Standby timeout"},{"空闲后进入待机层","Enter standby when idle"},{"秒","s"},{"点击切换","Tap to cycle"},
    {"已发送 · 连接","Sent · Link"},{"F103 离线 / 未发送","F103 offline / not sent"},
    {"正在读取设备状态","Reading device state"},{"只读页 · 无操作","Read-only page"},
    {"已启用 · 已保存","Enabled · saved"},{"已停用 · 已保存","Disabled · saved"},{"规则保存失败","Rule save failed"},
    {"已保存 · 链路恢复后生效","Saved · applies when the link is back"},
    {"在线","Online"},{"降级","Degraded"},{"离线","Offline"},
    {"预热中","Warming up"},{"超量程","Over range"},{"故障","Fault"},{"陈旧","Stale"},
    {"运行中","On"},{"已关闭","Off"},{"未知","Unknown"},{"已保护","Guarded"},
    {"优","Good"},{"良","Fair"},{"中","Moderate"},{"差","Poor"},
    {"有人","Present"},{"无人","No motion"},{"下雨","Raining"},{"未下雨","Dry"},{"干烧保护","Dry-run guard"},{"正常","Normal"},
    {"告警详情","Alert detail"},{"确认","Confirm"},{"取消","Cancel"},
    {"当前没有需要处理的告警。","No alert needs attention right now."},
    {"可燃气 MQ-4 读数超过阈值，已由 F103 本地蜂鸣提醒；建议开窗通风并确认气源。",
     "MQ-4 is above the threshold; the local buzzer is sounding. Ventilate and check the gas source."},
    {"规则编辑","Rule editing"},
    {"规则的编辑入口已定案：由 P4 提供本地配置端点（HTTP/WS）写 NVS 双槽 r0/r1，网页端负责编辑、设备端负责执行。屏上保持只读 + 开/关。",
     "Rule editing is settled: P4 exposes a local config endpoint (HTTP/WS) that writes the NVS slots r0/r1 — the web UI edits, the device executes. This screen stays read-only + on/off."},
    {"设置 / 设备与标定","SETTINGS / DEVICES & CALIBRATION"},
    {"设备与标定","Devices & calibration"},
    {"F103 固件与设备状态","F103 firmware and device status"},
    {"F103 固件升级","F103 firmware update"},
    {"镜像：/sdcard/ota/f103-node.bin","Image: /sdcard/ota/f103-node.bin"},
    {"升级","Update"},
    {"升级中","Updating"},
    {"升级成功，F103 重启中","Update accepted · F103 rebooting"},
    {"升级失败，查看串口日志","Update failed · check serial log"},
    {"F103 离线，无法升级","F103 offline · cannot update"},
    {"就绪","Ready"},
    {"升级 F103 固件？","Update F103 firmware?"},
    {"升级期间传感器数据暂停，F103 会自动重启。","Sensor reports pause during the update; F103 reboots automatically."},
    {"P4 系统更新","P4 system update"},
    {"从服务器检查并下载固件","Check and download firmware from the server"},
    {"检查更新","Check"},
    {"检查中","Checking"},
    {"下载中","Downloading"},
    {"已是最新","Up to date"},
    {"网络升级失败","Network update failed"},
    {"Wi-Fi 未连接","Wi-Fi not connected"},
};
static bool s_zh = true;
static const char *T(int k){ return (k>=0 && k<K_COUNT) ? (s_zh ? s_copy[k].zh : s_copy[k].en) : ""; }

/* ── 需要随语言切换重建文案的 label 注册表 ── */
typedef struct { lv_obj_t *obj; uint16_t key; } txtreg_t;
static txtreg_t s_txt[160];
static unsigned s_txt_n;

/* ── 前置声明 ── */
static void modal_close(void);
static void modal_open(const char*t,const char*b);
static void dev_sync(const device_view_t *view);
static void dev_sync_all(int k);
static void show_page_objs(int i);
static void apply_lang(void);
static void live_refresh_cb(lv_timer_t *t);
static void lang_switch_cb(lv_event_t *e);
static void scene_cb(lv_event_t *e);
static void scene_sync(int i);

static const lv_font_t *fallback_cn14(void){
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#else
    /* 即使 SD BinFont 缺失也保持中文可见；18px 内置字库是有意的折中，不是豆腐块回退。 */
    return &ui_font_cjk_18;
#endif
}
static const lv_font_t *fallback_cn16(void){
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#else
    return &ui_font_cjk_18;
#endif
}
static void set_fallbacks(void){
    const lv_font_t *latin = runtime_latin ? runtime_latin : &lv_font_montserrat_14;
    const lv_font_t *fonts[6] = {runtime_cn14, runtime_cn16, runtime_cn18, runtime_cn24, runtime_cn32, runtime_cn48};
    const lv_font_t *cjk_fallback[6] = {
        fallback_cn14(), fallback_cn16(), &ui_font_cjk_18, &ui_font_cjk_24,
        &ui_font_cjk_24, &ui_font_cjk_48
    };
    /* SD BinFont 是有意做小的子集；缺字形先链回内置 CJK，再回拉丁，
     * 这样新加的状态词在旧子集字体上也只会回退，不会显示方框。 */
    for(int i=0;i<6;i++) if(fonts[i])
        ((lv_font_t *)fonts[i])->fallback = cjk_fallback[i] ? cjk_fallback[i] : latin;
}

/* ════════════════════════════════════════════════════
 *  2. 基础绘制 helper（暖纸色外壳沿用阶段三）
 * ════════════════════════════════════════════════════ */
static lv_obj_t *box(lv_obj_t *p,int x,int y,int w,int h,uint32_t c,int r){lv_obj_t*o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_bg_color(o,lv_color_hex(c),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_radius(o,r,0);lv_obj_set_style_pad_all(o,0,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;}
static lv_obj_t *txt(lv_obj_t*p,int x,int y,int w,int h,const char*s,const lv_font_t*f,uint32_t c){lv_obj_t*o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_label_set_text(o,s);return o;}
static lv_obj_t *click(lv_obj_t*p,int x,int y,int w,int h,uint32_t c,int r){lv_obj_t*o=box(p,x,y,w,h,c,r);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;}
static void line(lv_obj_t*p,int x,int y,int w){box(p,x,y,w,1,HAIR,0);}
static void setv(lv_obj_t*o,const char*s){if(o)lv_label_set_text(o,s);}
static void align_right(lv_obj_t*o){if(o)lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_RIGHT,0);}
static void align_center(lv_obj_t*o){if(o)lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);}
static void txt_put(lv_obj_t *o,uint32_t color,const char *s){ if(!o)return; lv_label_set_text(o,s); lv_obj_set_style_text_color(o,lv_color_hex(color),0); }

/* 注册一位随语言切换的 label；apply_lang 统一重设文案。 */
static lv_obj_t *tlab(lv_obj_t*p,int x,int y,int w,int h,int key,const lv_font_t*f,uint32_t c){
    lv_obj_t *o=txt(p,x,y,w,h,T(key),f,c);
    if(s_txt_n<sizeof(s_txt)/sizeof(s_txt[0])){ s_txt[s_txt_n].obj=o; s_txt[s_txt_n].key=(uint16_t)key; s_txt_n++; }
    return o;
}

/* 等级胶囊：小圆角容器 + 1px 描边 + 底色（原型 .tier / .t-ok / .t-mid / .t-bad） */
typedef struct { lv_obj_t *box,*lab; } pill_t;
static pill_t pill_mk(lv_obj_t *p,int x,int y,int w){
    pill_t t; t.box=box(p,x,y,w,26,ALT,8);
    lv_obj_set_style_border_width(t.box,1,0);
    lv_obj_set_style_border_color(t.box,lv_color_hex(HAIR),0);
    lv_obj_set_style_border_opa(t.box,LV_OPA_COVER,0);
    t.lab=txt(t.box,0,3,w,20,"",F14,MUTED); align_center(t.lab);
    return t;
}
static void pill_set(const pill_t *t,const char *s,uint32_t fg,uint32_t bg,uint32_t bd){
    if(!t||!t->box)return;
    setv(t->lab,s);
    lv_obj_set_style_text_color(t->lab,lv_color_hex(fg),0);
    lv_obj_set_style_bg_color(t->box,lv_color_hex(bg),0);
    lv_obj_set_style_border_color(t->box,lv_color_hex(bd),0);
}
enum { TIER_OK, TIER_MID, TIER_BAD, TIER_NONE };
static void pill_tier(const pill_t *t,const char *s,int tier){
    switch(tier){
        case TIER_OK:   pill_set(t,s,FG_OK,TINT_OK_BG,0x22C55E); break;
        case TIER_MID:  pill_set(t,s,FG_MID,TINT_MID_BG,METAL); break;
        case TIER_BAD:  pill_set(t,s,FG_BAD,TINT_BAD_BG,DANGER); break;
        default:        pill_set(t,s,MUTED,ALT,HAIR); break;
    }
}
static pill_t live_sig_pill[3];

/* 开关：视觉 44×24，命中区按 TK_TOUCH_MIN 扩到 44×44 */
static lv_obj_t *switch_mk(lv_obj_t *p,int x,int y,bool on,lv_event_cb_t cb,void *ud){
    const int padx=(TK_TOUCH_MIN-TK_TOGGLE_W)/2, pady=(TK_TOUCH_MIN-TK_TOGGLE_H)/2;
    lv_obj_t *hit=click(p,x-padx,y-pady,TK_TOUCH_MIN,TK_TOUCH_MIN,0,0);
    lv_obj_set_style_bg_opa(hit,LV_OPA_TRANSP,0);
    lv_obj_t *sw=box(hit,padx,pady,TK_TOGGLE_W,TK_TOGGLE_H,on?METAL:HAIR,TK_TOGGLE_H/2);
    lv_obj_t *knob=box(sw,on?22:2,2,20,20,SURFACE,10);
    /* box() 建出来的对象默认带 LV_OBJ_FLAG_CLICKABLE（见 box/click 实现），
     * 而 LVGL 的命中判定只看该标志 + 区域（lv_obj_pos.c lv_obj_hit_test），
     * 于是点开关“本体”会命中 sw/knob 这两个没有回调的子对象 → 事件被吞掉，
     * 只有点到本体上下各 10px 的透明边条才会落到 hit 上。表现为“有时按了没反应”。
     * 关掉子对象的命中，让整个 TK_TOUCH_MIN×TK_TOUCH_MIN 都归 hit 处理。 */
    lv_obj_remove_flag(sw,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(knob,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(hit,sw);
    if(cb) lv_obj_add_event_cb(hit,cb,LV_EVENT_CLICKED,ud);
    return hit;
}
static void switch_set(lv_obj_t *hit,bool on){
    if(!hit)return;
    lv_obj_t *sw=(lv_obj_t*)lv_obj_get_user_data(hit);
    if(!sw)return;
    lv_obj_set_style_bg_color(sw,lv_color_hex(on?METAL:HAIR),0);
    lv_obj_t *knob=lv_obj_get_child(sw,0);
    if(knob) lv_obj_set_x(knob,on?22:2);
}

/* ════════════════════════════════════════════════════
 *  3. 语言状态（持久化在 NVS storage 命名空间）
 * ════════════════════════════════════════════════════ */
static void lang_load(void){
    char buf[8]; size_t len=sizeof(buf);
    if(storage_get_str("ui_lang",buf,&len)==ESP_OK && buf[0]) s_zh = (buf[0]!='e');
    else s_zh = true;
}
static void lang_save(void){ storage_set_str("ui_lang", s_zh ? "zh" : "en"); }
static void apply_lang(void){
    for(unsigned i=0;i<s_txt_n;i++) setv(s_txt[i].obj, T((int)s_txt[i].key));
    if(lang_switch) switch_set(lang_switch, s_zh);
    for(int i=0;i<3;i++) scene_sync(i);
    /* 动态词（值/状态/等级/画像/告警）也要按新语言重算一次 */
    for(unsigned i=0;i<device_view_count;i++) dev_sync(&device_views[i]);
}

/* ════════════════════════════════════════════════════
 *  4. 弹窗 / Toast
 * ════════════════════════════════════════════════════ */
static lv_timer_t *toast_timer;
static void toast_hide_timer(lv_timer_t *t){
    if (toast) lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN);
    /* 定时器回调后自动删除；不要保留一个后续按钮会二次删除的悬空指针。 */
    if (toast_timer == t) toast_timer = NULL;
}
static void toast_show(const char*s){
    if(!toast)return;
    setv(lv_obj_get_child(toast,0),s);
    lv_obj_clear_flag(toast,LV_OBJ_FLAG_HIDDEN);
    if(toast_timer){lv_timer_delete(toast_timer); toast_timer=NULL;}
    toast_timer=lv_timer_create(toast_hide_timer,2400,NULL);
    if(!toast_timer) return;
    lv_timer_set_repeat_count(toast_timer,1);
    lv_timer_set_auto_delete(toast_timer,true);
}
static void toast_key_cb(lv_event_t *e){toast_show(T((int)(intptr_t)lv_event_get_user_data(e)));}
/* 现有弹窗的确认按钮原本写死为"开水泵"。F103 升级复用同一弹窗，
 * 用动作标签区分，避免在 UI 里再建一套确认层。 */
enum { MODAL_ACT_PUMP = 0, MODAL_ACT_F103_IAP };
static int s_modal_action = MODAL_ACT_PUMP;
static void modal_close(void){lv_obj_add_flag(modal,LV_OBJ_FLAG_HIDDEN);}
static void modal_open(const char*t,const char*b){setv(modal_title,t);setv(modal_body,b);lv_obj_clear_flag(modal,LV_OBJ_FLAG_HIDDEN);}
static void modal_cancel(lv_event_t*e){(void)e;s_modal_action=MODAL_ACT_PUMP;modal_close();}
/* 告警详情：文案由当前告警影子决定，无告警时不编造事件。 */
static void alert_detail_cb(lv_event_t *e){
    (void)e;
    s_modal_action = MODAL_ACT_PUMP;
    alarm_shadow_t a;
    bool active = AppProto_GetActiveAlarm(&a) && a.active;
#if CONFIG_SMART_HOME_MQ4_UNAVAILABLE
    /* MQ-4 暂时停用：它的告警不应再弹“可燃气超标” */
    if (active && a.alarm_id == PROTO_ALARM_GAS_MQ4) active = false;
#endif
    if (active) modal_open(T(K_MD_TITLE),T(K_MD_GAS));
    else modal_open(T(K_MD_TITLE),T(K_MD_NONE));
}
static void rule_edit_cb(lv_event_t *e){(void)e;s_modal_action = MODAL_ACT_PUMP;modal_open(T(K_MD_RULE_TITLE),T(K_MD_RULE));}
static void f103_upgrade_cb(lv_event_t *e){
    (void)e;
    if (f103_iap_is_running()) { toast_show(T(K_SD_FW_RUNNING)); return; }
    if (AppProto_GetLinkState() != LINK_UP) { toast_show(T(K_SD_FW_OFFLINE)); return; }
    s_modal_action = MODAL_ACT_F103_IAP;
    modal_open(T(K_SD_FW_CONFIRM_TITLE), T(K_SD_FW_CONFIRM_BODY));
}
static void net_ota_check_cb(lv_event_t *e){
    (void)e;
    if (net_ota_is_busy()) { toast_show(T(K_SD_NET_CHECKING)); return; }
    esp_err_t err = net_ota_check_now();
    if (err == ESP_OK) toast_show(T(K_SD_NET_CHECKING));
    else if (net_ota_get_status() == NET_OTA_OFFLINE) toast_show(T(K_SD_NET_OFFLINE));
    else toast_show(T(K_SD_NET_FAILED));
}
static void modal_ok(lv_event_t*e){
    (void)e;
    if (s_modal_action == MODAL_ACT_F103_IAP) {
        modal_close();
        esp_err_t err = f103_iap_start();
        toast_show(err == ESP_OK ? T(K_SD_FW_RUNNING) : T(K_SD_FW_FAILED));
        s_modal_action = MODAL_ACT_PUMP;
        return;
    }
    if (AppProto_GetLinkState() != LINK_UP) { modal_close(); toast_show(T(K_TO_OFF)); return; }
    AppProto_MarkUiClick();
    AppProto_SendActuatorCmd(PROTO_DEV_RELAY2, PROTO_ACTION_ON, 6000);
    modal_close();
    toast_show(T(K_TO_SENT));
}

/* ════════════════════════════════════════════════════
 *  5. 页面切换 / 轨道
 * ════════════════════════════════════════════════════ */
static void page_indicator(int i){int n=i+1;char b[3]={(char)('0'+n/10),(char)('0'+n%10),'\0'};setv(lv_obj_get_child(rail,1),b);}
static void nav_icon_set(int i, bool active){
    lv_obj_t *icon=lv_obj_get_child(nav[i],0);
    if(!icon)return;
    lv_obj_set_style_image_recolor(icon,lv_color_hex(active?METAL:MUTED),0);
    lv_obj_set_style_image_recolor_opa(icon,LV_OPA_COVER,0);
}
static void show_page_objs(int i){
    page=i; sub=-1;
    for(int n=0;n<PAGE_COUNT;n++){
        if(n==i)lv_obj_clear_flag(pages[n],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(pages[n],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(nav[n],lv_color_hex(RAIL),0); nav_icon_set(n,n==i);
    }
    for(int n=0;n<SUB_COUNT;n++) lv_obj_add_flag(subs[n],LV_OBJ_FLAG_HIDDEN);
    page_indicator(i);
}
static void nav_cb(lv_event_t*e){show_page_objs((int)(intptr_t)lv_event_get_user_data(e));}
static void page_set(int i){show_page_objs(i);}
static void sub_show(int which){
    if(which<0||which>=SUB_COUNT)return;
    sub=which;
    for(int n=0;n<PAGE_COUNT;n++) lv_obj_add_flag(pages[n],LV_OBJ_FLAG_HIDDEN);
    for(int n=0;n<SUB_COUNT;n++){ if(n==which)lv_obj_clear_flag(subs[n],LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(subs[n],LV_OBJ_FLAG_HIDDEN); }
    for(int n=0;n<PAGE_COUNT;n++) lv_obj_set_style_bg_color(nav[n],lv_color_hex(RAIL),0);
    nav_icon_set(PAGE_SETTINGS,true); page_indicator(PAGE_SETTINGS);
    /* 进入下级界面立刻按当前数据刷新一次，不必等 1s 定时器（否则第一帧是空的）。 */
    live_refresh_cb(NULL);
}
static void back_cb(lv_event_t*e){(void)e;show_page_objs(PAGE_SETTINGS);}
static void setting_sub_cb(lv_event_t*e){sub_show((int)(intptr_t)lv_event_get_user_data(e));}

/* ════════════════════════════════════════════════════
 *  6. 执行器卡（概览 + 设备页共用；命令 → 回读 → 状态词）
 * ════════════════════════════════════════════════════ */
static void name_color(const device_view_t *v,uint32_t c){ if(v->name) lv_obj_set_style_text_color(v->name,lv_color_hex(c),0); }
static void desc_color(const device_view_t *v,uint32_t c){ if(v->desc) lv_obj_set_style_text_color(v->desc,lv_color_hex(c),0); }
static void state_put(const device_view_t *v,const char *s,uint32_t c){ if(!v->state)return; setv(v->state,s); lv_obj_set_style_text_color(v->state,lv_color_hex(c),0); }

static void dev_sync(const device_view_t *view){
    if(!view || view->key<0 || view->key>=3)return;
    if (view->key == 2) {   /* 门锁槽位：无实物接入 */
        lv_obj_set_style_bg_color(view->root,lv_color_hex(ALT),0);
        name_color(view,MUTED); desc_color(view,MUTED);
        state_put(view,T(K_SV_NA),MUTED);
        return;
    }
    bool link_up = AppProto_GetLinkState()==LINK_UP;
    /* 链路不通或没有快照 → 一律"未知"（不给假状态，点击也被 dev_cb 拦下） */
    if(!dev_known[view->key] || !link_up){
        lv_obj_set_style_bg_color(view->root,lv_color_hex(ALT),0);
        name_color(view,INK); desc_color(view,MUTED);
        state_put(view,T(K_W_ACT_UNKNOWN),MUTED);
        return;
    }
    bool on=dev_on[view->key];
    lv_obj_set_style_bg_color(view->root,lv_color_hex(on?INK:ALT),0);
    name_color(view,on?CREAM:INK); desc_color(view,on?MDARK:MUTED);
    state_put(view,on?T(K_W_ACT_ON):T(K_W_ACT_OFF),on?METAL:MUTED);
}
static void dev_sync_all(int k){
    for(unsigned i=0;i<device_view_count;i++)
        if(device_views[i].key==k) dev_sync(&device_views[i]);
}
static void dev_refresh_from_shadow(void)
{
    actuator_shadow_entry_t sh[4];
    int n = AppProto_GetActuatorShadow(sh, 4);
    if (AppProto_GetLinkState() != LINK_UP) {
        for (int k = 0; k < 2; ++k) if (dev_known[k]) { dev_known[k] = false; dev_sync_all(k); }
        return;
    }
    for (int i = 0; i < n; ++i) {
        int k = sh[i].dev_id == PROTO_DEV_RELAY1 ? 0 : sh[i].dev_id == PROTO_DEV_RELAY2 ? 1 : -1;
        /* 执行器状态是权威快照，不是周期采样：F103 在握手/状态变化时上报，
         * 因此这里不做 3 秒新鲜度超时（那会把健康继电器误判为未知并锁死下一次点击）。 */
        bool known = sh[i].last_update_us > 0;
        bool logical_on = sh[i].state != 0U;
        if (k >= 0 && known && (!dev_known[k] || dev_on[k] != logical_on)) {
            dev_on[k] = logical_on; dev_known[k] = true; dev_sync_all(k);
        }
    }
}
static void dev_cb(lv_event_t*e){
    device_view_t *view=(device_view_t*)lv_event_get_user_data(e);
    if(!view)return;
    int k=view->key;
    if(AppProto_GetLinkState()!=LINK_UP){toast_show(T(K_TO_OFF));return;}
    if (!dev_known[k]) {
        /* 重连后 LINK_UP 可能先于执行器快照到达；主动查询，避免控件"点了没反应"。 */
        AppProto_QueryActuatorStatus();
        toast_show(T(K_TO_BUSY));
        return;
    }
    uint8_t dev = k==0 ? PROTO_DEV_RELAY1 : PROTO_DEV_RELAY2;
    uint8_t action = dev_on[k] ? PROTO_ACTION_OFF : PROTO_ACTION_ON;
    AppProto_MarkUiClick();
    int64_t lat_t0 = esp_timer_get_time();
    AppProto_SendActuatorCmd(dev, action, 0);
    ESP_LOGI("UI_LAT", "[LAT] local cb->tx = %lld us", (long long)(esp_timer_get_time() - lat_t0));
    toast_show(T(K_TO_SENT));
}
/* 执行器卡：名字/说明走文案表（随语言切换），状态词由 dev_sync 按影子刷新。
 * 窄卡（概览 170px）只放名字 + 状态；宽卡（设备页 386px）另加说明行。 */
static lv_obj_t*control(lv_obj_t*p,int x,int y,int w,int name_key,int note_key,int k){
    lv_obj_t*r=click(p,x,y,w,94,dev_on[k]?INK:ALT,12);
    if(device_view_count>=sizeof(device_views)/sizeof(device_views[0])) return r;
    device_view_t *view=&device_views[device_view_count++];
    view->root=r; view->key=k; view->desc=NULL;
    if(w<220){
        view->name=tlab(r,26,27,72,24,name_key,F18,dev_on[k]?CREAM:INK);
    } else {
        view->name=tlab(r,26,27,w-150,24,name_key,F18,dev_on[k]?CREAM:INK);
        view->desc=note_key>=0 ? tlab(r,26,56,w-52,20,note_key,F14,dev_on[k]?MDARK:MUTED)
                               : txt(r,26,56,w-52,20,"",F14,MUTED);
    }
    view->state=txt(r,0,36,w-20,24,"",F18,dev_on[k]?METAL:MUTED);
    align_right(view->state);
    lv_obj_add_event_cb(r,dev_cb,LV_EVENT_CLICKED,view);
    dev_sync(view);
    return r;
}

/* ════════════════════════════════════════════════════
 *  7. 实时数据绑定：影子 → 控件；所有"词"由状态矩阵决定
 * ════════════════════════════════════════════════════ */
static float live_lookup(const sensor_shadow_entry_t *sh, int n, uint8_t dev, uint8_t *q)
{
    for (int i = 0; i < n; i++) if (sh[i].dev_id == dev) { *q = sh[i].quality; return sh[i].value; }
    *q = 0xFF; return 0.0f;
}
static uint32_t live_age_ms(const sensor_shadow_entry_t *sh, int n, uint8_t dev)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        if (sh[i].dev_id != dev) continue;
        if (sh[i].last_update_us <= 0 || now < sh[i].last_update_us) return UINT32_MAX;
        int64_t age = (now - sh[i].last_update_us) / 1000;
        return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    }
    return UINT32_MAX;
}
/* 0 = 无数据/陈旧, 1 = 有值但质量非 VALID, 2 = 有效 */
static int live_sample_state(const sensor_shadow_entry_t *sh, int n, uint8_t dev, uint8_t quality)
{
    if (live_age_ms(sh, n, dev) > 3000U) return 0;
    return quality == PROTO_QUAL_VALID ? 2 : 1;
}
/* 影子里最新一条数据的时间（用于环境页 LIVE / STALE 判定） */
static uint32_t live_newest_age_ms(const sensor_shadow_entry_t *sh, int n)
{
    uint32_t newest = UINT32_MAX;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        if (sh[i].last_update_us <= 0 || now < sh[i].last_update_us) continue;
        int64_t age = (now - sh[i].last_update_us) / 1000;
        uint32_t ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        if (ms < newest) newest = ms;
    }
    return newest;
}
static const char *quality_word(uint8_t q){
    switch(q){
        case PROTO_QUAL_WARMUP:   return T(K_W_Q_WARMUP);
        case PROTO_QUAL_OVERFLOW: return T(K_W_Q_OVER);
        case PROTO_QUAL_STALE:    return T(K_W_Q_STALE);
        case PROTO_QUAL_FAULT:
        default:                  return T(K_W_Q_FAULT);
    }
}
/* 空气质量阶梯：≤20 优 / ≤40 良 / ≤60 中 / >60 差（阈值 60 与告警一致） */
static int air_tier(float p){ return p<=20.0f?TIER_OK : p<=60.0f?TIER_MID : TIER_BAD; }
static int air_tier_key(float p){ return p<=20.0f?K_W_A_OK : p<=40.0f?K_W_A_MID : p<=60.0f?K_W_A_LOW : K_W_A_BAD; }

static void trend_push(float t){
    if(s_trend_n<TREND_POINTS) s_trend[s_trend_n++]=t;
    else { for(int i=1;i<TREND_POINTS;i++) s_trend[i-1]=s_trend[i]; s_trend[TREND_POINTS-1]=t; }
    if(live_chart&&live_chart_ser) lv_chart_set_next_value(live_chart,live_chart_ser,(int32_t)(t*10.0f));
    if(live_chart_window){
        int mins=(s_trend_n*TREND_PERIOD_S)/60; if(mins<1)mins=1;
        char b[24]; snprintf(b,sizeof(b),"%s %d %s",T(K_EN_WIN_A),mins,T(K_EN_WIN_B));
        setv(live_chart_window,b);
    }
}
/* 本次开机内的 min→max（真实派生量，不是编造的趋势） */
static void sig_minmax(int idx,float v,char *out,size_t n){
    static float mn[2]={0,0}, mx[2]={0,0}; static bool init[2]={false,false};
    if(!init[idx]){ mn[idx]=mx[idx]=v; init[idx]=true; }
    else { if(v<mn[idx])mn[idx]=v; if(v>mx[idx])mx[idx]=v; }
    snprintf(out,n,"%.0f → %.0f",(double)mn[idx],(double)mx[idx]);
}

static void wifi_rows_refresh(void)
{
    int total = wifi_scan_result_count();
    bool scanning = wifi_scan_in_progress();
    for (int i = 0; i < WF_ROWS; i++) {
        wifi_ap_info_t ap;
        if (i >= total || !wifi_scan_result_get(i, &ap)) {
            lv_obj_add_flag(wf_row_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(wf_row_box[i], LV_OBJ_FLAG_HIDDEN);
        setv(wf_row_ssid[i], ap.ssid);
        const char *sig = ap.rssi >= -55 ? T(K_WF_SIG_STRONG) : ap.rssi >= -70 ? T(K_WF_SIG_GOOD) :
                          ap.rssi >= -80 ? T(K_WF_SIG_FAIR) : T(K_WF_SIG_WEAK);
        char sb[48]; snprintf(sb,sizeof(sb),"%s %s",T(K_WF_BARS),sig);
        setv(wf_row_note[i], sb);
        setv(wf_row_state[i], ap.authmode == 0 ? T(K_WF_OPEN) : T(K_WF_LOCKED));
        lv_obj_set_style_text_color(wf_row_state[i], lv_color_hex(ap.authmode==0?FG_OK:MUTED), 0);
    }
    if (wf_scan_label) setv(wf_scan_label, scanning ? T(K_WF_SCANNING) : T(K_WF_SCAN));
}

static void wifi_scan_cb(lv_event_t *e){ (void)e; wifi_scan_start(); wifi_rows_refresh(); }
static void wifi_row_cb(lv_event_t *e){
    int idx=(int)(intptr_t)lv_event_get_user_data(e);
    wifi_ap_info_t ap;
    if(!wifi_scan_result_get(idx,&ap)) return;
    if(ap.authmode==0){
        if(wifi_connect_sta(ap.ssid,NULL,true)==ESP_OK) toast_show(T(K_WF_CONN));
        else toast_show(T(K_WF_FAIL));
    } else {
        /* 加密网络：屏上不内置键盘，沿用现有网页配网通道输入密码 */
        toast_show(T(K_WF_TAP_HINT));
    }
}

static void live_refresh_cb(lv_timer_t *t)
{
    (void)t;
    dev_refresh_from_shadow();
    sensor_shadow_entry_t sh[SHADOW_MAX_SENSORS];
    int n = AppProto_GetSensorShadow(sh, SHADOW_MAX_SENSORS);
    link_state_t link = AppProto_GetLinkState();
    bool fresh = (link == LINK_UP);
    uint8_t q; float v; char b[64];
    actuator_feedback_t feedback;
    bool feedback_recent = AppProto_GetLastActuatorFeedback(&feedback) &&
        esp_timer_get_time() - feedback.last_update_us <= 3000000LL;

/* 数值列的统一出口：先过质量层，再决定显示值 / 质量词 / "--" */
#define LIVEVAL(obj, color, dev, fmt)                                        \
    do {                                                                     \
        v = live_lookup(sh, n, (dev), &q);                                   \
        int _st = fresh ? live_sample_state(sh, n, (dev), q) : 0;            \
        if (_st == 2) { snprintf(b, sizeof(b), fmt, (double)v); txt_put((obj), (color), b); } \
        else if (_st == 1) { txt_put((obj), METAL, quality_word(q)); }        \
        else { txt_put((obj), MUTED, "--"); }                                \
    } while (0)

    /* ── 链路三态词 ── */
    const char *link_word = link==LINK_DOWN ? T(K_W_LINK_DOWN) : link==LINK_DEGRADED ? T(K_W_LINK_DEG) : T(K_W_LINK_UP);
    if (feedback_recent && feedback.result != PROTO_RESULT_OK) {
        const char *reason = feedback.result == PROTO_RESULT_ERR_TTL ? "SAFETY LIMIT" :
                             feedback.result == PROTO_RESULT_ERR_BUSY ? "DEVICE BUSY" :
                             feedback.result == PROTO_RESULT_ERR_FAULT ? "DEVICE FAULT" : "COMMAND FAILED";
        txt_put(live_status, METAL, reason);
    } else if (feedback_recent) {
        txt_put(live_status, fresh ? MUTED : METAL, link_word);
    } else {
        txt_put(live_status, fresh ? MUTED : METAL, link_word);
    }
    txt_put(live_system_health, fresh ? CREAM : MDARK,
            link==LINK_DOWN ? T(K_SH_OFF) : link==LINK_DEGRADED ? T(K_SH_DEG) : T(K_SH_OK));
    txt_put(live_rule_status, fresh ? METAL : MUTED, fresh ? T(K_RS_ACTIVE) : T(K_RS_PAUSED));
    /* 链路态变化时刷新规则行的"已暂停"字样（平时不重绘，省一次失效重绘）。 */
    {
        static link_state_t last_link = (link_state_t)-1;
        if (last_link != link) { last_link = link; for (int i = 0; i < 3; i++) scene_sync(i); }
    }

    /* ── 概览气候卡 ── */
    LIVEVAL(live_temp, CREAM, PROTO_DEV_DHT11_TEMP, "%.1f°");
    LIVEVAL(live_hum,  CREAM, PROTO_DEV_DHT11_HUM,  "%.0f%%");
    {
        uint8_t qp; float vp = live_lookup(sh, n, PROTO_DEV_BMP280_PRESS, &qp);
        int stp = fresh ? live_sample_state(sh, n, PROTO_DEV_BMP280_PRESS, qp) : 0;
        if (stp == 2) { snprintf(b,sizeof(b),"%.0f",(double)(vp/100.0f)); txt_put(live_press, CREAM, b); }
        else if (stp == 1) txt_put(live_press, METAL, quality_word(qp));
        else txt_put(live_press, MUTED, "--");
    }

    /* ── 概览信号列：等级胶囊 + 本次开机 min→max + 新鲜度圆点 ── */
    for (int i = 0; i < 2; i++) {
#if CONFIG_SMART_HOME_MQ4_UNAVAILABLE
        /* MQ-4 硬件修复前停用：固定“不可用”，不显示优/良/中/差等级 */
        if (i == 1) {
            pill_tier(&live_sig_pill[1], T(K_SV_NA), TIER_NONE);
            txt_put(live_sig_hist[1], MUTED, "--");
            lv_obj_set_style_bg_color(live_sig_dot[1], lv_color_hex(MDARK), 0);
            continue;
        }
#endif
        uint8_t qi; uint8_t dev = i==0 ? PROTO_DEV_MQ135 : PROTO_DEV_MQ4;
        float p = live_lookup(sh, n, dev, &qi);
        int st = fresh ? live_sample_state(sh, n, dev, qi) : 0;
        if (st == 2) {
            pill_tier(&live_sig_pill[i], T(air_tier_key(p)), air_tier(p));
            sig_minmax(i, p, b, sizeof(b));
            txt_put(live_sig_hist[i], INK, b);
        } else if (st == 1) {
            pill_tier(&live_sig_pill[i], quality_word(qi), TIER_NONE);
            txt_put(live_sig_hist[i], MUTED, "--");
        } else {
            pill_tier(&live_sig_pill[i], "--", TIER_NONE);
            txt_put(live_sig_hist[i], MUTED, "--");
        }
        lv_obj_set_style_bg_color(live_sig_dot[i], lv_color_hex(st==2?METAL:MDARK), 0);
    }
    {
        uint8_t qh; float human = live_lookup(sh, n, PROTO_DEV_HUMAN, &qh);
        int st = fresh ? live_sample_state(sh, n, PROTO_DEV_HUMAN, qh) : 0;
        if (st == 2) pill_tier(&live_sig_pill[2], human>=0.5f?T(K_W_H_YES):T(K_W_H_NO), human>=0.5f?TIER_OK:TIER_NONE);
        else if (st == 1) pill_tier(&live_sig_pill[2], quality_word(qh), TIER_NONE);
        else pill_tier(&live_sig_pill[2], "--", TIER_NONE);
        txt_put(live_sig_hist[2], MUTED, "—");
        lv_obj_set_style_bg_color(live_sig_dot[2], lv_color_hex(st==2?METAL:MDARK), 0);
    }

    /* ── 环境页新鲜度 ── */
    {
        uint32_t age = live_newest_age_ms(sh, n);
        if (link != LINK_UP) txt_put(live_ind, METAL, T(K_W_LINK_DOWN));
        else if (age == UINT32_MAX || age > 6000U) txt_put(live_ind, METAL, T(K_W_Q_STALE));
        else { uint32_t sec=(age+500U)/1000U; if(!sec)sec=1; snprintf(b,sizeof(b),"LIVE / %lus",(unsigned long)sec); txt_put(live_ind, MUTED, b); }
    }
    /* ── 环境读数列 ── */
    LIVEVAL(live_env[0], INK, PROTO_DEV_DHT11_TEMP, "%.1f °C");
    LIVEVAL(live_env[1], INK, PROTO_DEV_DHT11_HUM,  "%.0f %%RH");
    {
        uint8_t qp; float vp = live_lookup(sh, n, PROTO_DEV_BMP280_PRESS, &qp);
        int stp = fresh ? live_sample_state(sh, n, PROTO_DEV_BMP280_PRESS, qp) : 0;
        if (stp == 2) { snprintf(b,sizeof(b),"%.0f hPa",(double)(vp/100.0f)); txt_put(live_env[2], INK, b); }
        else if (stp == 1) txt_put(live_env[2], METAL, quality_word(qp));
        else txt_put(live_env[2], MUTED, "--");
    }
    {
        uint8_t qs; float soil = live_lookup(sh, n, PROTO_DEV_SOIL, &qs);
        int st = fresh ? live_sample_state(sh, n, PROTO_DEV_SOIL, qs) : 0;
        if (st == 2) { snprintf(b,sizeof(b),"%.0f%%",(double)soil); txt_put(live_env[3], SOIL_INK, b); }
        else if (st == 1) txt_put(live_env[3], METAL, quality_word(qs));
        else txt_put(live_env[3], MUTED, "--");
    }
    {
        uint8_t qa,qb,qc;
        float air=live_lookup(sh,n,PROTO_DEV_MQ135,&qa), gas=live_lookup(sh,n,PROTO_DEV_MQ4,&qb), rain=live_lookup(sh,n,PROTO_DEV_RAIN,&qc);
        int sa=fresh?live_sample_state(sh,n,PROTO_DEV_MQ135,qa):0,
            sb=fresh?live_sample_state(sh,n,PROTO_DEV_MQ4,qb):0,
            sc=fresh?live_sample_state(sh,n,PROTO_DEV_RAIN,qc):0;
        /* 逐路各自出词：某一路异常（例如雨滴超量程）不该把整行退化成 "--"，
         * 否则另外两路的有效读数会被无辜藏掉。 */
        char pa[20], pb[20], pc[20];
        bool tainted = false;
        if (sa==2) snprintf(pa,sizeof(pa),"%.0f%%",(double)air);
        else if (sa==1) { snprintf(pa,sizeof(pa),"%s",quality_word(qa)); tainted=true; }
        else { snprintf(pa,sizeof(pa),"--"); tainted=true; }
        if (sb==2) snprintf(pb,sizeof(pb),"%.0f%%",(double)gas);
        else if (sb==1) { snprintf(pb,sizeof(pb),"%s",quality_word(qb)); tainted=true; }
        else { snprintf(pb,sizeof(pb),"--"); tainted=true; }
#if CONFIG_SMART_HOME_MQ4_UNAVAILABLE
        /* MQ-4 已停用：显示“不可用”而不是故障/超量程；
         * 整行颜色只由空气/雨滴两路决定，避免常驻金属色。 */
        snprintf(pb, sizeof(pb), "%s", T(K_SV_NA));
        tainted = (sa != 2) || (sc != 2);
#endif
        if (sc==2) snprintf(pc,sizeof(pc),"%s", rain>=50.0f?T(K_W_RAIN_YES):T(K_W_RAIN_NO));
        else if (sc==1) { snprintf(pc,sizeof(pc),"%s",quality_word(qc)); tainted=true; }
        else { snprintf(pc,sizeof(pc),"--"); tainted=true; }
        snprintf(b,sizeof(b),"%s / %s / %s", pa, pb, pc);
        txt_put(live_env[4], tainted?METAL:INK, b);
    }

    /* ── 房间画像：温湿度组合 + 质量态 → 大字词 + 建议行（阈值 20/26℃、40/60%） ── */
    {
        uint8_t qt, qh;
        float vt = live_lookup(sh, n, PROTO_DEV_DHT11_TEMP, &qt);
        float vh = live_lookup(sh, n, PROTO_DEV_DHT11_HUM,  &qh);
        int st  = fresh ? live_sample_state(sh, n, PROTO_DEV_DHT11_TEMP, qt) : 0;
        int shm = fresh ? live_sample_state(sh, n, PROTO_DEV_DHT11_HUM,  qh) : 0;
        if (st == 2 && shm == 2) {
            int pk = K_PF_COMFORT, ak = K_FA_COMFORT;
            if (vt>26.0f && vh>60.0f) { pk=K_PF_MUGGY; ak=K_FA_MUGGY; }
            else if (vt>26.0f)        { pk=K_PF_HOT;   ak=K_FA_HOT; }
            else if (vt<20.0f)        { pk=K_PF_COLD;  ak=K_FA_COLD; }
            else if (vh<40.0f)        { pk=K_PF_DRY;   ak=K_FA_DRY; }
            else if (vh>60.0f)        { pk=K_PF_HUMID; ak=K_FA_HUMID; }
            txt_put(live_profile_title, CREAM, T(pk));
            txt_put(live_profile, MDARK, T(ak));
            /* 温度趋势：每 30s 采一点，环形滚动（本机无持久历史，不声称"24 小时"） */
            if (++s_trend_tick >= TREND_PERIOD_S) { s_trend_tick = 0; trend_push(vt); }
        } else if (st == 1 || shm == 1) {
            txt_put(live_profile_title, METAL, T(K_W_Q_FAULT));
            txt_put(live_profile, MUTED, T(K_FA_NODATA));
        } else {
            txt_put(live_profile_title, MUTED, T(K_EN_NODATA));
            txt_put(live_profile, MUTED, T(K_FA_NODATA));
        }
    }

    /* ── 告警页：由 ALARM_EVENT 影子驱动 ── */
    {
        uint8_t qs, qh;
        float soil = live_lookup(sh, n, PROTO_DEV_SOIL, &qs);
        float human = live_lookup(sh, n, PROTO_DEV_HUMAN, &qh);
        bool soil_ok = fresh && live_sample_state(sh, n, PROTO_DEV_SOIL, qs)==2;
        bool human_ok = fresh && live_sample_state(sh, n, PROTO_DEV_HUMAN, qh)==2;
        alarm_shadow_t a; AppProto_GetAlarmShadow(&a);   /* 无条件取影子：未告警时才读得到"已恢复" */
#if CONFIG_SMART_HOME_MQ4_UNAVAILABLE
        /* MQ-4 停用期间：其告警不显示 active，也不显示“已恢复” */
        if (a.alarm_id == PROTO_ALARM_GAS_MQ4) { a.active = false; a.alarm_id = 0; }
#endif
        bool active = a.active && fresh;
        /* "已恢复"只在最近一次事件后 10 分钟内显示，之后回到"无告警"，避免永久停留。 */
        bool cleared = !active && a.alarm_id != 0 && a.last_update_us > 0 &&
                       (esp_timer_get_time() - a.last_update_us) <= 600000000LL;
        if (!fresh) {
            lv_obj_set_style_bg_color(live_alert_banner, lv_color_hex(INK), 0);
            txt_put(live_alert_count, METAL, T(K_W_LINK_DOWN));
            txt_put(live_alert_main, CREAM, T(K_AL_NONE));
            txt_put(live_alert_sub, MDARK, T(K_AL_NONE_SUB));
        } else if (active) {
            lv_obj_set_style_bg_color(live_alert_banner, lv_color_hex(ALERT_BG), 0);
            txt_put(live_alert_count, METAL, T(K_AC_ALERT));
            const char *kind = a.alarm_id==PROTO_ALARM_PUMP ? T(K_W_PUMP_ALARM) : T(K_AL_KIND_GAS);
            txt_put(live_alert_main, CREAM, kind);
            const char *lvl = a.level==PROTO_LEVEL_URGENT?T(K_LV_URGENT):a.level==PROTO_LEVEL_WARN?T(K_LV_WARN):T(K_LV_HINT);
            /* 只展示设备真实上报的触发值/阈值；阈值缺失就只给等级词。 */
            if (a.threshold > 0.0f) snprintf(b,sizeof(b),"%s · %s %.0f%% / %s %.0f%%",lvl,T(K_AL_TRIGGER),(double)a.value,T(K_AL_THR),(double)a.threshold);
            else snprintf(b,sizeof(b),"%s",lvl);
            txt_put(live_alert_sub, MDARK, b);
        } else {
            lv_obj_set_style_bg_color(live_alert_banner, lv_color_hex(INK), 0);
            txt_put(live_alert_count, INK, cleared ? T(K_AC_CLEAR) : T(K_AC_NONE));
            txt_put(live_alert_main, CREAM, cleared ? T(K_AL_CLEARED) : T(K_AL_NONE));
            if (cleared) snprintf(b,sizeof(b),"%s %.0f%% < %s %.0f%%",T(K_AL_TRIGGER),(double)a.value,T(K_AL_THR),(double)a.threshold);
            else snprintf(b,sizeof(b),"%s · %s",T(K_AL_NONE_SUB), human_ok&&human>=0.5f?T(K_W_H_YES):T(K_W_H_NO));
            txt_put(live_alert_sub, MDARK, b);
        }
        if (soil_ok) { snprintf(b,sizeof(b),"%.0f%%",(double)soil); txt_put(live_soil_alert_value, INK, b); }
        else txt_put(live_soil_alert_value, MUTED, "--");
    }

    /* ── 设置页摘要 ── */
    txt_put(live_setting_network, fresh?INK:METAL, fresh?"F103 LINK UP":"F103 OFFLINE");
    {
        /* 阶段 6：C3 作为第二个 MQTT 节点。离线=availability offline 或 90s 无数据 */
        remote_node_status_t rn;
        if (remote_node_get(&rn)) {
            if (rn.online && rn.has_rssi) {
                snprintf(b, sizeof(b), "C3 NODE ONLINE - %d dBm", rn.rssi);
                txt_put(live_setting_devices, INK, b);
            } else if (rn.online) {
                txt_put(live_setting_devices, INK, "C3 NODE ONLINE");
            } else {
                txt_put(live_setting_devices, MUTED, "C3 NODE OFFLINE");
            }
        } else {
            txt_put(live_setting_devices, fresh?INK:MUTED,
                    fresh?"SENSORS / ACTUATORS ONLINE":"WAITING FOR DEVICES");
        }
    }

    /* ── 下级界面（只在可见时刷新，避免无谓重绘） ── */
    if (sub == SUB_NETWORK) {
        wifi_state_t wst = wifi_get_state();
        char ssid[33]={0}, pwd[64]={0}, ip[16]={0};
        bool have_cred = storage_load_wifi_cred(ssid, sizeof(ssid), pwd, sizeof(pwd))==ESP_OK && ssid[0];
        wifi_get_ip_info(ip, NULL, NULL, sizeof(ip));
        if (wst == WIFI_STATE_CONNECTED) {
            snprintf(b,sizeof(b),"%s / %s", have_cred?ssid:"--", ip[0]?ip:"--");
            txt_put(wf_status, INK, b);
        } else if (wst == WIFI_STATE_CONNECTING) {
            txt_put(wf_status, METAL, T(K_WF_CONN));
        } else {
            txt_put(wf_status, MUTED, T(K_WF_CUR_NONE));
        }
        wifi_rows_refresh();
    }
    if (sub == SUB_DISPLAY) {
        uint32_t idle = 0; char iv[24];
        if (storage_get_u32("ui_idle_sec", &idle) != ESP_OK) idle = 60;
        snprintf(iv,sizeof(iv),"%u %s",(unsigned)idle,T(K_DP_SEC));
        setv(dp_idle_value, iv);
        bool mute = rule_runtime_get_scene_enabled(2);
        setv(dp_bright_value, T(K_DP_BRIGHT_VAL));
        setv(dp_vol_value, "—");
        switch_set(dp_mute_switch, !mute);
    }
    if (sub == SUB_DEVICES) {
        f103_iap_status_t st = f103_iap_get_status();
        bool up = AppProto_GetLinkState() == LINK_UP;
        int pct = f103_iap_get_progress();
        if (st == F103_IAP_RUNNING) {
            snprintf(b,sizeof(b),"%s %d%%",T(K_SD_FW_RUNNING),pct);
            txt_put(live_f103_iap_status, METAL, b);
        } else if (st == F103_IAP_SUCCESS) {
            txt_put(live_f103_iap_status, FG_OK, T(K_SD_FW_SUCCESS));
        } else if (st == F103_IAP_FAILED) {
            txt_put(live_f103_iap_status, DANGER, T(K_SD_FW_FAILED));
        } else {
            txt_put(live_f103_iap_status, up?INK:MUTED,
                    up?T(K_SD_FW_READY):T(K_SD_FW_OFFLINE));
        }

        /* 网络 OTA 状态：检查/下载/结果都由后台任务驱动 */
        net_ota_status_t ns = net_ota_get_status();
        bool nbusy = net_ota_is_busy();
        switch (ns) {
        case NET_OTA_CHECKING:
            txt_put(live_net_ota_status, METAL, T(K_SD_NET_CHECKING));
            break;
        case NET_OTA_DOWNLOADING:
            snprintf(b,sizeof(b),"%s %d%%",T(K_SD_NET_DOWNLOADING),net_ota_get_progress());
            txt_put(live_net_ota_status, METAL, b);
            break;
        case NET_OTA_SUCCESS:
            txt_put(live_net_ota_status, FG_OK, T(K_SD_FW_SUCCESS));
            break;
        case NET_OTA_UP_TO_DATE:
            txt_put(live_net_ota_status, FG_OK, T(K_SD_NET_UP_TO_DATE));
            break;
        case NET_OTA_FAILED:
            txt_put(live_net_ota_status, DANGER, T(K_SD_NET_FAILED));
            break;
        case NET_OTA_OFFLINE:
            txt_put(live_net_ota_status, MUTED, T(K_SD_NET_OFFLINE));
            break;
        default:
            txt_put(live_net_ota_status, up?INK:MUTED, up?T(K_SD_FW_READY):T(K_SD_NET_OFFLINE));
            break;
        }
        setv(net_ota_btn_label, nbusy?T(K_SD_NET_CHECKING):T(K_SD_NET_CHECK));
    }
#undef LIVEVAL
}

/* ════════════════════════════════════════════════════
 *  8. 六个页面
 * ════════════════════════════════════════════════════ */
static void overview(lv_obj_t*p){
    tlab(p,36,44,500,16,K_OV_EYEBROW,F10,MUTED);
    tlab(p,36,74,500,34,K_OV_TITLE,F28,INK);
    live_status=txt(p,36,116,520,22,"",F14,MUTED);
    tlab(p,760,44,140,18,K_OV_NODES,F12,MUTED);
    lv_obj_t*h=box(p,36,166,292,300,INK,12);
    tlab(h,24,34,230,16,K_OV_CLIMATE,F10,MDARK);
    live_temp=txt(h,24,92,244,72,"--",F48,CREAM);
    tlab(h,24,166,230,20,K_OV_TEMPNOTE,F14,MDARK);
    box(h,24,216,244,1,DARK_HAIR,0);
    live_hum=txt(h,24,224,64,22,"--",F18,CREAM);
    tlab(h,96,226,120,18,K_OV_HUM,F14,MDARK);
    live_press=txt(h,24,256,64,22,"--",F18,CREAM);
    tlab(h,96,258,150,18,K_OV_PRESS,F14,MDARK);

    tlab(p,362,166,300,16,K_OV_SIGNALS,F10,MUTED);
    tlab(p,362,204,250,24,K_OV_SIGTITLE,F18,INK);
    const int sign_keys[3]={K_SIG_AIR,K_SIG_GAS,K_SIG_HUMAN};
    for(int i=0;i<3;i++){
        int y=242+i*68;
        line(p,362,y,500);
        tlab(p,362,y+22,200,20,sign_keys[i],F14,MUTED);
        live_sig_pill[i]=pill_mk(p,586,y+18,76);
        live_sig_hist[i]=txt(p,700,y+19,140,22,"--",F14,INK); align_right(live_sig_hist[i]);
        live_sig_dot[i]=box(p,860,y+28,10,10,METAL,5);
    }
    tlab(p,362,420,220,16,K_OV_ONETOUCH,F10,MUTED);
    control(p,362,450,170,K_DEV_LIGHT,-1,0);
    control(p,546,450,170,K_DEV_PUMP,-1,1);
    box(p,36,556,122,3,METAL,0);
}

static void environment(lv_obj_t*p){
    tlab(p,36,44,500,16,K_EN_EYEBROW,F10,MUTED);
    tlab(p,36,74,500,34,K_EN_TITLE,F28,INK);
    tlab(p,36,116,600,22,K_EN_SUB,F14,MUTED);
    live_ind=txt(p,770,44,110,18,"",F12,MUTED); align_right(live_ind);

    lv_obj_t*h=box(p,36,166,344,340,INK,12);
    tlab(h,24,34,260,16,K_EN_PROFILE,F10,MDARK);
    live_profile_title=txt(h,24,104,290,72,"--",F48,MUTED);
    live_profile=txt(h,24,180,300,20,"--",F14,MDARK);
    box(h,24,214,296,1,DARK_HAIR,0);
    tlab(h,24,226,150,16,K_EN_TREND,F10,MDARK);
    live_chart_window=txt(h,180,226,140,16,"",F10,MDARK); align_right(live_chart_window);
    live_chart=lv_chart_create(h);
    lv_obj_set_pos(live_chart,24,248); lv_obj_set_size(live_chart,296,84);
    lv_obj_set_style_bg_opa(live_chart,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(live_chart,0,0);
    lv_obj_set_style_pad_all(live_chart,0,0);
    lv_obj_set_style_radius(live_chart,0,0);
    lv_obj_clear_flag(live_chart,LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(live_chart,LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(live_chart,TREND_POINTS);
    lv_chart_set_range(live_chart,LV_CHART_AXIS_PRIMARY_Y,0,400);   /* 0.0~40.0 ℃ ×10 */
    lv_chart_set_div_line_count(live_chart,2,0);
    lv_chart_set_update_mode(live_chart,LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_line_color(live_chart,lv_color_hex(CHART_GRID),LV_PART_MAIN);
    lv_obj_set_style_line_width(live_chart,2,LV_PART_ITEMS);
    lv_obj_set_style_width(live_chart,0,LV_PART_INDICATOR);
    lv_obj_set_style_height(live_chart,0,LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(live_chart,LV_OPA_TRANSP,LV_PART_INDICATOR);
    live_chart_ser=lv_chart_add_series(live_chart,lv_color_hex(METAL),LV_CHART_AXIS_PRIMARY_Y);

    tlab(p,424,166,300,16,K_EN_READINGS,F10,MUTED);
    tlab(p,424,204,250,24,K_EN_LIVE,F18,INK);
    const int keys[5]={K_S_TEMP,K_S_HUM,K_S_PRESS,K_S_SOIL,K_S_AIR};
    for(int i=0;i<5;i++){
        int y=242+i*35;
        if(i<4) line(p,424,y,440);
        tlab(p,424,y+8,240,20,keys[i],F14,MUTED);
        live_env[i]=txt(p,690,y+6,190,22,"--",i==3?F18:F14,INK);
        align_right(live_env[i]);
    }
}

static void devices(lv_obj_t*p){
    tlab(p,36,44,500,16,K_DV_EYEBROW,F10,MUTED);
    tlab(p,36,74,500,34,K_DV_TITLE,F28,INK);
    tlab(p,36,116,600,22,K_DV_SUB,F14,MUTED);
    tlab(p,750,44,130,18,K_DV_COUNT,F12,MUTED);
    control(p,36,166,386,K_DEV_LIGHT,K_DV_LIGHT_NOTE,0);
    control(p,36,282,386,K_DEV_PUMP,K_DV_PUMP_NOTE,1);

    tlab(p,478,166,300,16,K_DV_SAFETY,F10,MUTED);
    const int lk[3]={K_SF_LOCK,K_SF_BEEP,K_SF_GUARD};
    for(int i=0;i<3;i++){
        int y=204+i*66;
        line(p,478,y,420);
        tlab(p,478,y+22,240,20,lk[i],F14,MUTED);
        int vk = i==0?K_SV_NA : i==1?K_SV_MUTE : K_SV_CONFIRM;
        tlab(p,790,y+20,110,22,vk,F18,INK);
    }
    lv_obj_t*c=box(p,36,400,386,68,ALT,12);   /* 语言开关：与原型同位置 */
    tlab(c,24,16,240,24,K_DV_LANG,F18,INK);
    tlab(c,24,42,250,18,K_DV_LANG_NOTE,F14,MUTED);
    lang_switch=switch_mk(c,318,22,s_zh,lang_switch_cb,NULL);
}

/* ════════════════════════════════════════════════════
 *  9. 自动化（只读 + 开/关；编辑走网页 → P4 本地配置端点 → NVS 双槽 r0/r1）
 * ════════════════════════════════════════════════════ */
static void scene_sync(int i){
    if(i<0||i>=3||!scene_names[i])return;
    lv_obj_t *r=lv_obj_get_parent(scene_names[i]);
    bool on=scene_on[i];
    bool can_run = AppProto_GetLinkState()==LINK_UP;
    lv_obj_set_style_bg_color(r,lv_color_hex(on?INK:ALT),0);
    lv_obj_set_style_text_color(scene_names[i],lv_color_hex(on?CREAM:INK),0);
    if(scene_descs[i]) lv_obj_set_style_text_color(scene_descs[i],lv_color_hex(on?MDARK:MUTED),0);
    /* 链路离线时规则不会执行：保留持久化的开关位置，但状态词明确显示"已暂停"，
     * 让"能打开却没效果"这件事在屏上直接说清楚，而不是让用户以为坏了。 */
    if(on && !can_run){
        setv(scene_states[i],T(K_RM_PAUSED));
        lv_obj_set_style_text_color(scene_states[i],lv_color_hex(MUTED),0);
    } else {
        setv(scene_states[i],on?T(K_RM_ACTIVE):T(K_RM_OFF));
        lv_obj_set_style_text_color(scene_states[i],lv_color_hex(on?METAL:MUTED),0);
    }
    switch_set(scene_switches[i],on);
}
static void scene_cb(lv_event_t*e){
    int k=(int)(intptr_t)lv_event_get_user_data(e);
    bool next=!scene_on[k];
    if(rule_runtime_set_scene_enabled((uint8_t)k,next)!=ESP_OK){ toast_show(T(K_TO_SAVE_FAIL)); return; }
    scene_on[k]=next; scene_sync(k);
    if(next && AppProto_GetLinkState()!=LINK_UP) toast_show(T(K_TO_SAVED_PAUSED));
    else toast_show(next?T(K_TO_SAVED_ON):T(K_TO_SAVED_OFF));
}
static void lang_switch_cb(lv_event_t *e){
    (void)e;
    s_zh = !s_zh;
    lang_save();
    apply_lang();
    live_refresh_cb(NULL);   /* 动态词（状态/等级/画像/告警）按新语言重算一次 */
}
static void automation(lv_obj_t*p){
    tlab(p,36,44,500,16,K_AU_EYEBROW,F10,MUTED);
    tlab(p,36,74,620,34,K_AU_TITLE,F28,INK);
    live_rule_status=txt(p,36,116,600,22,"",F14,MUTED);
    lv_obj_t*a=click(p,764,44,100,44,INK,22);
    { lv_obj_t*l=tlab(a,0,12,100,20,K_AU_ADD,F14,CREAM); align_center(l); }
    lv_obj_add_event_cb(a,rule_edit_cb,LV_EVENT_CLICKED,NULL);
    tlab(p,36,166,300,16,K_AU_ENGINE,F10,MUTED);
    const int nk[3]={K_RULE1,K_RULE2,K_RULE3}, dk[3]={K_RULE1D,K_RULE2D,K_RULE3D};
    for(int i=0;i<3;i++){
        scene_on[i]=rule_runtime_get_scene_enabled((uint8_t)i);
        lv_obj_t*r=click(p,36,192+i*84,864,68,scene_on[i]?INK:ALT,12);
        scene_names[i]=tlab(r,24,20,140,24,nk[i],F18,scene_on[i]?CREAM:INK);
        scene_descs[i]=tlab(r,118,22,560,20,dk[i],F14,scene_on[i]?MDARK:MUTED);
        scene_states[i]=txt(r,708,22,60,20,"",F14,METAL);
        scene_switches[i]=switch_mk(r,800,22,scene_on[i],scene_cb,(void*)(intptr_t)i);
        scene_sync(i);
    }
    tlab(p,36,470,864,18,K_AU_PENDING,F12,NOTE_BAD);
}

/* ════════════════════════════════════════════════════
 *  10. 告警（黑卡保持简洁：状态行 + 一行细节 + 查看）
 * ════════════════════════════════════════════════════ */
static void alerts(lv_obj_t*p){
    tlab(p,36,44,500,16,K_AL_EYEBROW,F10,MUTED);
    tlab(p,36,74,600,34,K_AL_TITLE,F28,INK);
    tlab(p,36,116,600,22,K_AL_SUB,F14,MUTED);
    live_alert_count=txt(p,740,44,140,18,"",F12,MUTED); align_right(live_alert_count);
    live_alert_banner=click(p,36,166,864,96,INK,12);
    txt(live_alert_banner,24,18,300,16,"LIVE MONITOR",F10,METAL);
    live_alert_main=txt(live_alert_banner,24,40,620,24,"",F18,CREAM);
    live_alert_sub=txt(live_alert_banner,24,66,640,20,"",F14,MDARK);
    tlab(live_alert_banner,766,40,80,20,K_AL_VIEW,F14,METAL);
    lv_obj_add_event_cb(live_alert_banner,alert_detail_cb,LV_EVENT_CLICKED,NULL);
    line(p,36,308,864);
    live_soil_alert=tlab(p,36,330,520,22,K_AL_SOIL,F14,MUTED);
    live_soil_alert_value=txt(p,740,330,140,22,"--",F14,INK); align_right(live_soil_alert_value);
    line(p,36,371,864);
    tlab(p,36,393,520,22,K_AL_PRIV,F14,MUTED);
    { lv_obj_t* l=tlab(p,740,393,140,22,K_AL_INFO,F14,MUTED); align_right(l); }
    tlab(p,36,440,864,22,K_AL_NOTE,F14,MUTED);
}

/* ════════════════════════════════════════════════════
 *  11. 设置（四卡，前两卡进下级界面）
 * ════════════════════════════════════════════════════ */
static void settings(lv_obj_t*p){
    tlab(p,36,44,500,16,K_ST_EYEBROW,F10,MUTED);
    tlab(p,36,74,600,34,K_ST_TITLE,F28,INK);
    tlab(p,36,116,600,22,K_ST_SUB,F14,MUTED);
    tlab(p,36,166,300,16,K_ST_PREFS,F10,MUTED);
    const int nk[4]={K_SET1,K_SET2,K_SET3,K_SET4};
    for(int i=0;i<4;i++){
        /* 阶段 8：第 3 卡「设备与标定」改为可进入的二级页（F103 固件升级）。 */
        bool linked = (i<=2);
        lv_obj_t*c=click(p,36+(i%2)*396,192+(i/2)*94,372,72,ALT,12);
        tlab(c,24,18,280,24,nk[i],F18,INK);
        if(i==0) live_setting_network=txt(c,24,45,290,20,"",F14,MUTED);
        else if(i==1) tlab(c,24,45,290,20,K_SET2_DESC,F14,MUTED);
        else if(i==2) live_setting_devices=txt(c,24,45,290,20,"",F14,MUTED);
        else tlab(c,24,45,290,20,K_SET4_DESC,F14,MUTED);
        if(linked){
            int target = (i==0)?SUB_NETWORK : (i==1)?SUB_DISPLAY : SUB_DEVICES;
            txt(c,332,22,30,24,">",F18,METAL);
            lv_obj_add_event_cb(c,setting_sub_cb,LV_EVENT_CLICKED,(void*)(intptr_t)target);
        } else {
            lv_obj_add_event_cb(c,toast_key_cb,LV_EVENT_CLICKED,(void*)(intptr_t)K_TO_RO);
        }
    }
    lv_obj_t*h=box(p,36,400,768,42,INK,12);
    live_system_health=txt(h,24,12,700,18,"",F12,CREAM);
}

/* ════════════════════════════════════════════════════
 *  12. 下级界面 A：网络连接（实时扫描列表；开放网络可直接连）
 * ════════════════════════════════════════════════════ */
static void dp_idle_cb(lv_event_t *e){
    (void)e;
    uint32_t v=0; if(storage_get_u32("ui_idle_sec",&v)!=ESP_OK) v=60;
    uint32_t next = v<60u?60u : v<120u?120u : v<300u?300u : 30u;
    storage_set_u32("ui_idle_sec", next);
    char b[24]; snprintf(b,sizeof(b),"%u %s",(unsigned)next,T(K_DP_SEC));
    setv(dp_idle_value,b);
}
/* 蜂鸣器提示音与自动化「夜间守护」共用同一个偏好（rule_runtime scene 2），
 * 保证屏上两处开关状态永远一致，也不出现两套互相打架的"静音"。 */
static void dp_mute_cb(lv_event_t *e){
    lv_obj_t *hit=lv_event_get_target(e);
    bool next_mute = !rule_runtime_get_scene_enabled(2);
    if(rule_runtime_set_scene_enabled(2,next_mute)!=ESP_OK){ toast_show(T(K_TO_SAVE_FAIL)); return; }
    scene_on[2]=next_mute; scene_sync(2);
    switch_set(hit,!next_mute);
    toast_show(next_mute?T(K_TO_SAVED_OFF):T(K_TO_SAVED_ON));
}

static void build_subs(void){
    /* ── 下级 A：网络连接 ── */
    lv_obj_t *w=subs[SUB_NETWORK];
    lv_obj_t *back=click(w,36,36,96,44,ALT,22);
    { lv_obj_t*l=tlab(back,0,12,96,20,K_PG_BACK,F14,INK); align_center(l); }
    lv_obj_add_event_cb(back,back_cb,LV_EVENT_CLICKED,NULL);
    tlab(w,150,44,300,16,K_WF_EYEBROW,F10,MUTED);
    tlab(w,36,100,600,34,K_WF_TITLE,F28,INK);
    wf_status=txt(w,36,140,600,22,"",F14,MUTED);
    { lv_obj_t*l=tlab(w,676,150,200,18,K_WF_HINT,F12,MUTED); align_right(l); }
    /* 扫描列表：可上下滑动的容器（结果多于可见高度时靠滑动浏览） */
    wf_list=lv_obj_create(w);
    lv_obj_remove_style_all(wf_list);
    lv_obj_set_pos(wf_list,36,222);
    lv_obj_set_size(wf_list,864,240);
    lv_obj_set_style_bg_opa(wf_list,LV_OPA_TRANSP,0);
    lv_obj_set_style_pad_all(wf_list,0,0);
    lv_obj_add_flag(wf_list,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wf_list,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wf_list,LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(wf_list,4,LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(wf_list,2,LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(wf_list,lv_color_hex(MDARK),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(wf_list,LV_OPA_COVER,LV_PART_SCROLLBAR);

    for(int i=0;i<WF_ROWS;i++){
        wf_row_box[i]=click(wf_list,0,i*55,864,55,ALT,0);
        lv_obj_set_style_bg_opa(wf_row_box[i],LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(wf_row_box[i],1,0);
        lv_obj_set_style_border_color(wf_row_box[i],lv_color_hex(HAIR),0);
        lv_obj_set_style_border_side(wf_row_box[i],LV_BORDER_SIDE_TOP,0);
        wf_row_ssid[i]=txt(wf_row_box[i],0,12,420,22,"",F18,INK);
        wf_row_note[i]=txt(wf_row_box[i],364,15,240,20,"",F14,MUTED);
        wf_row_state[i]=txt(wf_row_box[i],624,15,160,20,"",F14,MUTED);
        lv_obj_add_event_cb(wf_row_box[i],wifi_row_cb,LV_EVENT_CLICKED,(void*)(intptr_t)i);
        lv_obj_add_flag(wf_row_box[i],LV_OBJ_FLAG_HIDDEN);
    }
    wf_scan_btn=click(w,36,468,180,44,INK,22);
    wf_scan_label=tlab(wf_scan_btn,0,12,180,20,K_WF_SCAN,F14,CREAM); align_center(wf_scan_label);
    lv_obj_add_event_cb(wf_scan_btn,wifi_scan_cb,LV_EVENT_CLICKED,NULL);
    tlab(w,36,522,864,18,K_WF_FEAS,F12,MUTED);

    /* ── 下级 B：屏幕与声音（只放实机真实能力） ── */
    lv_obj_t *d=subs[SUB_DISPLAY];
    lv_obj_t *back2=click(d,36,36,96,44,ALT,22);
    { lv_obj_t*l=tlab(back2,0,12,96,20,K_PG_BACK,F14,INK); align_center(l); }
    lv_obj_add_event_cb(back2,back_cb,LV_EVENT_CLICKED,NULL);
    tlab(d,150,44,300,16,K_DP_EYEBROW,F10,MUTED);
    tlab(d,36,100,600,34,K_DP_TITLE,F28,INK);
    tlab(d,36,140,700,22,K_DP_SUB,F14,MUTED);
    const int dn[3]={K_DP_BRIGHT,K_DP_VOL,K_DP_IDLE};
    const int dnote[3]={K_DP_BRIGHT_NOTE,K_DP_VOL_NOTE,K_DP_IDLE_NOTE};
    for(int i=0;i<3;i++){
        int y=224+i*72;
        line(d,36,y,864);
        tlab(d,36,y+26,220,24,dn[i],F18,INK);
        tlab(d,300,y+28,420,20,dnote[i],F14,MUTED);
        if(i==2){
            dp_idle_value=txt(d,740,y+26,140,24,"",F14,INK);
            lv_obj_t*hit=click(d,36,y,864,72,0,0);
            lv_obj_set_style_bg_opa(hit,LV_OPA_TRANSP,0);
            lv_obj_add_event_cb(hit,dp_idle_cb,LV_EVENT_CLICKED,NULL);
            tlab(d,36,y+46,420,16,K_DP_CYCLE,F12,MUTED);
        } else if(i==0) dp_bright_value=txt(d,740,y+26,140,24,"",F14,INK);
        else dp_vol_value=txt(d,740,y+26,140,24,"",F14,INK);
        align_right(i==2?dp_idle_value:i==0?dp_bright_value:dp_vol_value);
    }
    line(d,36,440,864);
    tlab(d,36,466,220,24,K_DP_MUTE,F18,INK);
    tlab(d,300,468,420,20,K_DP_MUTE_NOTE,F14,MUTED);
    dp_mute_switch=switch_mk(d,700,462,!rule_runtime_get_scene_enabled(2),dp_mute_cb,NULL);

    /* ── 下级 C：设备与标定（阶段 8：F103 固件升级） ── */
    lv_obj_t *v=subs[SUB_DEVICES];
    lv_obj_t *back3=click(v,36,36,96,44,ALT,22);
    { lv_obj_t*l=tlab(back3,0,12,96,20,K_PG_BACK,F14,INK); align_center(l); }
    lv_obj_add_event_cb(back3,back_cb,LV_EVENT_CLICKED,NULL);
    tlab(v,150,44,300,16,K_SD_EYEBROW,F10,MUTED);
    tlab(v,36,100,700,34,K_SD_TITLE,F28,INK);
    tlab(v,36,140,800,22,K_SD_SUB,F14,MUTED);

    lv_obj_t *fw=box(v,36,222,864,150,ALT,12);
    tlab(fw,24,18,600,24,K_SD_FW_TITLE,F18,INK);
    tlab(fw,24,46,700,20,K_SD_FW_DESC,F14,MUTED);
    live_f103_iap_status=txt(fw,24,82,620,20,"",F14,MUTED);
    lv_obj_t *btn=click(fw,700,88,140,44,INK,22);
    { lv_obj_t*l=tlab(btn,0,12,140,20,K_SD_FW_UPGRADE,F14,CREAM); align_center(l); }
    lv_obj_add_event_cb(btn,f103_upgrade_cb,LV_EVENT_CLICKED,NULL);

    /* ── P4 网络 OTA（阶段 8 补充：manifest + SHA256 + 防降级） ── */
    lv_obj_t *no=box(v,36,390,864,150,ALT,12);
    tlab(no,24,18,600,24,K_SD_NET_TITLE,F18,INK);
    tlab(no,24,46,700,20,K_SD_NET_DESC,F14,MUTED);
    live_net_ota_status=txt(no,24,82,620,20,"",F14,MUTED);
    net_ota_btn=click(no,700,88,140,44,INK,22);
    { lv_obj_t*l=tlab(net_ota_btn,0,12,140,20,K_SD_NET_CHECK,F14,CREAM); align_center(l); net_ota_btn_label=l; }
    lv_obj_add_event_cb(net_ota_btn,net_ota_check_cb,LV_EVENT_CLICKED,NULL);
}

/* ════════════════════════════════════════════════════
 *  13. 轨道 / 弹窗 / Toast 外壳
 * ════════════════════════════════════════════════════ */
static void build_rail(void){
    static const lv_image_dsc_t *icons[PAGE_COUNT]={&reicon_nav_home,&reicon_nav_thermometer,&reicon_nav_devices,&reicon_nav_workflow,&reicon_nav_triangle_alert,&reicon_nav_settings};
    rail=box(root,18,18,88,564,RAIL,18); txt(rail,26,28,40,16,"SH",F10,CREAM); txt(rail,25,76,40,16,"01",F12,METAL); box(rail,25,142,35,1,0x69645B,0);
    for(int i=0;i<PAGE_COUNT;i++){
        nav[i]=click(rail,20,168+i*52,48,40,RAIL,4);
        lv_obj_t*icon=lv_image_create(nav[i]); lv_obj_set_size(icon,24,24); lv_obj_set_pos(icon,12,8);
        lv_image_set_src(icon,icons[i]); nav_icon_set(i,i==0);
        lv_obj_add_event_cb(nav[i],nav_cb,LV_EVENT_CLICKED,(void*)(intptr_t)i);
    }
    box(rail,36,532,16,16,RAIL,8); box(rail,41,537,6,6,METAL,3);
}
static void build_modal(void){
    modal=box(root,0,0,TK_CANVAS_W,TK_CANVAS_H,0x0C0C0B,0);
    lv_obj_set_style_bg_opa(modal,190,0);
    lv_obj_add_flag(modal,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t*c=box(modal,282,182,560,236,SURFACE,16);
    modal_title=txt(c,30,28,500,32,"",F28,INK);
    line(c,30,96,512);
    modal_body=txt(c,30,124,500,44,"",F18,INK);
    lv_obj_t*x=click(c,330,178,96,44,ALT,22);
    { lv_obj_t*l=tlab(x,0,12,96,20,K_MD_CANCEL,F14,INK); align_center(l); }
    lv_obj_add_event_cb(x,modal_cancel,LV_EVENT_CLICKED,NULL);
    lv_obj_t*o=click(c,438,178,96,44,INK,22);
    { lv_obj_t*l=tlab(o,0,12,96,20,K_MD_OK,F14,CREAM); align_center(l); }
    lv_obj_add_event_cb(o,modal_ok,LV_EVENT_CLICKED,NULL);
}

/* ════════════════════════════════════════════════════
 *  14. 公共 API
 * ════════════════════════════════════════════════════ */
void ui_pages_set_fonts(const lv_font_t *cn14, const lv_font_t *cn16, const lv_font_t *cn18, const lv_font_t *cn24, const lv_font_t *cn32, const lv_font_t *cn48){runtime_cn14=cn14;runtime_cn16=cn16;runtime_cn18=cn18;runtime_cn24=cn24;runtime_cn32=cn32;runtime_cn48=cn48;set_fallbacks();}
void ui_pages_set_latin_font(const lv_font_t *latin){runtime_latin=latin;set_fallbacks();}
void ui_pages_load_sd_fonts(const char *mount_point){
    static const char *names[6]={"fontcn14.bin","fontcn16.bin","fontcn18.bin","fontcn24.bin","fontcn32.bin","fontcn48.bin"};
    const char *base=(mount_point&&mount_point[0])?mount_point:"/sdcard";
    const lv_font_t **slots[6]={&runtime_cn14,&runtime_cn16,&runtime_cn18,&runtime_cn24,&runtime_cn32,&runtime_cn48};
    for(int i=0;i<6;i++){
        char path[160];
        snprintf(path,sizeof(path),"%s/fonts/cn_font/%s",base,names[i]);
        FILE *f=fopen(path,"rb");
        if(!f){ESP_LOGW(TAG,"font missing: %s",path);continue;}
        if(fseek(f,0,SEEK_END)!=0){fclose(f);continue;}
        long size=ftell(f); rewind(f);
        if(size<=0 || size>(long)(16*1024*1024)){fclose(f);ESP_LOGW(TAG,"font invalid size: %s",path);continue;}
        uint8_t *data=(uint8_t*)malloc((size_t)size);
        if(!data || fread(data,1,(size_t)size,f)!=(size_t)size){free(data);fclose(f);ESP_LOGW(TAG,"font read failed: %s",path);continue;}
        fclose(f);
        lv_font_t *font=lv_binfont_create_from_buffer(data,(uint32_t)size);
        if(!font){free(data);ESP_LOGW(TAG,"font parse failed: %s",path);continue;}
        *slots[i]=font;
        runtime_font_data[i]=data; runtime_font_obj[i]=font;
        ESP_LOGI(TAG,"loaded %s (%ld bytes)",path,size);
    }
}
void ui_pages_create(lv_obj_t*p){
    root=p;
    lv_obj_set_style_bg_color(p,lv_color_hex(0x0C0C0B),0);
    lv_obj_set_style_bg_opa(p,LV_OPA_COVER,0);
    F10=runtime_cn14?runtime_cn14:fallback_cn14();
    F12=runtime_cn14?runtime_cn14:fallback_cn14();
    F14=runtime_cn14?runtime_cn14:fallback_cn14();
    F18=runtime_cn18?runtime_cn18:&ui_font_cjk_18;
    F28=runtime_cn24?runtime_cn24:&ui_font_cjk_24;
    F48=runtime_cn48?runtime_cn48:&ui_font_cjk_48;
    set_fallbacks();
    lang_load();
    screen=box(root,106,18,900,564,SURFACE,18);
    build_rail();
    for(int i=0;i<PAGE_COUNT;i++){
        pages[i]=lv_obj_create(screen); lv_obj_remove_style_all(pages[i]);
        lv_obj_set_pos(pages[i],0,0); lv_obj_set_size(pages[i],900,564);
        lv_obj_set_style_bg_opa(pages[i],LV_OPA_TRANSP,0);
        lv_obj_clear_flag(pages[i],LV_OBJ_FLAG_SCROLLABLE);
    }
    for(int i=0;i<SUB_COUNT;i++){
        subs[i]=lv_obj_create(screen); lv_obj_remove_style_all(subs[i]);
        lv_obj_set_pos(subs[i],0,0); lv_obj_set_size(subs[i],900,564);
        lv_obj_set_style_bg_color(subs[i],lv_color_hex(SURFACE),0);
        lv_obj_set_style_bg_opa(subs[i],LV_OPA_COVER,0);
        lv_obj_clear_flag(subs[i],LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(subs[i],LV_OBJ_FLAG_HIDDEN);
    }
    overview(pages[0]); environment(pages[1]); devices(pages[2]);
    automation(pages[3]); alerts(pages[4]); settings(pages[5]);
    build_subs(); build_modal();
    toast=box(root,380,548,264,36,INK,18);
    txt(toast,12,9,240,18,"",F14,CREAM);
    lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN);
    apply_lang();
    page_set(0);
    lv_timer_create(live_refresh_cb,1000,NULL);
    live_refresh_cb(NULL);
}
void ui_pages_set_radial_background(bool e){(void)e;}
void ui_pages_debug_refresh(int times){ for(int i=0;i<times;i++) live_refresh_cb(NULL); }
void ui_pages_debug_show_device(uint8_t id){(void)id;page_set(PAGE_DEVICES);}
void ui_pages_debug_show_page(uint8_t id){
    if(id<PAGE_COUNT){ page_set((int)id); return; }
    /* 6 / 7 = 两个下级界面（供 PC 模拟器逐屏截图自检） */
    if(id==PAGE_SETTINGS+1){ sub_show(SUB_NETWORK); return; }
    if(id==PAGE_SETTINGS+2){ sub_show(SUB_DISPLAY); return; }
    if(id==PAGE_SETTINGS+3){ sub_show(SUB_DEVICES); return; }
}
const lv_font_t *ui_pages_font(uint8_t size){
    switch(size){
        case 16: return runtime_cn16?runtime_cn16:fallback_cn16();
        case 18: return runtime_cn18?runtime_cn18:&ui_font_cjk_18;
        case 24: return runtime_cn24?runtime_cn24:&ui_font_cjk_24;
        case 32: return runtime_cn32?runtime_cn32:&ui_font_cjk_24;
        case 48: return runtime_cn48?runtime_cn48:&ui_font_cjk_48;
        case 14:
        default: return runtime_cn14?runtime_cn14:fallback_cn14();
    }
}
