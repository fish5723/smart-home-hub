/**
 * @file bsp_iwdg.c
 * @brief IWDG 独立看门狗 + 复位原因诊断（T-04 / T-05）— HAL 版本
 *
 * CubeMX 已生成 MX_IWDG_Init() 做基础初始化；
 * BSP_IWDG_Init(timeout_ms) 在此基础上按目标超时重配 PR/RLR。
 *
 * 来源：本工程新建 → 阶段 1 重构为 HAL
 */
#include "bsp_iwdg.h"
#include "iwdg.h"      /* hiwdg 句柄（CubeMX 生成）       */
#include <stdio.h>     /* snprintf — BSP 层仅格式化不输出 */

/* ════════════════════════════════════════════════════
 *  LSI / 预分频表
 *  ════════════════════════════════════════════════════ */
#define LSI_TYPICAL_HZ      40000U
#define IWDG_RLR_MAX        0xFFFU

static const struct {
    uint32_t hal_prescaler;   /* IWDG_PRESCALER_x 宏值 */
    uint16_t div;             /* 分频比              */
} s_pr_table[] = {
    { IWDG_PRESCALER_4,     4 },
    { IWDG_PRESCALER_8,     8 },
    { IWDG_PRESCALER_16,   16 },
    { IWDG_PRESCALER_32,   32 },
    { IWDG_PRESCALER_64,   64 },
    { IWDG_PRESCALER_128, 128 },
    { IWDG_PRESCALER_256, 256 },
};
#define PR_COUNT (sizeof(s_pr_table) / sizeof(s_pr_table[0]))

/* ════════════════════════════════════════════════════
 *  T-05: 复位原因诊断（RCC_CSR）
 *  ════════════════════════════════════════════════════ */

static bsp_rst_reason_t s_last_reason = BSP_RST_UNKNOWN;

bsp_rst_reason_t BSP_IWDG_GetResetReason(void)
{
    uint32_t csr = RCC->CSR;

    if (csr & RCC_CSR_IWDGRSTF)  return BSP_RST_IWDG;
    if (csr & RCC_CSR_WWDGRSTF)  return BSP_RST_WWDG;
    if (csr & RCC_CSR_SFTRSTF)   return BSP_RST_SW;
    if (csr & RCC_CSR_PINRSTF)   return BSP_RST_PIN;
    if (csr & RCC_CSR_PORRSTF)   return BSP_RST_POR;
    if (csr & RCC_CSR_LPWRRSTF)  return BSP_RST_LPWR;

    return BSP_RST_UNKNOWN;
}

void BSP_IWDG_GetResetReasonStr(char *buf, size_t len)
{
    uint32_t csr = RCC->CSR;
    s_last_reason = BSP_IWDG_GetResetReason();

    snprintf(buf, len,
        "\r\n=== RST Reason ===\r\n"
        "  POR    : %s\r\n"
        "  PIN    : %s\r\n"
        "  SW     : %s\r\n"
        "  IWDG   : %s\r\n"
        "  WWDG   : %s\r\n"
        "  LPWR   : %s\r\n"
        "===  ============\r\n\r\n",
        (csr & RCC_CSR_PORRSTF)  ? "YES" : "no",
        (csr & RCC_CSR_PINRSTF)  ? "YES" : "no",
        (csr & RCC_CSR_SFTRSTF)   ? "YES" : "no",
        (csr & RCC_CSR_IWDGRSTF) ? "YES" : "no",
        (csr & RCC_CSR_WWDGRSTF) ? "YES" : "no",
        (csr & RCC_CSR_LPWRRSTF) ? "YES" : "no");

    BSP_IWDG_ClearFlags();
}

void BSP_IWDG_ClearFlags(void)
{
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

/* ════════════════════════════════════════════════════
 *  T-04: IWDG 初始化 + 喂狗（HAL 版本）
 *  ════════════════════════════════════════════════════ */

uint32_t BSP_IWDG_Init(uint32_t timeout_ms)
{
    /* 选最优预分频 */
    uint32_t target_ticks = (uint32_t)((uint64_t)timeout_ms * LSI_TYPICAL_HZ / 1000UL);
    uint32_t hal_psc = s_pr_table[0].hal_prescaler;
    uint16_t rlr     = 0;
    uint16_t div     = s_pr_table[0].div;

    for (uint8_t i = 0; i < PR_COUNT; i++) {
        uint32_t ticks = target_ticks / s_pr_table[i].div;
        if (ticks <= IWDG_RLR_MAX) {
            hal_psc = s_pr_table[i].hal_prescaler;
            rlr     = (uint16_t)ticks;
            div     = s_pr_table[i].div;
            break;
        }
        if (i == PR_COUNT - 1) {
            hal_psc = s_pr_table[i].hal_prescaler;
            rlr     = IWDG_RLR_MAX;
            div     = s_pr_table[i].div;
        }
    }
    if (rlr == 0) rlr = 1;

    /* ── 冻结 IWDG 当调试器挂起时 ── */
    __HAL_DBGMCU_FREEZE_IWDG();

    /* ── 重新配置 IWDG 为需要的超时 ── */
    hiwdg.Instance       = IWDG;
    hiwdg.Init.Prescaler = hal_psc;
    hiwdg.Init.Reload    = rlr;
    HAL_IWDG_Init(&hiwdg);

    uint32_t actual_ms = (uint32_t)((uint64_t)rlr * div * 1000UL / LSI_TYPICAL_HZ);
    return actual_ms;
}

void BSP_IWDG_Feed(void)
{
    HAL_IWDG_Refresh(&hiwdg);
}