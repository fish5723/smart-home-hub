/**
 * @file bsp_eeprom.c
 * @brief AT24Cxx EEPROM I2C 驱动 + 掉电计数（T-06）
 *
 * 阶段 1（T-06）：掉电计数 — 拔电重启 +1 且校验和正确。
 *
 * 硬件：
 *   - I2C1（PB6-SCL / PB7-SDA），100kHz，与 OLED(0x3C)、BMP280(0x76) 共总线
 *   - EEPROM 地址 0x50（A0=A1=A2=GND）
 *
 * 写流程：字节/页写入 → HAL_Delay(5ms) 等待 tWR
 * 读流程：当前地址读取即可（无需等待周期）
 *
 * 来源：本工程新建
 */
#include "bsp_eeprom.h"
#include "i2c.h"

/* ════════════════════════════════════════════════════
 *  内部常量
 *  ════════════════════════════════════════════════════ */
#define I2C_HANDLE          (&hi2c1)
#define I2C_TIMEOUT_MS      20U

/* ════════════════════════════════════════════════════
 *  底层字节读写
 *  ════════════════════════════════════════════════════ */

uint8_t BSP_EEPROM_ReadByte(uint16_t addr, uint8_t *pdata)
{
    /*
     * AT24Cxx 读时序：
     *   START → DeviceAddr(W) → WordAddr → RESTART → DeviceAddr(R) → 读 1B → STOP
     *
     * HAL_I2C_Mem_Read 封装了以上时序。
     * MemAddress = 1 字节（对 ≤2KB 的 AT24C 足够）
     */
    return HAL_I2C_Mem_Read(I2C_HANDLE,
                            BSP_EEPROM_I2C_ADDR << 1,
                            addr,
                            I2C_MEMADD_SIZE_8BIT,
                            pdata, 1,
                            I2C_TIMEOUT_MS);
}

uint8_t BSP_EEPROM_WriteByte(uint16_t addr, uint8_t data)
{
    HAL_StatusTypeDef ret;

    ret = HAL_I2C_Mem_Write(I2C_HANDLE,
                            BSP_EEPROM_I2C_ADDR << 1,
                            addr,
                            I2C_MEMADD_SIZE_8BIT,
                            &data, 1,
                            I2C_TIMEOUT_MS);

    if (ret != HAL_OK) return 1;

    /* 等待内部写周期完成（tWR = 5ms）*/
    HAL_Delay(BSP_EEPROM_WRITE_MS);

    /* 轮询应答确认写入完成（可选加固）*/
    ret = HAL_I2C_IsDeviceReady(I2C_HANDLE,
                                BSP_EEPROM_I2C_ADDR << 1,
                                3, I2C_TIMEOUT_MS);
    return (ret == HAL_OK) ? 0 : 1;
}

/* ════════════════════════════════════════════════════
 *  批量读写（自动分页）
 *  ════════════════════════════════════════════════════ */

uint8_t BSP_EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(I2C_HANDLE,
                            BSP_EEPROM_I2C_ADDR << 1,
                            addr,
                            I2C_MEMADD_SIZE_8BIT,
                            buf, len,
                            100U /* 宽松超时 */);
}

uint8_t BSP_EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    uint16_t remaining = len;
    uint16_t offset    = 0;
    uint8_t  ret;

    while (remaining > 0) {
        /* 计算当前页剩余空间 */
        uint16_t page_end = ((addr / BSP_EEPROM_PAGE_SIZE) + 1)
                            * BSP_EEPROM_PAGE_SIZE;
        uint16_t chunk = page_end - addr;
        if (chunk > remaining) chunk = remaining;

        ret = HAL_I2C_Mem_Write(I2C_HANDLE,
                                BSP_EEPROM_I2C_ADDR << 1,
                                addr,
                                I2C_MEMADD_SIZE_8BIT,
                                (uint8_t *)(buf + offset), chunk,
                                100U);
        if (ret != HAL_OK) return 1;

        /* 等写周期 → 轮询确认 */
        HAL_Delay(BSP_EEPROM_WRITE_MS);
        ret = HAL_I2C_IsDeviceReady(I2C_HANDLE,
                                    BSP_EEPROM_I2C_ADDR << 1,
                                    3, I2C_TIMEOUT_MS);
        if (ret != HAL_OK) return 2;

        addr      += chunk;
        offset    += chunk;
        remaining -= chunk;
    }

    return 0;
}