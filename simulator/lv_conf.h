/**
 * @file lv_conf.h — PC 模拟器 LVGL 配置
 *
 * 与板上差异: 色深 32 位(板 RGB565)、Montserrat 14~48 全开。
 * 其余保持 LVGL 9.4 默认。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 色深: PC 32 位(板上是 16, UI 代码不感知差异) */
#define LV_COLOR_DEPTH 16

/* 主周期 */
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

/* 字体 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK 1
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK 1
#define LV_FONT_DEFAULT_MONTSERRAT_16 1

/* 功能裁剪: 模拟器只需要核心 + 演示控件 */
#define LV_USE_SDL    1        /* SDL2 宿主窗口驱动 */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB   /* 系统 malloc —— 内置 64KB 池装不下四页对象 */
#define LV_USE_CLIB_STRING 1
#define LV_USE_FS_MEMFS 1
#define LV_FS_MEMFS_LETTER 'M'   /* lv_binfont_create_from_buffer 需要 */
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1   /* radial 渐变调试 */
#define LV_USE_OS     LV_OS_NONE
#define LV_USE_FLEX   1
#define LV_USE_GRID   1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'A'
#define LV_FS_STDIO_PATH ""
#define LV_USE_SVG 1
#define LV_USE_VECTOR_GRAPHIC 1
#define LV_USE_MATRIX 1
#define LV_USE_FLOAT 1
#define LV_USE_DRAW_SW 1
#define LV_USE_THORVG_INTERNAL 1

/* 关闭板上才用的外设 */
#define LV_USE_LOTTIE  0
#define LV_USE_LODEPNG 1        /* 背景光晕 PNG */
#define LV_USE_FFMPEG  0
#define LV_USE_SNAPSHOT 1

/* 使用托管组件同源配置的默认项 */
#define LV_TICK_CUSTOM 0
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

#endif /* LV_CONF_H */
