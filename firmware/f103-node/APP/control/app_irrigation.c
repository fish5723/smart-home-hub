/**
 * @file app_irrigation.c
 * @brief 双通道灌溉控制（T-12：RELAY PB14 + PUMP PB15 统一逻辑）
 *
 * 物理映射：
 *   - CH0 RELAY  PB14  低电平吸合 / 高电平断开
 *   - CH1 PUMP   PB15  高电平启动 / 低电平停止
 *
 * 控制逻辑（双阈值迟滞）：
 *   土壤 ≤ ON_PERCENT  → 开启
 *   土壤 ≥ OFF_PERCENT → 关闭
 *   下雨 / 传感器无效  → 全部关闭
 *   水泵连续运行 > MAX_SEC → 强制关闭 + 告警（T-18）
 *
 * 来源：本工程新建（合并 app_relay.c + app_pump.c）
 */
#include "app_irrigation.h"
#include "app_data.h"
#include "bsp_pins.h"   /* T-15: 引脚集中 */
#include "act_manager.h"
#include "app_proto.h"
#include "main.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════
 *  可配置参数
 *
 *  双阈值迟滞的参数现在是**运行时可配**的（阶段 1 验收整改：报告 §2.3 T-12
 *  要求"双阈值参数可配"）。默认值如下，用 APP_IRR_SetThresholds() 覆盖。
 *
 *  为什么不做 EEPROM 持久化：阶段 2 的协议里已经有 CONFIG_SET 消息，
 *  配置下发最终会落到这个接口上；现在再加一份 EEPROM 记录等于引入
 *  第二个配置来源，反而容易出现"两处不一致"。
 * ════════════════════════════════════════════════════ */
#define IRR_SOIL_ON_PERCENT_DEFAULT   30U
#define IRR_SOIL_OFF_PERCENT_DEFAULT  45U

#define PUMP_MAX_ON_SEC       30U    /* T-18: 水泵最大连续运行时间（秒）*/
#define IRR_PERIOD_MS         500U   /* 控制周期 */

static uint8_t s_on_percent  = IRR_SOIL_ON_PERCENT_DEFAULT;
static uint8_t s_off_percent = IRR_SOIL_OFF_PERCENT_DEFAULT;

/* ════════════════════════════════════════════════════
 *  驱动层（轻量 GPIO，无 printf）
 *  引脚定义见 BSP/bsp_pins.h
 *  ════════════════════════════════════════════════════ */
static void irr_relay_on(void)
{
    (void)act_manager_command(PROTO_DEV_RELAY1, PROTO_ACTION_ON, 0,
                               ACT_SOURCE_LOCAL, HAL_GetTick());
}

static void irr_relay_off(void)
{
    (void)act_manager_command(PROTO_DEV_RELAY1, PROTO_ACTION_OFF, 0,
                               ACT_SOURCE_LOCAL, HAL_GetTick());
}

static void irr_pump_on(void)
{
    (void)act_manager_command(PROTO_DEV_RELAY2, PROTO_ACTION_ON,
                               PUMP_MAX_ON_SEC * 1000U, ACT_SOURCE_LOCAL,
                               HAL_GetTick());
}

static void irr_pump_off(void)
{
    (void)act_manager_command(PROTO_DEV_RELAY2, PROTO_ACTION_OFF, 0,
                               ACT_SOURCE_LOCAL, HAL_GetTick());
}

/* ════════════════════════════════════════════════════
 *  状态变量
 *  ════════════════════════════════════════════════════ */
static uint8_t  s_relay_on  = 0;
static uint8_t  s_pump_on   = 0;
static uint32_t s_pump_start_tick = 0;
static uint8_t  s_pump_alarm = 0;   /* T-18: 水泵干烧告警（锁存） */
static uint32_t s_pump_alarm_count = 0;  /* 累计触发次数（观测用） */

/* ════════════════════════════════════════════════════
 *  初始化
 *  ════════════════════════════════════════════════════ */
void APP_IRR_Init(void)
{
    irr_relay_off();
    irr_pump_off();
    s_relay_on  = 0;
    s_pump_on   = 0;
    s_pump_alarm = 0;
    s_pump_alarm_count = 0;
    g_sensor_data.pump_on    = 0;
    g_sensor_data.pump_alarm = 0;
}

/* ════════════════════════════════════════════════════
 *  灌溉任务（T-12 + T-18）
 *  ════════════════════════════════════════════════════ */
/* ════════════════════════════════════════════════════
 *  阈值配置接口（T-12 整改：报告 §2.3）
 * ════════════════════════════════════════════════════ */
uint8_t APP_IRR_SetThresholds(uint8_t on_percent, uint8_t off_percent)
{
    /* 合法性：必须 0 < on < off <= 100。
     * 若 on >= off，迟滞区间消失，开/关判定会在同一轮里互相取消，
     * 表现为继电器高频抖动 —— 所以宁可拒绝也不接受非法组合。 */
    if (on_percent == 0U || off_percent > 100U || on_percent >= off_percent) {
        printf("[IRR] thresholds rejected (on=%u off=%u)：需满足 0 < on < off <= 100\r\n",
               (unsigned)on_percent, (unsigned)off_percent);
        return 0;
    }
    s_on_percent  = on_percent;
    s_off_percent = off_percent;
    printf("[IRR] thresholds set: 开<=%u%%  关>=%u%%\r\n",
           (unsigned)on_percent, (unsigned)off_percent);
    return 1;
}

uint8_t APP_IRR_GetOnThreshold(void)  { return s_on_percent; }
uint8_t APP_IRR_GetOffThreshold(void) { return s_off_percent; }

void APP_IRR_Task(void)
{
    static uint32_t last_tick = 0;
    uint32_t now = HAL_GetTick();

    if (now - last_tick < IRR_PERIOD_MS)
        return;
    last_tick = now;

#if !APP_IRR_LOCAL_FALLBACK_ENABLE
    /* 设备尚未接入：保持本地灌溉输出安全关闭，避免土壤 0% 在
     * LINK_DOWN 时启动不存在的水泵并产生误导性的超时告警。P4 的
     * 显式 ACTUATOR_CMD 路径不受此本地兜底开关影响。 */
    /* 链路正常时由 P4 的显式命令管理执行器，不能在这里周期性覆盖。 */
    if (AppProto_GetLinkState() != LINK_DOWN)
        return;
    if (s_relay_on) {
        irr_relay_off();
        s_relay_on = 0;
    }
    if (s_pump_on) {
        irr_pump_off();
        s_pump_on = 0;
    }
    s_pump_alarm = 0;
    g_sensor_data.pump_on    = 0;
    g_sensor_data.pump_alarm = 0;
    g_sensor_data.pump_tick  = now;
    return;
#endif

    /* ADR-021: the local irrigation policy is a fallback only.  While the
     * P4↔F103 link is alive, P4's rule engine owns irrigation commands. */
    if (AppProto_GetLinkState() != LINK_DOWN)
        return;

    /* 土壤必须"有效且质量合格"才允许参与灌溉判定。
     * 电阻式土壤探头断线或在空气中时输出接近电源轨，会被误判成"极干(0%)"，
     * 若只检查 valid 就会在无水环境下持续开泵 —— 故这里显式要求 quality == VALID。 */
    uint8_t soil_ok  = g_sensor_data.soil_valid
                    && (g_sensor_data.soil_quality == (uint8_t)SENSOR_QUAL_VALID)
                    && g_sensor_data.rain_valid;
    uint8_t raining  = g_sensor_data.raining;

    /* ── 需求判定：是否"需要"灌溉（双阈值迟滞，不含告警状态）── */
    uint8_t demand = 0;
    if (soil_ok && !raining) {
        if (!s_relay_on && g_sensor_data.soil <= s_on_percent) {
            demand = 1;
        } else if (s_relay_on && g_sensor_data.soil < s_off_percent) {
            demand = 1;   /* 未到关闭阈值，保持开 */
        }
    }

    /* ── 水泵超时保护（T-18）──
     * 阶段 1 验收整改（报告 §2.2 第 3 项）：
     *   原实现是「超时 → 置告警 → 下一轮因 !s_pump_on 立刻清告警 → 再次启动」，
     *   形成 30s 开 / 0.5s 关的无限循环，保护等于形同虚设。
     *   现改为【锁存】语义：
     *     - 超时 → 强制关泵并锁存告警
     *     - 锁存期间禁止再启动
     *     - 只有"不再需要灌溉"（土壤恢复 / 下雨 / 传感器无效）时才自动解除
     *   最坏结果是"水泵停转 + 持续告警"——安全侧失败，
     *   而不是反复冲击水泵与继电器（后者会缩短器件寿命且掩盖真实故障）。 */
    if (s_pump_on && !s_pump_alarm) {
        uint32_t elapsed_sec = (now - s_pump_start_tick) / 1000UL;
        if (elapsed_sec >= PUMP_MAX_ON_SEC) {
            s_pump_alarm = 1;
            s_pump_alarm_count++;
            printf("[IRR] PUMP TIMEOUT %luse → LATCH OFF (count=%lu)\r\n",
                   (unsigned long)elapsed_sec,
                   (unsigned long)s_pump_alarm_count);
        }
    }

    /* 告警解除：仅在"不再需要灌溉"时（锁存语义的关键） */
    if (s_pump_alarm && !demand) {
        s_pump_alarm = 0;
        printf("[IRR] pump alarm cleared (no irrigation demand)\r\n");
    }

    uint8_t should_on = demand && !s_pump_alarm;

    /* ── Relay（PB14 低电平吸合）── */
    if (should_on && !s_relay_on) {
        irr_relay_on();
        s_relay_on = 1;
        printf("[IRR] RELAY ON  (soil=%u%%)\r\n", g_sensor_data.soil);
    } else if (!should_on && s_relay_on) {
        irr_relay_off();
        s_relay_on = 0;
        printf("[IRR] RELAY OFF\r\n");
    }

    /* ── Pump（PB15 高电平启动）── */
    uint8_t pump_should = should_on && !s_pump_alarm;
    if (pump_should && !s_pump_on) {
        irr_pump_on();
        s_pump_on = 1;
        s_pump_start_tick = now;
        printf("[IRR] PUMP ON  (soil=%u%%)\r\n", g_sensor_data.soil);
    } else if (!pump_should && s_pump_on) {
        irr_pump_off();
        s_pump_on = 0;
        printf("[IRR] PUMP OFF\r\n");
    }

    g_sensor_data.pump_on    = s_pump_on;
    g_sensor_data.pump_alarm = s_pump_alarm;
    g_sensor_data.pump_tick  = now;   /* 控制环新鲜度（§3.4）*/
}
