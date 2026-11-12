/**
 * @file test_mq_math.c
 * @brief 主机端单测：验证 firmware/f103-node/common/mq_math.h 的换算正确性
 *
 * 目的：阶段 1 验收报告 §2.2 第 1 项（MQ 浓度恒为 100.0 ppm）的修复验证。
 *       原先的实现用定长查表 + 定义域夹紧，而 log10 表上限只有 100，
 *       导致 MQ-4 的两端浓度 200/5000 ppm 双双被夹到 2.0，
 *       log_c1 == log_c2 → 插值分子为 0 → ppm 恒为 100.0。
 *
 * 这里不靠手算：log10 与 pow10 直接与主机 libm 对照，曲线端点做反演检查。
 *
 * 编译运行（在仓库根目录）：
 *   gcc -O2 -Wall -o tools/test_mq_math tools/test_mq_math.c -lm && ./tools/test_mq_math
 */
#include <stdio.h>
#include <math.h>

#include "../firmware/f103-node/common/mq_math.h"

/* 与 app_mq_calib.c 的 g_mq_sens 保持一致（数据手册典型值） */
#define MQ4_C1  200.0f
#define MQ4_R1  0.55f
#define MQ4_C2  5000.0f
#define MQ4_R2  0.15f

#define AIR_C1  100.0f
#define AIR_R1  0.65f
#define AIR_C2  1000.0f
#define AIR_R2  0.15f

static int g_fail = 0;

static void chk(const char *what, double got, double want, double tol_rel)
{
    const double rel = (want != 0.0) ? fabs(got - want) / fabs(want) : fabs(got);
    const int ok = (rel <= tol_rel);
    if (!ok) g_fail++;
    printf("  %-40s got=%-12.4f want=%-12.4f rel=%7.3f%%  %s\n",
           what, got, want, rel * 100.0, ok ? "OK" : "** FAIL **");
}

int main(void)
{
    printf("=== 1. mq_log10 vs libm log10（相对误差 <= 0.1%%）===\n");
    {
        /* 覆盖 0.01 ~ 100000，两端远超原先的 [0.01,100] 表范围 */
        const double xs[] = { 0.01, 0.15, 0.55, 1.0, 2.0, 5.0, 10.0,
                              50.0, 100.0, 200.0, 1000.0, 5000.0, 100000.0 };
        char label[64];
        for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
            snprintf(label, sizeof(label), "log10(%.4g)", xs[i]);
            chk(label, (double)mq_log10((float)xs[i]), log10(xs[i]), 0.001);
        }
    }

    printf("\n=== 2. mq_pow10 vs libm pow(10,y)（相对误差 <= 1%%）===\n");
    {
        const double ys[] = { -3.0, -2.0, -1.3, -0.5, 0.0, 0.5,
                              1.0, 2.0, 2.301, 3.0, 3.699, 4.0 };
        char label[64];
        for (unsigned i = 0; i < sizeof(ys) / sizeof(ys[0]); i++) {
            snprintf(label, sizeof(label), "pow10(%.4g)", ys[i]);
            chk(label, (double)mq_pow10((float)ys[i]), pow(10.0, ys[i]), 0.01);
        }
    }

    printf("\n=== 3. 曲线端点反演（应在 1%% 内回到手册端点）===\n");
    chk("MQ-4  Rs/R0=0.55 -> 200 ppm",
        mq_ppm_from_rs_r0(MQ4_R1, MQ4_C1, MQ4_R1, MQ4_C2, MQ4_R2), 200.0, 0.01);
    chk("MQ-4  Rs/R0=0.15 -> 5000 ppm",
        mq_ppm_from_rs_r0(MQ4_R2, MQ4_C1, MQ4_R1, MQ4_C2, MQ4_R2), 5000.0, 0.01);
    chk("MQ-135 Rs/R0=0.65 -> 100 ppm",
        mq_ppm_from_rs_r0(AIR_R1, AIR_C1, AIR_R1, AIR_C2, AIR_R2), 100.0, 0.01);
    chk("MQ-135 Rs/R0=0.15 -> 1000 ppm",
        mq_ppm_from_rs_r0(AIR_R2, AIR_C1, AIR_R1, AIR_C2, AIR_R2), 1000.0, 0.01);

    printf("\n=== 4. 回归断言：这些值在修复前一律是 100.0 ===\n");
    {
        const float r = mq_ppm_from_rs_r0(0.15f, MQ4_C1, MQ4_R1, MQ4_C2, MQ4_R2);
        const int ok = (r > 1000.0f);          /* 旧实现恒为 100.0 */
        if (!ok) g_fail++;
        printf("  %-40s = %.1f ppm  %s\n", "MQ-4 @ Rs/R0=0.15",
               (double)r, ok ? "OK（已不再是 100.0）" : "** FAIL（仍是饱和值）**");
    }

    printf("\n=== 5. 单调性：Rs/R0 升高 → 浓度必须单调下降 ===\n");
    {
        float prev = 1e9f;
        int mono_ok = 1;
        for (float r = 0.10f; r <= 0.80f; r += 0.01f) {
            const float c = mq_ppm_from_rs_r0(r, MQ4_C1, MQ4_R1, MQ4_C2, MQ4_R2);
            if (c > prev + 0.5f) { mono_ok = 0; break; }   /* 容 0.5ppm 抖动 */
            prev = c;
        }
        if (!mono_ok) g_fail++;
        printf("  %-40s %s\n", "0.10 → 0.80 全程单调",
               mono_ok ? "OK" : "** FAIL **");
    }

    printf("\n=== 6. 量程夹紧仍生效（超出端点不外推）===\n");
    chk("Rs/R0=0.01（低于高浓度端）-> 夹紧到 5000",
        mq_ppm_from_rs_r0(0.01f, MQ4_C1, MQ4_R1, MQ4_C2, MQ4_R2), 5000.0, 0.01);
    chk("Rs/R0=3.00（高于低浓度端）-> 夹紧到 200",
        mq_ppm_from_rs_r0(3.00f, MQ4_C1, MQ4_R1, MQ4_C2, MQ4_R2), 200.0, 0.01);

    printf("\n=== 7. 换量程（10~10000 ppm）不再撞表边界 ===\n");
    chk("自定义曲线 Rs/R0=1.0 -> 10 ppm",
        mq_ppm_from_rs_r0(1.0f, 10.0f, 1.0f, 10000.0f, 0.05f), 10.0, 0.01);
    chk("自定义曲线 Rs/R0=0.05 -> 10000 ppm",
        mq_ppm_from_rs_r0(0.05f, 10.0f, 1.0f, 10000.0f, 0.05f), 10000.0, 0.01);

    printf("\n────────────────────────────────────────\n");
    if (g_fail == 0) {
        printf("全部通过\n");
        return 0;
    }
    printf("失败 %d 项\n", g_fail);
    return 1;
}
