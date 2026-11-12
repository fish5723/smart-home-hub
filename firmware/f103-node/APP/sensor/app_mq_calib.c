/**
 * @file app_mq_calib.c
 * @brief MQ 传感器标定模块实现 — Rs/R0 + log-log 浓度估算
 *
 * 阶段 1（T-02）：核心标定逻辑。
 * 不引入浮点库以外的依赖（不依赖 math.h 的 log/pow，用查表 + 线性插值）。
 *
 * 来源：本工程新建
 */
#include "app_mq_calib.h"
#include "bsp_adc.h"
#include "bsp_eeprom.h"
#include "app_data.h"
#include "stdio.h"
#include <string.h>
#include "fmt_fixed.h"
#include "mq_math.h"

/* ════════════════════════════════════════════════════
 *  内部工具：对数近似
 *
 *  F103 的 -lm 在小 flash 下体积不小，这里对 MQ 传感器
 *  有限的浓度范围做查表 + 分段线性插值。
 *  ════════════════════════════════════════════════════ */

/**
 * ══════════════════════════════════════════════════════════════════
 *  数学层已抽到 common/mq_math.h（可主机端单测）
 *
 *  阶段 1 验收整改（报告 §2.2 第 1 项）：原先这里有两张定长查表，
 *  定义域分别是 log10 的 x∈[0.01,100] 与 pow10 的 y∈[-2,2]，
 *  而 MQ-4 的曲线端点是 200 / 5000 ppm（log10 = 2.30 / 3.70），
 *  双双被夹到 2.0 → log_c1 == log_c2 → 浓度恒为 100.0 ppm。
 *  现已改为无定义域上限的实现，单测见 tools/test_mq_math.c。
 * ══════════════════════════════════════════════════════════════════
 */

/* ════════════════════════════════════════════════════
 *  MQ 型号参数（数据手册典型值）
 *
 *  每条曲线取 (C, Rs/R0) 两个端点做 log-log 插值。
 *  ─ 端点 1: 低浓度端
 *  ─ 端点 2: 高浓度端
 *  ════════════════════════════════════════════════════ */
typedef struct {
    float c1;       /**< 低浓度 (ppm)   */
    float r1;       /**< 低浓度 Rs/R0   */
    float c2;       /**< 高浓度 (ppm)   */
    float r2;       /**< 高浓度 Rs/R0   */
} mq_sensitivity_t;

static const mq_sensitivity_t g_mq_sens[] = {
    /* MQ-4: CH4 甲烷，200~5000 ppm 段 */
    [MQ_TYPE_MQ4]   = { .c1 = 200.0f, .r1 = 0.55f,
                        .c2 = 5000.0f, .r2 = 0.15f },
    /* MQ-135: NH3 氨气，作为空气质量综合指标 */
    [MQ_TYPE_MQ135] = { .c1 = 100.0f,  .r1 = 0.65f,
                        .c2 = 1000.0f, .r2 = 0.15f },
};

/* ════════════════════════════════════════════════════
 *  公共 API
 *  ════════════════════════════════════════════════════ */

/* 该型号是否启用社区口径（编译期决定；未启用的型号走 24h 老化流程） */
static uint8_t mq_community_enabled(mq_type_t type)
{
    switch (type) {
#if MQ4_USE_COMMUNITY_CURVE
    case MQ_TYPE_MQ4:   return 1U;
#endif
#if MQ135_USE_COMMUNITY_CURVE
    case MQ_TYPE_MQ135: return 1U;
#endif
    default:            return 0U;
    }
}

void APP_MQ_Calib_Init(mq_calib_t *calib, mq_type_t type,
                       float divider, uint16_t rl_ohm)
{
    memset(calib, 0, sizeof(*calib));
    calib->type             = type;
    calib->divider_ratio    = divider;
    calib->load_resistor_ohm = rl_ohm;
    calib->state            = MQ_CALIB_WARMUP;
    calib->rs_r0            = 1.0f;
    calib->concentration    = 0.0f;

    /* 启动时把"当前生效的标定参数"打出来 ——
     * 避免一种很隐蔽的误判：以为设备在按 24h 老化，其实烧的是快速模式固件。 */
    char s_div[12];
    fmt_fixed(s_div, sizeof(s_div), divider, 3);
    printf("[MQ%u] init: divider=%s RL=%uohm warmup=%lus aging=%lus%s\r\n",
           (unsigned)type, s_div, (unsigned)rl_ohm,
           (unsigned long)(MQ_CALIB_WARMUP_MS / 1000UL),
           (unsigned long)(MQ_CALIB_AGING_MS / 1000UL),
           MQ_CALIB_FAST_MODE ? "  [FAST MODE]" : "");

    if (MQ_DIVIDER_IS_PLACEHOLDER(divider)) {
        printf("[MQ%u] WARN: 分压比仍为占位值 1.0 —— Rs / Rs-R0 / ppm 全部不可信！"
               " 实测后填入 MQ_DIVIDER_MQ4 / MQ_DIVIDER_MQ135（见 app_mq_calib.h）\r\n",
               (unsigned)type);
    }

    /* MQ-4 / MQ-135：社区口径（R0 由运行期基线自跟踪），跳过 24h 老化。 */
    if (mq_community_enabled(type)) {
        calib->r0_community = 1;
        const float ratio = (type == MQ_TYPE_MQ4) ? MQ4_CLEAN_AIR_RATIO
                                                  : MQ135_CLEAN_AIR_RATIO;
        const float a = (type == MQ_TYPE_MQ4) ? MQ4_CURVE_A : MQ135_CURVE_A;
        const float b = (type == MQ_TYPE_MQ4) ? MQ4_CURVE_B : MQ135_CURVE_B;
        printf("[MQ%u] community curve: R0=Rs(air)/%.1f, ppm=%.1f*ratio^%.3f — 跳过 24h 老化\r\n",
               (unsigned)type, ratio, a, b);
    }
}

/* ════════════════════════════════════════════════════
 *  R0 持久化（阶段 1 验收整改：报告 §2.3）
 *
 *  R0 是传感器在洁净空气中的固有属性，24h 老化才稳定 ——
 *  每次上电重来一遍毫无意义（而且大多数时候环境并不"洁净"，
 *  重标的 R0 反而会漂）。这里把标定结果写进 EEPROM，上电读回。
 *
 *  注意仍然执行预热：R0 是器件属性，预热是每次上电的物理需求。
 * ════════════════════════════════════════════════════ */
static uint16_t mq_r0_base(mq_type_t type)
{
    return (type == MQ_TYPE_MQ135) ? EEPROM_MQ_R0_BASE_MQ135
                                   : EEPROM_MQ_R0_BASE_MQ4;
}

uint8_t APP_MQ_Calib_SaveR0(const mq_calib_t *calib)
{
    if (calib == NULL || calib->r0_adc == 0U) return 0;

    uint8_t rec[EEPROM_MQ_R0_LEN];
    rec[0] = EEPROM_MQ_R0_MAGIC_HI;
    rec[1] = EEPROM_MQ_R0_MAGIC_LO;
    rec[2] = (uint8_t)(calib->r0_adc & 0xFFU);
    rec[3] = (uint8_t)((calib->r0_adc >> 8) & 0xFFU);
    rec[4] = (uint8_t)(rec[0] ^ rec[1] ^ rec[2] ^ rec[3]);

    if (BSP_EEPROM_Write(mq_r0_base(calib->type), rec, EEPROM_MQ_R0_LEN) != 0U) {
        printf("[MQ%u] R0 持久化失败（EEPROM 写入错误），本次仍可正常测量\r\n",
               (unsigned)calib->type);
        return 0;
    }

    printf("[MQ%u] R0 saved to EEPROM: adc=%u\r\n",
           (unsigned)calib->type, calib->r0_adc);
    return 1;
}

uint8_t APP_MQ_Calib_LoadR0(mq_calib_t *calib)
{
    if (calib == NULL) return 0;

    /* 社区口径：R0 由运行期基线自跟踪，不使用 EEPROM 里的旧标定值 */
    if (calib->r0_community) {
        return 0;
    }

    uint8_t rec[EEPROM_MQ_R0_LEN];
    if (BSP_EEPROM_Read(mq_r0_base(calib->type), rec, EEPROM_MQ_R0_LEN) != 0U) {
        return 0;                       /* EEPROM 未接或读失败 → 走完整标定 */
    }

    if (rec[0] != EEPROM_MQ_R0_MAGIC_HI || rec[1] != EEPROM_MQ_R0_MAGIC_LO) {
        return 0;                       /* 无有效记录 */
    }

    if ((uint8_t)(rec[0] ^ rec[1] ^ rec[2] ^ rec[3]) != rec[4]) {
        printf("[MQ%u] R0 记录校验失败，忽略（将重新标定）\r\n", (unsigned)calib->type);
        return 0;
    }

    calib->r0_adc = (uint16_t)(rec[2] | ((uint16_t)rec[3] << 8));

    /* 防除零：必须在 Init 之后调用（Init 会填入 divider_ratio） */
    const float div = (calib->divider_ratio > 0.0001f) ? calib->divider_ratio : 1.0f;
    calib->r0_mv     = (float)BSP_ADC_RawToMv(calib->r0_adc) / div;
    calib->r0_loaded = 1;

    printf("[MQ%u] R0 restored from EEPROM: adc=%u（跳过老化，仅执行预热）\r\n",
           (unsigned)calib->type, calib->r0_adc);
    return 1;
}

void APP_MQ_Calib_SetR0(mq_calib_t *calib, uint16_t r0_adc)
{
    calib->r0_adc = r0_adc;
    calib->r0_mv  = (float)BSP_ADC_RawToMv(r0_adc);
    calib->state  = MQ_CALIB_READY;
}

void APP_MQ_Calib_ForceCalibrate(mq_calib_t *calib)
{
    if (calib->state == MQ_CALIB_AGING) {
        calib->state        = MQ_CALIB_R0_READY;
        calib->sample_count = 0;
        calib->r0_sum_adc   = 0;
    }
}

static float mq_calc_rs(float vout_sensor_mv, uint16_t rl_ohm)
{
    /* Rs = RL × (Vc / Vout - 1) */
    if (vout_sensor_mv < 1.0f) return 1e7f; /* 防除零 */
    if (vout_sensor_mv >= (float)MQ_CALIB_VC_MV) return 0.0f;
    return (float)rl_ohm * ((float)MQ_CALIB_VC_MV / vout_sensor_mv - 1.0f);
}

static float mq_calc_rs_r0_ratio(uint16_t raw_adc, uint16_t r0_adc,
                                  float divider, uint16_t rl_ohm)
{
    if (r0_adc == 0) return 1.0f;

    float vout_pin   = (float)BSP_ADC_RawToMv(raw_adc);
    float vout_sensor= vout_pin / divider;             /* 补偿分压 */
    float vout_r0_pin= (float)BSP_ADC_RawToMv(r0_adc);
    float vout_r0    = vout_r0_pin / divider;

    float rs   = mq_calc_rs(vout_sensor, rl_ohm);
    float rs_r0_val = mq_calc_rs(vout_r0, rl_ohm);

    if (rs_r0_val < 1.0f) rs_r0_val = 1.0f;
    return rs / rs_r0_val;
}

static float mq_ppm_from_curve(float rs_r0, const mq_sensitivity_t *s)
{
    /* 算法在 common/mq_math.h（与主机端单测共用同一份实现） */
    return mq_ppm_from_rs_r0(rs_r0, s->c1, s->r1, s->c2, s->r2);
}

/* 社区公开参数回归：ppm = A × ratio^B（ratio = Rs / R0）
 *   MQ-4  : A=1012.7, B=-2.786（R0=1000ppm CH4 下的 Rs）
 *   MQ-135: A=116.602, B=-2.769（R0=100ppm NH3 下的 Rs）
 * 用 log10 域实现，避免引入 powf：log10(ppm) = log10(A) + B·log10(ratio) */
static float mq_ppm_from_community_curve(mq_type_t type, float ratio)
{
    if (ratio <= 0.0f) return 10000.0f;
    const float a = (type == MQ_TYPE_MQ4) ? MQ4_CURVE_A : MQ135_CURVE_A;
    const float b = (type == MQ_TYPE_MQ4) ? MQ4_CURVE_B : MQ135_CURVE_B;
    float log_ppm = mq_log10(a) + b * mq_log10(ratio);
    if (log_ppm < 0.0f) log_ppm = 0.0f;
    if (log_ppm > 4.0f) log_ppm = 4.0f;     /* 1 ~ 10000 ppm */
    return mq_pow10(log_ppm);
}

void APP_MQ_Calib_Process(mq_calib_t *calib, uint16_t raw_adc, uint16_t pin_mv)
{
    if (!calib) return;

    /* ── 累计通电时间 ── */
    calib->power_on_ms += 1000U;

    /* ── 计算 Rs（用于调试输出，不作判定依据） ── */
    float vout_pin   = (float)pin_mv;
    float vout_sensor= vout_pin / calib->divider_ratio;
    float rs         = mq_calc_rs(vout_sensor, calib->load_resistor_ohm);

    /* ── 状态机 ── */
    switch (calib->state) {

    case MQ_CALIB_WARMUP:
        if (calib->power_on_ms >= MQ_CALIB_WARMUP_MS) {
            calib->warmup_done = 1;
            if (calib->r0_loaded) {
                /* R0 来自 EEPROM：跳过老化与 R0 采样，预热一结束即可用 */
                calib->state = MQ_CALIB_READY;
                printf("[MQ%u] warmup done → READY (R0 来自 EEPROM)\r\n",
                       (unsigned)calib->type);
            } else if (calib->r0_community) {
                /* 社区口径：不做 24h 老化；R0 由运行期 Rs 峰值自动跟踪 */
                calib->state = MQ_CALIB_READY;
                printf("[MQ%u] warmup done → READY（基线自跟踪，无需老化）\r\n",
                       (unsigned)calib->type);
            } else {
                calib->state = MQ_CALIB_AGING;
                printf("[MQ%u] warmup done → aging %lus%s\r\n",
                       (unsigned)calib->type,
                       (unsigned long)(MQ_CALIB_AGING_MS / 1000UL),
                       MQ_CALIB_FAST_MODE ? "  [FAST MODE — 仅调试用，出货须用 Release]" : "");
            }
        }
        break;

    case MQ_CALIB_AGING:
        if (calib->power_on_ms >= (MQ_CALIB_WARMUP_MS + MQ_CALIB_AGING_MS)) {
            calib->state        = MQ_CALIB_R0_READY;
            calib->sample_count = 0;
            calib->r0_sum_adc   = 0;
            printf("[MQ] aging done → ready for R0 sampling\r\n");
        }
        break;

    case MQ_CALIB_R0_READY:
        /* 连续采样 R0 (洁净空气中) */
        calib->r0_sum_adc += raw_adc;
        calib->sample_count++;
        if (calib->sample_count >= MQ_CALIB_R0_SAMPLES) {
            calib->r0_adc = (uint16_t)(calib->r0_sum_adc / MQ_CALIB_R0_SAMPLES);
            calib->r0_mv  = (float)BSP_ADC_RawToMv(calib->r0_adc) / calib->divider_ratio;
            calib->state  = MQ_CALIB_READY;
            char s_vout[12];
            fmt_fixed(s_vout, sizeof(s_vout), calib->r0_mv, 1);
            printf("[MQ] R0 calibrated: ADC=%u  Vout=%smV\r\n",
                   calib->r0_adc, s_vout);
            /* 持久化：下次上电不必再等 24h 老化（§2.3） */
            (void)APP_MQ_Calib_SaveR0(calib);
        }
        break;

    case MQ_CALIB_READY:
        if (calib->r0_community) {
            /* 基线自跟踪：Rs 峰值视作空气基线，只缓慢衰减（约 -2%/小时）。
             * 加热漂移、温湿度变化都会被吸收；有燃气时 Rs 下降 → ppm 上升。 */
            if (calib->rs_peak <= 0.0f) {
                calib->rs_peak = rs;
            } else {
                calib->rs_peak *= 0.99999f;
                if (rs > calib->rs_peak) calib->rs_peak = rs;
            }
            if (calib->rs_peak < 1.0f) calib->rs_peak = 1.0f;

            const float clean_ratio = (calib->type == MQ_TYPE_MQ4)
                                      ? MQ4_CLEAN_AIR_RATIO : MQ135_CLEAN_AIR_RATIO;
            calib->r0_ohm = calib->rs_peak / clean_ratio;
            calib->rs_r0  = (rs > 0.0f) ? (rs / calib->r0_ohm) : 0.01f;
            if (calib->rs_r0 < 0.01f) calib->rs_r0 = 0.01f;
            calib->concentration = mq_ppm_from_community_curve(calib->type, calib->rs_r0);
            break;
        }

        /* 正常测量模式：计算 Rs/R0 与浓度 */
        calib->rs_r0 = mq_calc_rs_r0_ratio(raw_adc, calib->r0_adc,
                                             calib->divider_ratio,
                                             calib->load_resistor_ohm);

        /* 查表得到 ppm */
        if (calib->type < sizeof(g_mq_sens) / sizeof(g_mq_sens[0])) {
            calib->concentration = mq_ppm_from_curve(calib->rs_r0,
                                                      &g_mq_sens[calib->type]);
        }
        break;

    default:
        break;
    }

    /* ── 调试输出 ── */
    /* ── 周期性调试输出 ──
     * 阶段 1 验收整改（§2.3 T-14）：这段原先每次 Process（1Hz）都会打印，
     * 且 READY 之后每秒都打一遍 Rs/R0 与浓度 —— 数据密度低、噪声大。
     * 现收到 DBG_MQ_STATE 开关后面（默认关）。
     * 注意：真正有价值的**状态跳转**打印在状态机里（warmup done / aging done /
     * R0 calibrated / R0 saved），那些是低频一次性事件，不受本开关影响。 */
    (void)rs;
#if DBG_MQ_STATE
    switch (calib->state) {
    case MQ_CALIB_WARMUP:
        printf("[MQ%u] WARMUP %lus  adc=%u  mv=%u\r\n",
               (unsigned)calib->type,
               (unsigned long)(calib->power_on_ms / 1000UL),
               raw_adc, pin_mv);
        break;
    case MQ_CALIB_AGING: {
        char s_rs[16];
        fmt_fixed(s_rs, sizeof(s_rs), rs, 0);
        printf("[MQ%u] AGING %luh  adc=%u  mv=%u  Rs=%sΩ\r\n",
               (unsigned)calib->type,
               (unsigned long)((calib->power_on_ms - MQ_CALIB_WARMUP_MS)
                               / 3600000UL),
               raw_adc, pin_mv, s_rs);
        break;
    }
    case MQ_CALIB_R0_READY:
        printf("[MQ%u] R0_SAMP %u/%u  adc=%u\r\n",
               (unsigned)calib->type,
               calib->sample_count, MQ_CALIB_R0_SAMPLES,
               raw_adc);
        break;
    case MQ_CALIB_READY: {
        char s_r0[12], s_c[12];
        fmt_fixed(s_r0, sizeof(s_r0), calib->rs_r0,        2);
        fmt_fixed(s_c,  sizeof(s_c),  calib->concentration, 0);
        printf("[MQ%u] Rs/R0=%s  C=%sppm  adc=%u  mv=%u\r\n",
               (unsigned)calib->type,
               s_r0, s_c,
               raw_adc, pin_mv);
        break;
    }
    }
#endif /* DBG_MQ_STATE */
}

const char *APP_MQ_Calib_StateStr(mq_calib_state_t state)
{
    static const char *names[] = {
        [MQ_CALIB_WARMUP]   = "WARMUP",
        [MQ_CALIB_AGING]    = "AGING",
        [MQ_CALIB_R0_READY] = "R0_RDY",
        [MQ_CALIB_READY]    = "READY",
    };
    if (state > MQ_CALIB_READY) return "?";
    return names[state];
}
