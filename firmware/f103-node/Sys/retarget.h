/**
 * @file retarget.h
 * @brief printf 重定向接口
 *
 * 来源：前序工程 USER/Retarget/retarget.h
 * 本工程修改：
 *   - 阶段 1：随实现一起从 USER/ 移到 Sys/
 */
#ifndef _RETARGET_H__
#define _RETARGET_H__

#include "stm32f1xx_hal.h"
#include <sys/stat.h>
#include <stdio.h>

void RetargetInit(UART_HandleTypeDef *huart);

void RetargetFlush(void);

int _isatty(int fd);

int _write(int fd, char *ptr, int len);

int _close(int fd);

int _lseek(int fd, int ptr, int dir);

int _read(int fd, char *ptr, int len);

int _fstat(int fd, struct stat *st);

#endif //#ifndef _RETARGET_H__