/**
 * @file app_uart.h
 * @brief 串口汇总输出接口（USART3，调试用）
 *
 * 来源：前序工程 APP/uart/app_uart.h
 * 本工程修改：
 *   - 阶段 1：输出口定为 USART3；USART1 专供协议，不允许出现文本输出
 */
//
// Created by A on 2026/6/1.
//

#ifndef SMART_HOME_APP_UART_H
#define SMART_HOME_APP_UART_H
void APP_Uart_Task(void);
#endif //SMART_HOME_APP_UART_H
