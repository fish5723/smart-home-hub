/**
 * @file bsp_iwdg.h
 * @brief IWDG 独立看门狗 + 复位原因诊断（T-04 / T-05）
 *
 * 阶段 1（T-04）：IWDG 初始化与喂狗，超时自动复位
 * 阶段 1（T-05）：启动时读取 RCC_CSR 判断上次复位来源
 *
 * STM32F103 IWDG 使用 40kHz LSI 时钟，基于 HAL 库实现。
 * 支持 DBGMCU 冻结——挂调试器时自动暂停计数，不会误复位。
 *
 * 来源：本工程新建
 */
#ifndef BSP_IWDG_H
#define BSP_IWDG_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 复位原因枚举
 */
typedef enum {
    BSP_RST_UNKNOWN = 0,
    BSP_RST_POR     = 1,    /* 上电复位（首次上电或 VDD 跌落到 POR 阈值） */
    BSP_RST_PIN     = 2,    /* NRST 引脚复位（外部复位按键或烧录器）       */
    BSP_RST_SW      = 3,    /* 软件复位（NVIC_SystemReset / 调试器）        */
    BSP_RST_IWDG    = 4,    /* 独立看门狗复位                              */
    BSP_RST_WWDG    = 5,    /* 窗口看门狗复位                              */
    BSP_RST_LPWR    = 6,    /* 低功耗模式唤醒复位                          */
} bsp_rst_reason_t;

/**
 * @brief 将上次复位原因格式化为字符串（T-14: BSP 层不 printf）
 *
 * @param buf  输出缓冲区
 * @param len  缓冲区大小（建议 ≥256）
 *
 * 必须在初始化 IWDG 之前调用，否则 RCC_CSR 可能被修改。
 */
void BSP_IWDG_GetResetReasonStr(char *buf, size_t len);

/**
 * @brief 获取上次复位原因
 * @return 复位原因枚举值，若多个标志同时置位则返回"最严重"的那个
 */
bsp_rst_reason_t BSP_IWDG_GetResetReason(void);

/**
 * @brief 清除 RCC_CSR 中的全部复位标志位
 */
void BSP_IWDG_ClearFlags(void);

/**
 * @brief IWDG 初始化并启动
 *
 * LSI 约 40kHz，精度 ±50%（30~60kHz）。超时时间按典型值计算，
 * 实际超时窗口为 ~1.3s ~ 2.7s（对 2s 配置）。
 *
 * @param timeout_ms 看门狗超时时间 (ms)，会被自动映射到最接近的 PR/RLR 组合
 * @return 实际超时时间 (ms)（因 PR 分档，可能与请求值略有偏差）
 *
 *     2s（推荐）: PR=/64  RLR=1250
 *                    Debug 模式自动冻结 IWDG，烧录/断点不会复位
 */
uint32_t BSP_IWDG_Init(uint32_t timeout_ms);

/**
 * @brief 喂狗（重载计数器）
 *
 * 写入 IWDG_KR=0xAAAA。须在超时前周期性调用。
 * 当前架构在主循环顶部调用，loop 耗时 < 10ms → 安全。
 */
void BSP_IWDG_Feed(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_IWDG_H */