/**
 * @file bsp_eeprom.h
 * @brief AT24Cxx EEPROM I2C 驱动（T-06）
 *
 * 阶段 1（T-06）：掉电计数 — 记录系统上电次数，拔电重启 +1 且校验和正确。
 *
 * 硬件：
 *   - I2C1（PB6-SCL / PB7-SDA），100kHz，与 OLED/BMP280 共总线
 *   - 器件地址：0x50（7-bit，A0=A1=A2=GND）
 *   - 页大小：16 字节（AT24Cxx 典型值）
 *   - 写周期：5ms（tWR）
 *
 * 来源：本工程新建
 */
#ifndef BSP_EEPROM_H
#define BSP_EEPROM_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  EEPROM 参数
 *  ════════════════════════════════════════════════════ */
#define BSP_EEPROM_I2C_ADDR     0x50U   /* 7-bit 器件地址           */
#define BSP_EEPROM_PAGE_SIZE    16U     /* 单页最大写入字节数         */
#define BSP_EEPROM_WRITE_MS     5U      /* 写周期等待（ms）          */

/**
 * @brief EEPROM 字节读取
 * @param addr  内存地址 (0 ~ 容量-1)
 * @param pdata 读回数据指针
 * @retval 0 成功，非 0 失败
 */
uint8_t BSP_EEPROM_ReadByte(uint16_t addr, uint8_t *pdata);

/**
 * @brief EEPROM 字节写入（含写周期等待）
 * @param addr  内存地址
 * @param data  写入数据
 * @retval 0 成功，非 0 失败
 */
uint8_t BSP_EEPROM_WriteByte(uint16_t addr, uint8_t data);

/**
 * @brief EEPROM 连续读取
 * @param addr  起始地址
 * @param buf   缓冲区指针
 * @param len   读取长度（跨页自动处理）
 * @retval 0 成功，非 0 失败
 */
uint8_t BSP_EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len);

/**
 * @brief EEPROM 连续写入（自动分页 + 写周期等待）
 * @param addr  起始地址
 * @param buf   数据指针
 * @param len   写入长度（跨页自动分页）
 * @retval 0 成功，非 0 失败
 */
uint8_t BSP_EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len);

/* ════════════════════════════════════════════════════
 *  掉电计数（T-06 验收功能）
 *  ════════════════════════════════════════════════════ */

/**
 * @brief 掉电计数布局
 *
 *   偏移   长度  内容
 *   ─────────────────────────
 *   0-1    2B    Magic (0xA55A) — 判断 EEPROM 是否已初始化
 *   2-5    4B    上电计数器（uint32_t，小端）
 *   6      1B    校验和（字节 0-5 的 XOR）
 *
 *   总计 7 字节，占用地址 0x0000-0x0006
 */
#define EEPROM_PC_MAGIC_BASE   0x0000U
#define EEPROM_PC_MAGIC_LEN    7U

#define EEPROM_MAGIC_HI     0xA5U
#define EEPROM_MAGIC_LO     0x5AU

#define EEPROM_OFF_MAGIC_HI 0U
#define EEPROM_OFF_MAGIC_LO 1U
#define EEPROM_OFF_CTR0     2U
#define EEPROM_OFF_CTR1     3U
#define EEPROM_OFF_CTR2     4U
#define EEPROM_OFF_CTR3     5U
#define EEPROM_OFF_CHECKSUM 6U

/* ════════════════════════════════════════════════════
 *  MQ 传感器 R0 持久化布局（阶段 1 验收整改 §2.3）
 *
 *  每条记录 5 字节：
 *    偏移  长度  内容
 *    ─────────────────────────
 *    0     2B    Magic (0x5AC3) — 标记本条记录有效
 *    2     2B    R0 的 ADC 原始值（uint16_t，小端）
 *    4     1B    校验和（字节 0-3 的 XOR）
 *
 *  两条记录（MQ-4 / MQ-135）各占 8 字节、页对齐，互不跨页；
 *  掉电计数占用 0x0000-0x0006，故从 0x0010 开始。
 * ════════════════════════════════════════════════════ */
#define EEPROM_MQ_R0_BASE_MQ4    0x0010U
#define EEPROM_MQ_R0_BASE_MQ135  0x0018U
#define EEPROM_MQ_R0_LEN         5U
#define EEPROM_MQ_R0_MAGIC_HI    0x5AU
#define EEPROM_MQ_R0_MAGIC_LO    0xC3U

#ifdef __cplusplus
}
#endif

#endif /* BSP_EEPROM_H */