/**
 * @file bsp_dht11.c
 * @brief DHT11 温湿度传感器实现（单总线，DWT 微秒延时）
 *
 * 来源：前序工程 BSP/DHT11/driver_dht11.c
 * 本工程修改：
 *   - 阶段 1：引脚改从 bsp_pins.h 取；起始信号从 HAL_Delay(20) 改为 delay_us(20000)
 *   - 阶段 1（T-09）：起始脉冲的低电平 20ms **移出临界区**（对时序精度无要求），
 *     关中断窗口从约 24ms 降到约 5ms（30µs 拉高切换 + 约 5ms 响应与 40 位读取）。
 *     残留影响：每 2s 仍丢约 5 个 SysTick 节拍（≈0.125% 走慢），可接受；
 *     若要进一步消除，需把微秒延时从 DWT 忙等换成 TIM 基准（见验收报告 §2.3）。
 *   - ⚠️ 仍未实现：读取失败无重试、错误码无法区分"响应超时/校验错/无器件"
 */
#include "bsp_dht11.h"
#include "delay.h"

#include "bsp_pins.h"   /* T-15: 引脚集中 */

static void DHT11_Pin_Output(void)
{
    GPIO_InitTypeDef GPIO_InitStruct ={0};
    GPIO_InitStruct.Pin = DHT11_PIN;
    GPIO_InitStruct.Mode =GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed =GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT11_PORT,&GPIO_InitStruct);
}
static void DHT11_Pin_Input(void)
{
    GPIO_InitTypeDef GPIO_InitStruct ={0};
    GPIO_InitStruct.Pin = DHT11_PIN;
    GPIO_InitStruct.Mode =GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull =GPIO_PULLUP;

    HAL_GPIO_Init(DHT11_PORT,&GPIO_InitStruct);
}
static uint8_t DHT11_ReadPin(void)
{
    return HAL_GPIO_ReadPin(DHT11_PORT,DHT11_PIN);
}
static uint8_t DHT11_CheckResponse(void)
{
    uint32_t timeout = 0;

    /** 等待低电平 */
    while (DHT11_ReadPin())
    {
        delay_us(1);
        timeout++;
        if(timeout > 100)
        {
            return 0;
        }
    }

    timeout = 0;

    /** 等待高电平  */
    while(!DHT11_ReadPin())
    {
        delay_us(1);

        timeout++;

        if(timeout > 100)
        {
            return 0;
        }
    }

    timeout = 0;

    /** 等待响应结束  */
    while(DHT11_ReadPin())
    {
        delay_us(1);

        timeout++;

        if(timeout > 100)
        {
            return 0;
        }
    }

    return 1;
}
static uint8_t DHT11_ReadBit(void)
{
    uint32_t timeout = 0;

    /** 等待高电平开始 */
    while(!DHT11_ReadPin())
    {
        delay_us(1);

        timeout++;

        if(timeout > 100)
        {
            return 0;
        }
    }

    /** 等待40us */
    delay_us(40);

    /* 判断高低 */
    uint8_t bit = DHT11_ReadPin();

    /* 等待高电平结束 */
    timeout = 0;
    while (DHT11_ReadPin())
    {
        delay_us(1);
        timeout++;
        if (timeout > 100)
        {
            break;
        }
    }

    return bit;
}
static uint8_t DHT11_ReadByte(void)
{
    uint8_t i;

    uint8_t data = 0;

    for(i = 0; i < 8; i++)
    {
        data <<= 1;

        data |= DHT11_ReadBit();
    }

    return data;
}
uint8_t DHT11_Read(dht11_data_t *data)
{
    uint8_t buf[5];
    uint8_t i;

    /*
     * T-09 修复：DHT11 起始脉冲的低电平 20ms 对时序精度无要求，
     * 放在开中断环境执行，避免长时间关中断导致 SysTick 丢失。
     *
     * 关中断仅保护：
     *   1. 拉高 30µs + 切换输入方向（微秒级）
     *   2. 响应信号 + 40 位数据读取（约 5ms）
     * 总计关中断窗口约 5ms，远小于之前 ~25ms。
     */

    /* ── 步骤 1：拉低 20ms（开中断，无需时序精度）── */
    DHT11_Pin_Output();
    HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_RESET);
    delay_us(20000);

    /* ── 步骤 2：拉高 + 切换到输入（关中断，约 30µs）── */
    __disable_irq();

    HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
    delay_us(30);
    DHT11_Pin_Input();

    /* ── 步骤 3：响应 + 数据（关中断，约 5ms）── */
    if (DHT11_CheckResponse() == 0)
    {
        __enable_irq();
        return 0;
    }

    for (i = 0; i < 5; i++)
    {
        buf[i] = DHT11_ReadByte();
    }

    __enable_irq();

    /** 校验  */
    if((buf[0] + buf[1] +
        buf[2] + buf[3]) != buf[4])
    {
        return 0;
    }

    data->humidity = buf[0];

    data->temperature = buf[2];

    return 1;
}