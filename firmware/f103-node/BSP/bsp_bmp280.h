/**
 * @file bsp_bmp280.h
 * @brief BMP280 气压/温度传感器接口（I2C）
 *
 * 来源：前序工程 BSP/BMP-280（质量最好的一支驱动）
 * 本工程修改：
 *   - 阶段 1：引脚与总线句柄改从 bsp_pins.h / hi2c1 取；错误码沿用 0=成功
 */
//
// Created by A on 2026/5/29.
//

#ifndef SMART_HOME_BSP_BMP280_H
#define SMART_HOME_BSP_BMP280_H
#ifdef __cplusplus
extern "C"{
#endif

#include "main.h"

// BMP280数据结构
typedef struct
{
    float pressure;      // Pa
    float temperature;   // °C（同一帧读出，comp_T 的返回值不再丢弃）
}bmp280_data_t;

/** 初始化*/
uint8_t BSP_BMP280_Init(void);

/** 读取ID*/
uint8_t BSP_BMP280_ReadID(void);

/** 读取气压*/
uint8_t BSP_BMP280_ReadData(bmp280_data_t *data);

#ifdef __cplusplus
}
#endif
#endif //SMART_HOME_BSP_BMP280_H
