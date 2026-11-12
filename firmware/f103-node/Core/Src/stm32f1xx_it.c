/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "proto_uart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
 * @brief Cortex-M3 HardFault 诊断输出（C 侧）
 *
 * 由 HardFault_Handler 的汇编 trampoline 调用，R0 = 异常栈帧指针。
 *
 * ⚠️ 现状（2026-10-18 盘点）：该汇编 trampoline **尚未接入** —— 矢量表里
 *    仍是默认的 `while(1)`，因此本函数当前不会被调用。保留待接线。
 * 栈帧布局（Cortex-M3 自动入栈）：
 *   [0]=R0  [1]=R1  [2]=R2  [3]=R3
 *   [4]=R12 [5]=LR  [6]=PC  [7]=xPSR
 */
void HardFault_Handler_C(uint32_t *stack_frame)
{
    uint32_t cfsr = *(volatile uint32_t *)0xE000ED28U; /* SCB->CFSR */
    uint32_t hfsr = *(volatile uint32_t *)0xE000ED2CU; /* SCB->HFSR */
    uint32_t bfar = *(volatile uint32_t *)0xE000ED38U; /* SCB->BFAR */

    printf("\r\n╔════════════════════════════════╗\r\n");
    printf("║      HARD FAULT               ║\r\n");
    printf("╠════════════════════════════════╣\r\n");
    printf("║ PC   = 0x%08lX            ║\r\n", stack_frame[6]);
    printf("║ LR   = 0x%08lX            ║\r\n", stack_frame[5]);
    printf("║ CFSR = 0x%08lX            ║\r\n", cfsr);
    printf("║ HFSR = 0x%08lX            ║\r\n", hfsr);
    printf("║ BFAR = 0x%08lX            ║\r\n", bfar);
    printf("╚════════════════════════════════╝\r\n");

    /* CFSR 各字段解码 */
    if (cfsr & 0x0001)  printf("  [CFSR] IACCVIOL: instruction access violation\r\n");
    if (cfsr & 0x0002)  printf("  [CFSR] DACCVIOL: data access violation\r\n");
    if (cfsr & 0x0100)  printf("  [CFSR] UNSTKERR: unstacking error\r\n");
    if (cfsr & 0x0200)  printf("  [CFSR] STKERR:   stacking error\r\n");
    if (cfsr & 0x0400)  printf("  [CFSR] MLSPERR:  floating-point lazy state error\r\n");
    if (cfsr & 0x00010000) printf("  [CFSR] UNDEFINSTR: undefined instruction\r\n");
    if (cfsr & 0x00020000) printf("  [CFSR] INVSTATE:  invalid state\r\n");
    if (cfsr & 0x00040000) printf("  [CFSR] INVPC:     invalid PC load\r\n");
    if (cfsr & 0x00080000) printf("  [CFSR] NOCP:      no coprocessor\r\n");
    if (cfsr & 0x02000000) printf("  [CFSR] UNALIGNED: unaligned access\r\n");
    if (cfsr & 0x04000000) printf("  [CFSR] DIVBYZERO: divide by zero\r\n");
    if (hfsr & 0x40000000) printf("  [HFSR] FORCED:    escalated from MemManage/BusFault/UsageFault\r\n");
    if (hfsr & 0x80000000) printf("  [HFSR] DEBUGEVT:  debug event\r\n");

    volatile uint32_t delay;
    for (delay = 0; delay < 10000000; delay++) { __NOP(); }

    /* 死循环 → IWDG 将复位设备 */
    while (1) { __NOP(); }
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_usart3_tx;
extern DMA_HandleTypeDef hdma_usart1_tx;
extern DMA_HandleTypeDef hdma_usart1_rx;
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart1;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M3 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32F1xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f1xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 channel2 global interrupt.
  */
void DMA1_Channel2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel2_IRQn 0 */

  /* USER CODE END DMA1_Channel2_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_tx);
  /* USER CODE BEGIN DMA1_Channel2_IRQn 1 */

  /* USER CODE END DMA1_Channel2_IRQn 1 */
}

/**
  * @brief This function handles USART3 global interrupt.
  */
void USART3_IRQHandler(void)
{
  /* USER CODE BEGIN USART3_IRQn 0 */

  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
  /* USER CODE BEGIN USART3_IRQn 1 */

  /* USER CODE END USART3_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel4 global interrupt (USART1_TX).
  */
void DMA1_Channel4_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart1_tx);
}

/**
  * @brief This function handles DMA1 channel5 global interrupt (USART1_RX).
  */
void DMA1_Channel5_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart1_rx);
}

/**
  * @brief This function handles USART1 global interrupt (IDLE detection).
  */
void USART1_IRQHandler(void)
{
  ProtoUart_NotifyIdle();
  HAL_UART_IRQHandler(&huart1);
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */