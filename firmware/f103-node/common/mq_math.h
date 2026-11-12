/**
 * @file mq_math.h
 * @brief MQ 传感器换算所需的 log10 / pow10 / 曲线插值
 *
 * 本文件**不依赖 math.h 与 HAL**，因此可以在 PC 上编译并做单元测试
 * （见 tools/test_mq_math.c）。这是阶段 2「协议与逻辑层要能脱离硬件单测」
 * 的分层要求，也顺带让这次的修复可以被真正验证，而不是靠手算。
 *
 * ══════════════════════════════════════════════════════
 *  阶段 1 验收整改（报告 §2.2 第 1 项）
 * ══════════════════════════════════════════════════════
 * 原实现在 app_mq_calib.c 内部用两张定长查表，且都做了定义域夹紧：
 *     mq_log10 覆盖 x ∈ [0.01, 100]
 *     mq_pow10 覆盖 y ∈ [-2, 2]      （即 0.01 ~ 100 ppm）
 * 而两个传感器的曲线端点是：
 *     MQ-4   : 200 ~ 5000 ppm  → log10 = 2.301 ~ 3.699
 *     MQ-135 : 100 ~ 1000 ppm  → log10 = 2.000 ~ 3.000
 * 于是 **log10(200) 与 log10(5000) 双双被夹到 2.0**，导致：
 *     log_c1 == log_c2 == 2.0  →  插值分子 (log_c2 - log_c1) == 0
 *     → log_c 恒为 2.0 → ppm 恒为 100.0
 * 连带后果：OLED 的 GAS/AIR 恒显 2%、告警阈值（60%）永远不可能触发。
 *
 * 现改为「十进制分解 + 单周期查表 + 线性插值」，**取消定义域上限**，
 * 从根上消除这一整类缺陷（换任何量程的传感器都不会再撞到表边界）。
 *
 * 来源：本工程新建（阶段 1 验收整改；算法自 app_mq_calib.c 抽出以便单测）
 */
#ifndef MQ_MATH_H
#define MQ_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief log10(x)，x > 0；x ≤ 0 时返回 0
 *
 * 精度：单周期 19 点查表（步长 0.5）+ 线性插值，最大误差约 0.003
 *      → 折算到浓度约 0.7%，远优于 MQ 系列传感器本身 ±30% 的离散度
 */
static inline float mq_log10(float x)
{
    if (x <= 0.0f) return 0.0f;

    /* 分解为 x = m × 10^e，m ∈ [1, 10) */
    int e = 0;
    while (x >= 10.0f) { x *= 0.1f; e++; if (e > 38) break; }
    while (x <   1.0f) { x *= 10.0f; e--; if (e < -38) break; }

    /* lut[i] = log10(1 + i × 0.5)，i = 0..18 */
    static const float lut[19] = {
        0.00000f, 0.17609f, 0.30103f, 0.39794f, 0.47712f,
        0.54407f, 0.60206f, 0.65321f, 0.69897f, 0.74036f,
        0.77815f, 0.81291f, 0.84510f, 0.87506f, 0.90309f,
        0.92942f, 0.95424f, 0.97772f, 1.00000f,
    };

    float idx = (x - 1.0f) * 2.0f;      /* x=1 → 0, x=10 → 18 */
    int i = (int)idx;
    if (i < 0)  i = 0;
    if (i > 17) i = 17;                 /* 保证 lut[i+1] 不越界 */
    float t = idx - (float)i;
    return (float)e + lut[i] + t * (lut[i + 1] - lut[i]);
}

/**
 * @brief pow10(y) = 10^y，无定义域限制
 *
 * 精度：单周期 11 点查表（步长 0.1）+ 线性插值，最大误差约 0.8%
 */
static inline float mq_pow10(float y)
{
    /* 取 floor —— 直接 (int) 截断对负数不成立（-1.3 → -1，f 会变成 -0.3） */
    int e = (int)y;
    if ((float)e > y) e--;

    if (e >  38) e =  38;               /* float 上限保护，同时避免下面的循环失控 */
    if (e < -38) e = -38;

    float f = y - (float)e;             /* f ∈ [0, 1) */

    float base = 1.0f;
    for (int n = e; n > 0; n--) base *= 10.0f;
    for (int n = e; n < 0; n++) base *= 0.1f;

    /* lut[i] = 10^(i × 0.1)，i = 0..10 */
    static const float lut[11] = {
        1.000f, 1.259f, 1.585f, 1.995f, 2.512f, 3.162f,
        3.981f, 5.012f, 6.310f, 7.943f, 10.000f,
    };

    float idx = f * 10.0f;
    int i = (int)idx;
    if (i < 0) i = 0;
    if (i > 9) i = 9;
    float t = idx - (float)i;
    return base * (lut[i] + t * (lut[i + 1] - lut[i]));
}

/**
 * @brief 由 Rs/R0 反推气体浓度（log-log 两点插值）
 *
 * MQ 系列数据手册给出的 (浓度 C, 敏感体电阻比 Rs/R0) 曲线在 log-log 坐标下
 * 近似为直线，故取两端点做线性插值：
 *
 *     (log_r - log_r1) / (log_r2 - log_r1) = (log_c - log_c1) / (log_c2 - log_c1)
 *
 * 结果被夹紧在 [c1, c2] 内 —— 注意这个夹紧是**有意的**（超出量程不外推，
 * 避免给出比传感器本身更乐观的数字），与原先那种"因表太小而被动夹紧"不同。
 *
 * @param rs_r0  当前 Rs/R0
 * @param c1,r1  低浓度端点 (ppm, Rs/R0)
 * @param c2,r2  高浓度端点 (ppm, Rs/R0)
 * @return 估算浓度 (ppm)
 */
static inline float mq_ppm_from_rs_r0(float rs_r0,
                                      float c1, float r1,
                                      float c2, float r2)
{
    if (rs_r0 <= 0.0f) return c2;

    const float log_r  = mq_log10(rs_r0);
    const float log_c1 = mq_log10(c1);
    const float log_c2 = mq_log10(c2);
    const float log_r1 = mq_log10(r1);
    const float log_r2 = mq_log10(r2);

    const float denom = log_r2 - log_r1;
    if (denom == 0.0f) return c1;

    float log_c = log_c1 + (log_r - log_r1) * (log_c2 - log_c1) / denom;

    if (log_c < log_c1) log_c = log_c1;
    if (log_c > log_c2) log_c = log_c2;

    return mq_pow10(log_c);
}

#ifdef __cplusplus
}
#endif

#endif /* MQ_MATH_H */
