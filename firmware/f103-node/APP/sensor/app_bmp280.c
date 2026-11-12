/**
 * @file app_bmp280.c
 * @brief BMP280 应用层实现（1Hz）
 *
 * 来源：前序工程 APP/bmp-280/app_bmp280.c
 * 本工程修改：
 *   - 阶段 1：接入 quality 与 fault_cnt；⚠️ fault_cnt 成功后未清零
 */
//
// Created by A on 2026/5/29.
//

#include "app_bmp280.h"
#include "bsp_bmp280.h"
#include "app_data.h"
#include "stdio.h"
//bmp数据
static bmp280_data_t bmp_data;
//执行周期
#define BMP280_PERIOD_MS 1000U
// 初始化BMP280
void APP_BMP280_Init(void)
{
    if(BSP_BMP280_Init() == 0)
    {
        printf("BMP280 INIT OK\r\n");
    }
    else
    {
        printf("BMP280 INIT FAIL\r\n");
    }
}
// 任务函数
void APP_BMP280_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  fault_cnt = 0;      /* 连续失败计数（成功时清零） */

    if(HAL_GetTick() - last_tick < BMP280_PERIOD_MS)
    {
        return;
    }
    last_tick = HAL_GetTick();
    uint8_t ret = BSP_BMP280_ReadData(&bmp_data);

    if(ret == 0)
    {
        fault_cnt = 0;                  /* 读取成功 → 连续失败计数清零 */
        g_sensor_data.pressure = bmp_data.pressure;
        g_sensor_data.bmp280_temp = bmp_data.temperature;
        g_sensor_data.bmp280_tick = HAL_GetTick();   /* 数据新鲜度（§3.4）*/
        g_sensor_data.bmp280_quality = (uint8_t)SENSOR_QUAL_VALID;
        g_sensor_data.bmp280_valid = sensor_valid_from_quality(g_sensor_data.bmp280_quality);
    }
    else
    {
        g_sensor_data.bmp280_valid = 0;
        if (++fault_cnt >= SENSOR_FAULT_COUNT)
            g_sensor_data.bmp280_quality = (uint8_t)SENSOR_QUAL_FAULT;
        printf("BMP280 ReadData FAIL, ret=%d\r\n", ret);
    }

}