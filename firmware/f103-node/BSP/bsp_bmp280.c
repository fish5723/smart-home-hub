/**
 * @file bsp_bmp280.c
 * @brief BMP280 气压/温度传感器实现（I2C，含校准补偿）
 *
 * 来源：前序工程 BSP/BMP-280/bsp_bmp280.c
 * 本工程修改：
 *   - 阶段 1：仅重命名前缀与整理头文件，补偿算法保持原样（含 int64 防溢出）
 */
/*
 * bsp_bmp280.c
 */

#include "bsp_bmp280.h"
#include "i2c.h"

// BMP280 I2C地址
#define BMP280_ADDR               (0x76 << 1)

// BMP280 寄存器
#define BMP280_REG_ID             0xD0
#define BMP280_REG_RESET          0xE0
#define BMP280_REG_STATUS         0xF3
#define BMP280_REG_CTRL_MEAS      0xF4
#define BMP280_REG_CONFIG         0xF5
#define BMP280_REG_PRESS_MSB      0xF7

// BMP280 CHIP ID
#define BMP280_CHIP_ID            0x58

// 校准参数寄存器
#define BMP280_CALIB_REG          0x88

// 校准参数
static uint16_t dig_T1;
static int16_t  dig_T2;
static int16_t  dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2;
static int16_t  dig_P3;
static int16_t  dig_P4;
static int16_t  dig_P5;
static int16_t  dig_P6;
static int16_t  dig_P7;
static int16_t  dig_P8;
static int16_t  dig_P9;

static int32_t t_fine;

// 读取寄存器
static HAL_StatusTypeDef
BMP280_ReadReg(uint8_t reg, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(&hi2c1,
                            BMP280_ADDR,
                            reg,
                            I2C_MEMADD_SIZE_8BIT,
                            buf,
                            len,
                            100);
}

// 写寄存器
static HAL_StatusTypeDef
BMP280_WriteReg(uint8_t reg, uint8_t data)
{
    return HAL_I2C_Mem_Write(&hi2c1,
                             BMP280_ADDR,
                             reg,
                             I2C_MEMADD_SIZE_8BIT,
                             &data,
                             1,
                             100);
}

// 读取ID
uint8_t BSP_BMP280_ReadID(void)
{
    uint8_t id = 0;
    BMP280_ReadReg(BMP280_REG_ID, &id, 1);
    return id;
}

// 读取校准参数
static void BMP280_ReadCalib(void)
{
    uint8_t calib[24];
    BMP280_ReadReg(BMP280_CALIB_REG, calib, 24);

    dig_T1 = (uint16_t)((calib[1] << 8) | calib[0]);
    dig_T2 = (int16_t)((calib[3] << 8) | calib[2]);
    dig_T3 = (int16_t)((calib[5] << 8) | calib[4]);

    dig_P1 = (uint16_t)((calib[7] << 8) | calib[6]);
    dig_P2 = (int16_t)((calib[9] << 8) | calib[8]);
    dig_P3 = (int16_t)((calib[11] << 8) | calib[10]);
    dig_P4 = (int16_t)((calib[13] << 8) | calib[12]);
    dig_P5 = (int16_t)((calib[15] << 8) | calib[14]);
    dig_P6 = (int16_t)((calib[17] << 8) | calib[16]);
    dig_P7 = (int16_t)((calib[19] << 8) | calib[18]);
    dig_P8 = (int16_t)((calib[21] << 8) | calib[20]);
    dig_P9 = (int16_t)((calib[23] << 8) | calib[22]);
}

// 温度补偿（获取t_fine）
static int32_t BMP280_CompensateTemp(int32_t adc_T)
{
    int32_t var1, var2;

    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) *
              ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;

    t_fine = var1 + var2;

    return (t_fine * 5 + 128) >> 8;
}

// 气压补偿
static uint32_t BMP280_CompensatePress(int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) +
           ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;

    if (var1 == 0)
    {
        return 0;
    }

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);

    return (uint32_t)p;
}

// BMP280 初始化
uint8_t BSP_BMP280_Init(void)
{
    uint8_t id;

    id = BSP_BMP280_ReadID();

    if(id != BMP280_CHIP_ID)
    {
        return 1;
    }

    // 读取校准参数（关键！）
    BMP280_ReadCalib();

    // 正常模式，temp x1, press x1
    BMP280_WriteReg(BMP280_REG_CTRL_MEAS, 0x27);

    return 0;
}

// BMP280 读取气压
uint8_t BSP_BMP280_ReadData(bmp280_data_t *data)
{
    uint8_t buf[6];
    int32_t adc_P;
    int32_t adc_T;
    uint32_t comp_P;

    if(BMP280_ReadReg(BMP280_REG_PRESS_MSB, buf, 6) != HAL_OK)
    {
        return 1;
    }

    // 20bit Pressure RAW
    adc_P = ((int32_t)((uint32_t)buf[0] << 12) |
             (int32_t)((uint32_t)buf[1] << 4)  |
             (int32_t)((uint32_t)buf[2] >> 4));

    // 20bit Temperature RAW（必须读，用于补偿）
    adc_T = ((int32_t)((uint32_t)buf[3] << 12) |
             (int32_t)((uint32_t)buf[4] << 4)  |
             (int32_t)((uint32_t)buf[5] >> 4));

    // 先算温度得到t_fine（comp_T 单位为 0.01 °C）
    int32_t comp_T = BMP280_CompensateTemp(adc_T);
    data->temperature = (float)comp_T / 100.0f;

    // 气压补偿
    comp_P = BMP280_CompensatePress(adc_P);

    // 单位：Pa
    data->pressure = (float)comp_P / 256.0f;

    return 0;
}