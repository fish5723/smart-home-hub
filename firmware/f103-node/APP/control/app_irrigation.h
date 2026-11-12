/**
 * @file app_irrigation.h
 * @brief 双通道灌溉控制模块（T-12：合并 RELAY + PUMP）
 *
 * 两个物理通道共享相同逻辑：
 *   - 土壤湿度 < 阈值低 → 开启
 *   - 土壤湿度 ≥ 阈值高 → 关闭
 *   - 下雨或传感器无效 → 全部关闭
 *   - 水泵连续运行超时 → 强制关闭（T-18 干烧保护）
 *
 * 来源：本工程新建（合并 app_relay / app_pump）
 */
#ifndef APP_IRRIGATION_H
#define APP_IRRIGATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 未接入继电器/水泵阶段默认关闭 F103 本地灌溉兜底。接入执行器并完成
 * 空载验证后，可在工程编译定义中设为 1 重新启用。 */
#ifndef APP_IRR_LOCAL_FALLBACK_ENABLE
#define APP_IRR_LOCAL_FALLBACK_ENABLE 0
#endif

void APP_IRR_Init(void);
void APP_IRR_Task(void);

/**
 * @brief 设置双阈值（T-12 整改：阈值运行时可配）
 *
 * @param on_percent  开阀阈值（土壤 ≤ 该值则开），需 > 0
 * @param off_percent 关阀阈值（土壤 ≥ 该值则关），需 ≤ 100 且 > on_percent
 * @return 1=已生效，0=参数非法（拒绝并打印原因，保持原值不变）
 *
 * 迟滞区间 (on_percent, off_percent) 必须存在，否则开关判定会在同一轮
 * 互相取消，导致继电器高频抖动。默认 30 / 45。
 */
uint8_t APP_IRR_SetThresholds(uint8_t on_percent, uint8_t off_percent);

/** 读取当前开阀阈值 */
uint8_t APP_IRR_GetOnThreshold(void);

/** 读取当前关阀阈值 */
uint8_t APP_IRR_GetOffThreshold(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_IRRIGATION_H */
