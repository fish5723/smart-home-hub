/**
 * @file human.h
 * @brief HC-SR501 PIR 人体检测 BSP 接口
 *
 * 阶段 1（T-13）：新增 EXTI 中断驱动接口
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#ifndef SMART_HOME_HUMAN_H
#define SMART_HOME_HUMAN_H
#ifdef __cplusplus
extern "C"{
#endif

#include "main.h"

/** 初始化 PIR（EXTI 中断配置 — T-13 新增）*/
void BSP_HUMAN_Init(void);

/** 轮询读取引脚电平（保留，用于一致性对比）*/
uint8_t BSP_HUMAN_Read(void);

/** 获取 EXTI 触发标志并清除（T-13）*/
uint8_t BSP_HUMAN_GetTrigger(void);

/** 获取触发时的引脚电平（T-13）*/
uint8_t BSP_HUMAN_GetEdgeState(void);

/** 获取 EXTI 原始边沿计数（含抖动，T-13）— 与 APP 层的去抖计数对比用 */
uint32_t BSP_HUMAN_GetIrqCount(void);

#ifdef __cplusplus
}
#endif
#endif //SMART_HOME_HUMAN_H