/**
 * @file app_scheduler.h
 * @brief 应用层任务调度器
 *
 * 负责：初始化顺序编排 + 主循环任务调度
 * 从 main.c 提取，使 CubeMX 生成区只保留 HAL 初始化骨架。
 *
 * 来源：本工程新建（阶段 1 重构）
 */
#ifndef APP_SCHEDULER_H
#define APP_SCHEDULER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  任务追踪 ID（.noinit 区，存活于复位）
 *  各任务模块可在关键路径写入 sched_trace_set() 以标记当前阶段。
 *  ════════════════════════════════════════════════════ */
#define SCHED_TRACE_NONE          0
#define SCHED_TRACE_DHT11         1
#define SCHED_TRACE_DHT11_READ    2   /* DHT11: 进入 DHT11_Read()，关中断后 */
#define SCHED_TRACE_HUMAN        10
#define SCHED_TRACE_SOIL         11
#define SCHED_TRACE_MQ4          12
#define SCHED_TRACE_AIR          13
#define SCHED_TRACE_RAIN         14
#define SCHED_TRACE_BMP280       15
#define SCHED_TRACE_BEEP         16
#define SCHED_TRACE_MONITOR      17
#define SCHED_TRACE_IRR          18
#define SCHED_TRACE_UART         19
#define SCHED_TRACE_PROTO        21   /* 阶段 2: 协议收包 */
/* 主循环的循环体（心跳/统计打印）。复位后读到 IDLE 说明崩溃不在任何任务里，
 * 而在循环体本身 —— 原先末尾置 NONE 会把这个信息丢掉（§3.3）。 */
#define SCHED_TRACE_IDLE         99

extern volatile uint32_t g_sched_trace;

/* 初始化阶段标记（.noinit，复位后保留）。正常启动最终为 20；
 * 若停在某个值，说明卡在该初始化步骤。 */
extern volatile uint32_t g_app_boot_stage;

void App_Scheduler_Init(void);
void App_Scheduler_Run(void);

/**
 * @brief 打印每个任务的"上次/最大"执行耗时（µs）与主循环总耗时
 *
 * 阶段 1 验收整改（§3.3）：原调度器只有顺序编排、没有执行时间观测 ——
 * 而阶段 2 的协议需要量化主循环延迟上限来设定 ACK 超时。
 * 主循环每 60s 自动调用一次；也可手动调用。
 */
void App_Scheduler_PrintStats(void);

void APP_PowerCounter_Boot(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SCHEDULER_H */