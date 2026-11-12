/**
 * @file bsp_dht11.h
 * @brief DHT11 温湿度传感器接口（单总线）
 *
 * 来源：前序工程 BSP/DHT11/driver_dht11.h（原文件名 driver_dht11，本工程改前缀）
 * 本工程修改：
 *   - 阶段 1：数据只取整数部分（小数位丢弃），故对外用整数语义；已知问题见 bsp_dht11.c 头部（关中断窗口过大 + 无重试）
 */
#ifndef DHT11_H
#define DHT11_H

#include "main.h"

typedef struct
{
    float temperature;
    float humidity;

} dht11_data_t;

uint8_t DHT11_Read(dht11_data_t *data);

#endif