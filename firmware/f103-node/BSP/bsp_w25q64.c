/**
 * @file bsp_w25q64.c
 * @brief W25Q64 8MB SPI Flash 驱动
 *
 * 阶段 1（T-10）修复：
 *   - 补 WEL 位校验：写使能后确认 WEL=1 才继续
 *   - 补 tPUW 上电等待：Init 时延时 5ms
 *   - 补 Block Erase (64KB) 和 Chip Erase
 *   - 跨页写入被拒绝（已有，保留）
 *
 * 来源：前序工程（smart_home），本工程修改
 */
#include "bsp_w25q64.h"
#include "spi.h"

#include "bsp_pins.h"   /* T-15: 引脚集中 */
#include "bsp_iwdg.h"   /* IAP：擦除/写入等待期间喂狗 */

#define W25Q64_CMD_WRITE_ENABLE  0x06
#define W25Q64_CMD_READ_STATUS1  0x05
#define W25Q64_CMD_PAGE_PROGRAM  0x02
#define W25Q64_CMD_READ_DATA     0x03
#define W25Q64_CMD_SECTOR_ERASE  0x20  /*  4KB Sector Erase  */
#define W25Q64_CMD_BLOCK_ERASE   0xD8  /* 64KB Block  Erase  */
#define W25Q64_CMD_CHIP_ERASE    0xC7  /*     Chip Erase      */
#define W25Q64_CMD_JEDEC_ID      0x9F

#define W25Q64_SIZE_BYTES       0x800000UL
#define W25Q64_PAGE_SIZE        256U
#define W25Q64_SECTOR_SIZE      4096U
#define W25Q64_BLOCK_SIZE       65536U
#define W25Q64_SPI_TIMEOUT_MS   100U
#define W25Q64_BUSY_TIMEOUT_MS  1000U
/* 整片擦除专用超时：手册典型 40~80s，取 200s 留足余量（验收报告 §2.2 第 9 项） */
#define W25Q64_CHIP_ERASE_TIMEOUT_MS  200000U

/* tPUW: 上电后到可接受第一条指令的最大时长（W25Q64FV）*/
#define W25Q64_TPUW_MS 5U

/* 状态寄存器位定义 */
#define W25Q64_SR_BUSY 0x01U
#define W25Q64_SR_WEL  0x02U

static void W25Q64_CS_Low(void)
{
    HAL_GPIO_WritePin(W25Q64_CS_PORT, W25Q64_CS_PIN, GPIO_PIN_RESET);
}

static void W25Q64_CS_High(void)
{
    HAL_GPIO_WritePin(W25Q64_CS_PORT, W25Q64_CS_PIN, GPIO_PIN_SET);
}

static uint8_t W25Q64_SwapByte(uint8_t data)
{
    uint8_t rx = 0xFF;

    (void)HAL_SPI_TransmitReceive(&hspi1,
                                  &data,
                                  &rx,
                                  1,
                                  W25Q64_SPI_TIMEOUT_MS);

    return rx;
}

void BSP_W25Q64_Init(void)
{
    W25Q64_CS_High();

    /* T-10: 上电等待 tPUW (max 5ms)，确保器件可接受指令 */
    HAL_Delay(W25Q64_TPUW_MS);
}

uint32_t BSP_W25Q64_ReadID(void)
{
    uint8_t id[3];

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_JEDEC_ID);
    id[0] = W25Q64_SwapByte(0xFF);
    id[1] = W25Q64_SwapByte(0xFF);
    id[2] = W25Q64_SwapByte(0xFF);
    W25Q64_CS_High();

    return ((uint32_t)id[0] << 16) |
           ((uint32_t)id[1] << 8)  |
           (uint32_t)id[2];
}

static uint8_t W25Q64_ReadStatus(void)
{
    uint8_t status;

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_READ_STATUS1);
    status = W25Q64_SwapByte(0xFF);
    W25Q64_CS_High();

    return status;
}

/**
 * @brief 等待器件空闲（可指定超时）
 *
 * 阶段 1 验收整改（报告 §2.2 第 9 项）：不同操作的耗时差异巨大——
 * 页编程 <5ms、扇区擦除 <400ms、**整片擦除 40~80s**。
 * 原先只有一个 1000ms 的固定上限，导致 EraseChip 必然在 1s 后误判"失败"，
 * 而器件实际仍在擦除（此时再发指令会被 BUSY 吞掉）。
 * 故改为由调用方给出合适上限。
 *
 * @param timeout_ms 超时上限
 * @return 1=已空闲，0=超时
 */
static uint8_t W25Q64_WaitBusyMs(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while (W25Q64_ReadStatus() & W25Q64_SR_BUSY)
    {
        /* 阶段 8 IAP：整块擦除可达 1~2s，必须在这里喂狗，
         * 否则 app 2s 看门狗会在 OTA 备份/擦除阶段误复位。 */
        BSP_IWDG_Feed();
        if ((HAL_GetTick() - start) > timeout_ms)
        {
            return 0;
        }
    }

    return 1;
}

static uint8_t W25Q64_WaitBusy(void)
{
    return W25Q64_WaitBusyMs(W25Q64_BUSY_TIMEOUT_MS);
}

/**
 * @brief 写使能 + WEL 位校验（T-10）
 * @return 1=WEL 已置位，0=置位失败或器件未就绪
 */
static uint8_t W25Q64_WriteEnable(void)
{
    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_WRITE_ENABLE);
    W25Q64_CS_High();

    /* T-10: 回读 SR1，确认 WEL=1 */
    uint8_t sr = W25Q64_ReadStatus();
    return (sr & W25Q64_SR_WEL) ? 1 : 0;
}

uint8_t BSP_W25Q64_Write(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint16_t i;
    uint32_t page_remain = W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE);

    if(buf == 0 || len == 0 || addr >= W25Q64_SIZE_BYTES ||
       len > page_remain || (addr + len) > W25Q64_SIZE_BYTES)
    {
        return 0;
    }

    /* T-10 整改（验收报告 §2.2 第 8 项）：**必须先等器件空闲再发 WREN**。
     * 器件在上一笔编程/擦除进行中会忽略 WREN（WEL 保持 0），
     * 顺序反了会让 WriteEnable() 的 WEL 回读判定失败，
     * 表现为"连续写入/擦除时间歇性失败"，且极难归因。 */
    if (!W25Q64_WaitBusy())
    {
        return 0;
    }

    if (!W25Q64_WriteEnable())
    {
        return 0;
    }

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_PAGE_PROGRAM);
    W25Q64_SwapByte((uint8_t)(addr >> 16));
    W25Q64_SwapByte((uint8_t)(addr >> 8));
    W25Q64_SwapByte((uint8_t)addr);

    for (i = 0; i < len; i++)
    {
        W25Q64_SwapByte(buf[i]);
    }

    W25Q64_CS_High();

    return W25Q64_WaitBusy();
}

void BSP_W25Q64_Read(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if(buf == 0 || len == 0 || addr >= W25Q64_SIZE_BYTES ||
       (addr + len) > W25Q64_SIZE_BYTES)
    {
        return;
    }

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_READ_DATA);
    W25Q64_SwapByte((uint8_t)(addr >> 16));
    W25Q64_SwapByte((uint8_t)(addr >> 8));
    W25Q64_SwapByte((uint8_t)addr);

    for(i = 0; i < len; i++)
    {
        buf[i] = W25Q64_SwapByte(0xFF);
    }

    W25Q64_CS_High();
}

uint8_t BSP_W25Q64_EraseSector(uint32_t addr)
{
    if (addr >= W25Q64_SIZE_BYTES)
    {
        return 0;
    }

    /* 地址向下对齐到 4KB */
    addr &= ~(W25Q64_SECTOR_SIZE - 1U);

    /* T-10 整改（验收报告 §2.2 第 8 项）：**必须先等器件空闲再发 WREN**。
     * 器件在上一笔编程/擦除进行中会忽略 WREN（WEL 保持 0），
     * 顺序反了会让 WriteEnable() 的 WEL 回读判定失败，
     * 表现为"连续写入/擦除时间歇性失败"，且极难归因。 */
    if (!W25Q64_WaitBusy())
    {
        return 0;
    }

    if (!W25Q64_WriteEnable())
    {
        return 0;
    }

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_SECTOR_ERASE);
    W25Q64_SwapByte((uint8_t)(addr >> 16));
    W25Q64_SwapByte((uint8_t)(addr >> 8));
    W25Q64_SwapByte((uint8_t)addr);
    W25Q64_CS_High();

    return W25Q64_WaitBusy();
}

/**
 * @brief 块擦除（64KB Block Erase, CMD 0xD8）— T-10 新增
 * @param addr 块内任意地址，自动向下对齐到 64KB 边界
 * @return 1=成功, 0=超时或地址越界
 */
uint8_t BSP_W25Q64_EraseBlock(uint32_t addr)
{
    if (addr >= W25Q64_SIZE_BYTES)
    {
        return 0;
    }

    /* 地址向下对齐到 64KB */
    addr &= ~(W25Q64_BLOCK_SIZE - 1U);

    /* T-10 整改（验收报告 §2.2 第 8 项）：**必须先等器件空闲再发 WREN**。
     * 器件在上一笔编程/擦除进行中会忽略 WREN（WEL 保持 0），
     * 顺序反了会让 WriteEnable() 的 WEL 回读判定失败，
     * 表现为"连续写入/擦除时间歇性失败"，且极难归因。 */
    if (!W25Q64_WaitBusy())
    {
        return 0;
    }

    if (!W25Q64_WriteEnable())
    {
        return 0;
    }

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_BLOCK_ERASE);
    W25Q64_SwapByte((uint8_t)(addr >> 16));
    W25Q64_SwapByte((uint8_t)(addr >> 8));
    W25Q64_SwapByte((uint8_t)addr);
    W25Q64_CS_High();

    /* 64KB 块擦除手册最大约 2s，默认 1s 上限在部分批次会误判失败 */
    return W25Q64_WaitBusyMs(3000U);
}

/**
 * @brief 全片擦除（Chip Erase, CMD 0xC7）— T-10 新增
 *
 * W25Q64 全片擦除约需 40~80 秒，调用后必须等 WaitBusy 返回 1。
 * @return 1=成功, 0=超时
 */
uint8_t BSP_W25Q64_EraseChip(void)
{
    /* 同 Write/EraseSector：先等空闲再 WREN（T-10 整改） */
    if (!W25Q64_WaitBusy())
    {
        return 0;
    }

    if (!W25Q64_WriteEnable())
    {
        return 0;
    }

    W25Q64_CS_Low();
    W25Q64_SwapByte(W25Q64_CMD_CHIP_ERASE);
    W25Q64_CS_High();

    /* 整片擦除用专用长超时（40~80s），不能用默认的 1s —— 否则必然误判失败，
     * 而器件实际仍在擦除，此时再发指令会被 BUSY 吞掉（验收报告 §2.2 第 9 项）。 */
    return W25Q64_WaitBusyMs(W25Q64_CHIP_ERASE_TIMEOUT_MS);
}