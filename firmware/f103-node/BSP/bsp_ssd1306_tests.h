/**
 * @file bsp_ssd1306_tests.h
 * @brief SSD1306 自测代码声明（本工程不参与编译）
 *
 * 来源：第三方库 afiskon/stm32-ssd1306
 * 本工程修改：
 *   - 阶段 1：随 bsp_ssd1306_tests.c 一起被排除出构建
 */
#ifndef __SSD1306_TEST_H__
#define __SSD1306_TEST_H__

#include <_ansi.h>

_BEGIN_STD_C

void ssd1306_TestBorder(void);
void ssd1306_TestFonts1(void);
void ssd1306_TestFonts2(void);
void ssd1306_TestFPS(void);
void ssd1306_TestAll(void);
void ssd1306_TestLine(void);
void ssd1306_TestRectangle(void);
void ssd1306_TestRectangleFill(void);
void ssd1306_TestRectangleInvert(void);
void ssd1306_TestCircle(void);
void ssd1306_TestArc(void);
void ssd1306_TestPolyline(void);
void ssd1306_TestDrawBitmap(void);

_END_STD_C

#endif // __SSD1306_TEST_H__
