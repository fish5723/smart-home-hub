/**
 * @file bsp_pins.h
 * @brief 全局引脚/端口宏定义（T-15：单点维护）
 *
 * 本文件汇聚所有 BSP 层的 GPIO 引脚定义。
 * 任何模块需要通过引脚控制外设时，直接 include 本头文件，
 * 不得在各自 .c 中重复定义 *_PORT / *_PIN 宏。
 *
 * 来源：本工程新建（T-15：合并散落在各 BSP .c 内的重复定义）
 */
#ifndef BSP_PINS_H
#define BSP_PINS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ════════════════════════════════════════════════════
 *  GPIOB 资源（共 6 个外设，GPIOB 已饱和）
 *  ════════════════════════════════════════════════════ */

/* PB8  — 钛丝锁控制（暂不使用） */
#define LOCK_PORT   GPIOB
#define LOCK_PIN    GPIO_PIN_8

/* PB12 — 蜂鸣器
 * ⚠️ 事实说明（T-08）：PB12 在 STM32F103 上**没有任何定时器 PWM 通道**
 * （它是 TIM1_BKIN 刹车输入，不是输出）。因此蜂鸣器只能由 TIM4 的更新中断
 * 翻转 GPIO 来产生方波，占空比恒为 50%（对无源蜂鸣器正是想要的）。
 * 若将来需要真正的硬件 PWM 或可调占空比，必须把蜂鸣器移到
 * PA8/PA11(TIM1_CH1/CH4)、PB6/PB7/PB8/PB9(TIM4_CH1~CH4) 等有通道的引脚。 */
#define BUZZER_PORT GPIOB
#define BUZZER_PIN  GPIO_PIN_12

/* PB13 — HC-SR501 PIR 人体传感器（EXTI 中断） */
#define HUMAN_PORT  GPIOB
#define HUMAN_PIN   GPIO_PIN_13

/* PB14 — 继电器（通道 0，低电平吸合；匹配实测模块行为） */
#define RELAY_PORT  GPIOB
#define RELAY_PIN   GPIO_PIN_14

/* PB15 — 水泵（灌溉通道 1，高电平启动） */
#define PUMP_PORT   GPIOB
#define PUMP_PIN    GPIO_PIN_15

/* ════════════════════════════════════════════════════
 *  引脚位号工具（T-15 整改：报告 §2.3）
 *
 *  EXTI 的 IMR/RTSR/FTSR 以及 AFIO->EXTICR 都需要"位号"而非 GPIO_PIN_x 掩码。
 *  bsp_human.c 原先把 13 硬编码在 5 处，改引脚时极易漏改其中一处。
 *  HAL 的 GPIO_PIN_n 定义为 (1U << n)，故位号 = 尾部零的个数；
 *  __builtin_ctz 在参数为编译期常量时也是常量表达式，可直接用于 #if。
 * ════════════════════════════════════════════════════ */
#define BSP_PIN_POS(pin)    ((uint32_t)__builtin_ctz((unsigned int)(pin)))

/** HUMAN_PIN 对应的 EXTI 线号（PB13 → 线 13） */
#define HUMAN_EXTI_LINE     BSP_PIN_POS(HUMAN_PIN)

/* ════════════════════════════════════════════════════
 *  GPIOA 资源
 *  ════════════════════════════════════════════════════ */

/* PA4  — W25Q64 SPI 片选 */
#define W25Q64_CS_PORT GPIOA
#define W25Q64_CS_PIN  GPIO_PIN_4

/* PA8  — DHT11 温湿度传感器（开漏输出 + 上拉） */
#define DHT11_PORT  GPIOA
#define DHT11_PIN   GPIO_PIN_8

/* PA0~PA3 — ADC 模拟输入通道（由 CubeMX adc.c 管理，此处仅记录） */
/*
 * PA0 土壤湿度    ADC1_IN0
 * PA1 MQ-4 可燃气  ADC1_IN1
 * PA2 MQ-135 空气  ADC1_IN2
 * PA3 雨滴         ADC1_IN3
 */

/* ════════════════════════════════════════════════════
 *  I2C / SPI 总线（由 CubeMX 生成，此处仅记录引用）
 *  ════════════════════════════════════════════════════ */
/*
 * I2C1 (PB6-SCL / PB7-SDA)：OLED + BMP280 + EEPROM 共总线
 * SPI1 (PA5-SCK / PA6-MISO / PA7-MOSI)：W25Q64 Flash
 * USART3 (PB10-TX / PB11-RX)：调试串口 + printf retarget
 */

#ifdef __cplusplus
}
#endif
#endif /* BSP_PINS_H */
