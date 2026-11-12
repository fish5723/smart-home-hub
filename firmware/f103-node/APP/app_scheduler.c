/**
 * @file app_scheduler.c
 * @brief 应用层任务调度 — 从 main.c 提取，职责仅编排不持有逻辑
 *
 * 初始化顺序：
 *   1. 打印复位原因（T-05）
 *   2. 延时/串口重定向
 *   3. 掉电计数（T-06）
 *   4. 蜂鸣器 + PIR EXTI（T-13）+ 灌溉（T-12）
 *   5. OLED 初始化
 *   6. BMP280 传感器初始化
 *   7. 看门狗启动（T-04）+ 开机蜂鸣
 *
 * 主循环调度：
 *   喂狗 → 传感器采集 → 显示 → 控制 → 通信
 *
 * 来源：本工程新建（阶段 1 重构）
 */
#include "app_scheduler.h"
#include "main.h"
#include "usart.h"
#include "i2c.h"
#include "retarget.h"
#include "delay.h"
#include <stdio.h>
#include <string.h>

/* ── BSP ── */
#include "bsp_beep.h"
#include "bsp_iwdg.h"
#include "bsp_eeprom.h"
#include "bsp_ssd1306.h"
#include "bsp_ssd1306_fonts.h"
#include "bsp_human.h"

/* ── Sensor ── */
#include "app_bmp280.h"
#include "app_dht11.h"
#include "app_human.h"
#include "app_soil.h"
#include "app_mq4.h"
#include "app_air.h"
#include "app_rain.h"

/* ── Control ── */
#include "app_beep.h"
#include "app_irrigation.h"
#include "act_manager.h"

/* ── Display ── */
#include "app_monitor.h"

/* ── Comm ── */
#include "app_uart.h"
#include "proto_uart.h"
#include "app_proto.h"

/* ── OTA / IAP ── */
#include "app_f103_ota.h"

/* ════════════════════════════════════════════════════
 *  任务追踪（noinit 区，存活于 IWDG/WWDG/软复位）
 *  启动时打印上次崩溃前执行到哪个任务。
 *  宏定义见 app_scheduler.h （各模块可引用）。
 *  ════════════════════════════════════════════════════ */

static const char *sched_trace_name(uint32_t id)
{
    /* 表只覆盖 0..SCHED_TRACE_PROTO（22 项）；IDLE=99 单列，避免稀疏数组被撑到 100 项。 */
    static const char *names[] = {
        [SCHED_TRACE_DHT11]        = "DHT11",
        [SCHED_TRACE_DHT11_READ]   = "DHT11_READ(irq_off)",
        [SCHED_TRACE_HUMAN]        = "HUMAN",
        [SCHED_TRACE_SOIL]         = "SOIL",
        [SCHED_TRACE_MQ4]          = "MQ-4",
        [SCHED_TRACE_AIR]          = "AIR",
        [SCHED_TRACE_RAIN]         = "RAIN",
        [SCHED_TRACE_BMP280]       = "BMP280",
        [SCHED_TRACE_BEEP]         = "BEEP",
        [SCHED_TRACE_MONITOR]      = "MONITOR",
        [SCHED_TRACE_IRR]          = "IRR",
        [SCHED_TRACE_UART]         = "UART",
        [SCHED_TRACE_PROTO]        = "PROTO",
    };
    /* IDLE 原先不在表里：真正卡在主循环体的崩溃只会打印成 "?"（实测出现过一次）。 */
    if (id == SCHED_TRACE_IDLE) return "IDLE(main-loop)";
    /* 只认表里有的；返回 NULL = 无记录（.noinit 复位后可能是随机值）。 */
    if (id > SCHED_TRACE_NONE && id < sizeof(names)/sizeof(names[0]) && names[id])
        return names[id];
    return NULL;
}

__attribute__((section(".noinit"))) volatile uint32_t g_sched_trace;
__attribute__((section(".noinit"))) volatile uint32_t g_app_boot_stage;

#define BOOT_STAGE(n) do { g_app_boot_stage = (n); } while (0)

void App_Scheduler_Init(void)
{
    BOOT_STAGE(11);
    delay_init();

    /* ── 复位原因诊断（T-05）：必须在 IWDG 初始化之前读取 ── */
    {
        char rst_buf[256];
        BSP_IWDG_GetResetReasonStr(rst_buf, sizeof(rst_buf));
        printf("%s", rst_buf);
    }

    /* ── 看门狗（T-04）：2s 窗口
     * 注意启动时机（阶段 1 验收整改 §3.1）：CubeMX 的 MX_IWDG_Init() 已在 main.c 中
     * 刻意移除，避免它以约 410ms 的窗口在初始化早期生效又被误复位。
     * 从这一行起看门狗生效，因此下面的每一步初始化之间都要喂狗 ——
     * OLED/EEPROM 在器件缺失或总线卡住时单次 I2C 超时可达 50ms，
     * 若连续多次超时，累加就可能逼近 2s 窗口。 */
    BOOT_STAGE(12);
    uint32_t iwdg_ms = BSP_IWDG_Init(2000);
    printf("[APP] IWDG started  actual=%lums\r\n", (unsigned long)iwdg_ms);
    BSP_IWDG_Feed();

    /* ── 掉电计数（T-06）── */
    BOOT_STAGE(13);
    APP_PowerCounter_Boot();
    BSP_IWDG_Feed();

    /* ── 阶段 8: IAP 存储自检（W25Q64 JEDEC ID）── */
    BOOT_STAGE(14);
    AppF103Ota_Init();

    /* ── PIR EXTI（T-13）── */
    BOOT_STAGE(15);
    BSP_HUMAN_Init();
    BSP_IWDG_Feed();

    /* ── OLED 显示（I2C 命令最多，最需要喂狗）── */
    BOOT_STAGE(16);
    ssd1306_Init();
    BSP_IWDG_Feed();

    /* ── 传感器 ── */
    BOOT_STAGE(17);
    APP_BMP280_Init();
    BSP_IWDG_Feed();

    /* ── 开机蜂鸣 + 执行器管理器（必须先初始化蜂鸣器 BSP）── */
    BOOT_STAGE(18);
    APP_BEEP_Init();
    act_manager_init();
    /* ── 灌溉控制（T-12）── */
    APP_IRR_Init();
    APP_BEEP_Alarm();
    BSP_IWDG_Feed();

    /* ── 协议栈（阶段 2A: USART1 DMA+IDLE）── */
    BOOT_STAGE(19);
    ProtoUart_Init();
    AppProto_Init();
    BOOT_STAGE(20);
    printf("[APP] PROTO UART1 DMA+IDLE ready  buf=%uB\r\n", 1024U);
    BSP_IWDG_Feed();

    /* ── 上次崩溃点追踪 ──
     * g_sched_trace 在 .noinit 区（跨复位存活）；首次上电或复位后其内容可能是
     * 随机值。这里只认"落在名字表里的真实记录"，否则当作无记录，不再打印
     * "CRASH in: ?" 这种无法归因的假告警。 */
    {
        const char *trace = sched_trace_name(g_sched_trace);
        if (g_sched_trace != SCHED_TRACE_NONE && trace)
            printf("[APP] *** CRASH in: %s ***\r\n", trace);
    }
    g_sched_trace = SCHED_TRACE_NONE;
    printf("=== APP SCHEDULER READY ===\r\n");
}

/* ════════════════════════════════════════════════════
 *  任务耗时统计（阶段 1 验收整改：报告 §3.3）
 *
 *  原调度器只有"顺序编排 + 崩溃点追踪"，没有执行时间观测。而阶段 2 的协议
 *  需要量化主循环的最坏延迟来设定 ACK 超时（规范草案曾写 50ms，
 *  实测后定稿 100ms —— 见 docs/03-通信协议规范.md §6.1）。
 *  这里给每个任务记录"上次 / 历史最大"耗时，并统计整个主循环的耗时。
 *
 *  统计开销：两次 DWT 读 + 一次减法，可忽略。
 *  ⚠️ 单任务耗时以 uint16 µs 记录，超过 65ms 截断（正常任务都在 10ms 以内）。
 * ════════════════════════════════════════════════════ */
typedef enum {
    SCHED_ID_PROTO = 0,   /* 阶段 2: 协议收包 — 最高优先级，排在传感器之前 */
    SCHED_ID_BMP280,
    SCHED_ID_DHT11,
    SCHED_ID_HUMAN,
    SCHED_ID_SOIL,
    SCHED_ID_MQ4,
    SCHED_ID_AIR,
    SCHED_ID_RAIN,
    SCHED_ID_MONITOR,
    SCHED_ID_IRR,
    SCHED_ID_BEEP,
    SCHED_ID_UART,
    SCHED_ID_COUNT
} sched_task_id_t;

static const char *const s_task_names[SCHED_ID_COUNT] = {
    "PROTO", "BMP280", "DHT11", "HUMAN", "SOIL", "MQ4", "AIR",
    "RAIN", "MONITOR", "IRR", "BEEP", "UART",
};

static uint16_t s_task_last_us[SCHED_ID_COUNT];
static uint16_t s_task_max_us[SCHED_ID_COUNT];
static uint16_t s_loop_last_us;
static uint16_t s_loop_max_us;

static void sched_proto_run(void)
{
    ProtoUart_Poll();
    AppProto_Poll();
    AppProto_SendReportTask();
}

static void sched_record(sched_task_id_t id, uint32_t dt_us)
{
    uint16_t dt = (dt_us > 65535U) ? 65535U : (uint16_t)dt_us;
    s_task_last_us[id] = dt;
    if (dt > s_task_max_us[id]) s_task_max_us[id] = dt;
}

/** 执行一个任务并记录耗时与崩溃点标记 */
#define RUN_TASK(id, trace, fn)                       \
    do {                                              \
        g_sched_trace = (trace);                      \
        uint32_t _t0 = delay_micros();                \
        fn();                                         \
        sched_record((id), delay_micros() - _t0);     \
    } while (0)

void App_Scheduler_PrintStats(void)
{
    printf("[STAT] task      last_us  max_us\r\n");
    for (uint32_t i = 0; i < (uint32_t)SCHED_ID_COUNT; i++) {
        printf("[STAT] %-8s  %6u  %6u\r\n",
               s_task_names[i],
               (unsigned)s_task_last_us[i],
               (unsigned)s_task_max_us[i]);
    }
    printf("[STAT] %-8s  %6u  %6u\r\n", "LOOP",
           (unsigned)s_loop_last_us, (unsigned)s_loop_max_us);
}

void App_Scheduler_Run(void)
{
    static uint32_t last_hb    = 0;
    static uint32_t last_stats = 0;
    static uint32_t loop_cnt   = 0;
    uint32_t now     = HAL_GetTick();
    uint32_t loop_t0 = delay_micros();

    BSP_IWDG_Feed();

    /* ── 协议收包+发送管理（阶段 2A: 最高优先级）── */
    RUN_TASK(SCHED_ID_PROTO,  SCHED_TRACE_PROTO,  sched_proto_run);

    /* ── 传感器采集 ── */
    RUN_TASK(SCHED_ID_BMP280,  SCHED_TRACE_BMP280,  APP_BMP280_Task);
    RUN_TASK(SCHED_ID_DHT11,   SCHED_TRACE_DHT11,   APP_DHT11_Task);
    RUN_TASK(SCHED_ID_HUMAN,   SCHED_TRACE_HUMAN,   APP_HUMAN_Task);
    RUN_TASK(SCHED_ID_SOIL,    SCHED_TRACE_SOIL,    APP_SOIL_Task);
    RUN_TASK(SCHED_ID_MQ4,     SCHED_TRACE_MQ4,     APP_MQ4_Task);
    RUN_TASK(SCHED_ID_AIR,     SCHED_TRACE_AIR,     APP_AIR_Task);
    RUN_TASK(SCHED_ID_RAIN,    SCHED_TRACE_RAIN,    APP_RAIN_Task);

    /* ── 显示 ── */
    RUN_TASK(SCHED_ID_MONITOR, SCHED_TRACE_MONITOR, APP_MONITOR_Task);

    /* ── 控制 ── */
    RUN_TASK(SCHED_ID_IRR,     SCHED_TRACE_IRR,     APP_IRR_Task);
    RUN_TASK(SCHED_ID_BEEP,    SCHED_TRACE_BEEP,    APP_BEEP_Task);

    /* ── 通信 ── */
    RUN_TASK(SCHED_ID_UART,    SCHED_TRACE_UART,    APP_Uart_Task);

    /* 门锁（PB8）已无应用层任务：所用模块与设计不符，已删除
     * app_lock/bsp_lock（2026-10-18）。PB8 仍由 act_manager 独占写，
     * 默认保持"锁定"电平；P4 侧 dev=0x03 槽位保留但无实物接入。 */

    /* 崩溃点标记改为 IDLE（§3.3）：复位后若读到 IDLE，说明崩溃发生在
     * 主循环的循环体里（心跳/统计打印），而不是某个具体任务里。 */
    g_sched_trace = SCHED_TRACE_IDLE;

    uint32_t loop_us = delay_micros() - loop_t0;
    s_loop_last_us = (loop_us > 65535U) ? 65535U : (uint16_t)loop_us;
    if (s_loop_last_us > s_loop_max_us) s_loop_max_us = s_loop_last_us;

    /* 心跳：5s 一次（原为 1s，属调试残留；见 §2.3 T-14）。带上最大循环耗时 —— 
     * 这是阶段 2 设定协议 ACK 超时的直接依据。 */
    loop_cnt++;
    if (now - last_hb >= 5000U) {
        last_hb = now;
        printf("[HB] t=%lus loop=%lu  max_loop=%uus\r\n",
               (unsigned long)(now / 1000U), (unsigned long)loop_cnt,
               (unsigned)s_loop_max_us);
    }

    /* 完整任务耗时表：每 60s 一次 */
    if (now - last_stats >= 60000U) {
        last_stats = now;
        App_Scheduler_PrintStats();
    }
}

#define I2C_HANDLE          (&hi2c1)
#define I2C_TIMEOUT_MS      20U

static uint8_t calc_pc_checksum(const uint8_t *buf)
{
    return buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5];
}

void APP_PowerCounter_Boot(void)
{
    uint8_t buf[EEPROM_PC_MAGIC_LEN];
    uint32_t counter;
    uint8_t  ret;

    printf("[EEP] scan addr=0x%02X ... ", BSP_EEPROM_I2C_ADDR);

    ret = HAL_I2C_IsDeviceReady(I2C_HANDLE,
                                BSP_EEPROM_I2C_ADDR << 1,
                                3, 50U);
    if (ret != HAL_OK) {
        printf("NOT FOUND (ret=%u)\r\n", ret);
        return;
    }
    printf("OK\r\n");

    ret = BSP_EEPROM_Read(EEPROM_PC_MAGIC_BASE, buf, EEPROM_PC_MAGIC_LEN);
    if (ret != 0) {
        printf("[EEP] read fail (ret=%u)\r\n", ret);
        return;
    }

    uint8_t cs_calc = calc_pc_checksum(buf);

    if (buf[EEPROM_OFF_MAGIC_HI] == EEPROM_MAGIC_HI &&
        buf[EEPROM_OFF_MAGIC_LO] == EEPROM_MAGIC_LO &&
        buf[EEPROM_OFF_CHECKSUM]  == cs_calc)
    {
        counter  = (uint32_t)buf[EEPROM_OFF_CTR0];
        counter |= (uint32_t)buf[EEPROM_OFF_CTR1] << 8;
        counter |= (uint32_t)buf[EEPROM_OFF_CTR2] << 16;
        counter |= (uint32_t)buf[EEPROM_OFF_CTR3] << 24;
        counter++;
        printf("[EEP] Power-on #%lu\r\n", (unsigned long)counter);
    }
    else
    {
        printf("[EEP] Blank/dirty — first use\r\n");
        counter = 1;
        printf("[EEP] Power-on #%lu (initialized)\r\n", (unsigned long)counter);
    }

    buf[EEPROM_OFF_MAGIC_HI] = EEPROM_MAGIC_HI;
    buf[EEPROM_OFF_MAGIC_LO] = EEPROM_MAGIC_LO;
    buf[EEPROM_OFF_CTR0]     = (uint8_t)(counter);
    buf[EEPROM_OFF_CTR1]     = (uint8_t)(counter >> 8);
    buf[EEPROM_OFF_CTR2]     = (uint8_t)(counter >> 16);
    buf[EEPROM_OFF_CTR3]     = (uint8_t)(counter >> 24);
    buf[EEPROM_OFF_CHECKSUM] = calc_pc_checksum(buf);

    ret = BSP_EEPROM_Write(EEPROM_PC_MAGIC_BASE, buf, EEPROM_PC_MAGIC_LEN);
    if (ret != 0) {
        printf("[EEP] write fail (ret=%u)\r\n", ret);
    }
}
