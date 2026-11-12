/*
 * ui_pages.h — 六页硬件式 UI + Product Rail 导航（LVGL）
 *
 * 1024×600 HTML 原型的 1:1 LVGL 映射：暖白主屏、深色外壳、左侧轨道。
 * 页面/卡片/文字/按钮/弹窗分别映射为 lv_obj、lv_label 和可点击 lv_obj。
 *
 * 视觉准绳：阶段三确定的暖白主屏、深色产品轨道和香槟金强调色。
 * 中文使用 LVGL Source Han Sans 子集；拉丁数字使用 Montserrat。
 */
#pragma once
#include <stdint.h>
#include "lvgl.h"

/* 在 LVGL 任务上下文调用（lv_async_call / lvgl_port ready 之后）。
 * 创建 6 个页面容器 + 常驻 Product Rail，触摸轨道切换页面。 */
void ui_pages_create(lv_obj_t *parent);
void ui_pages_set_radial_background(bool enabled);

/* Optional runtime fonts (used by the simulator and the SD-card font loader).
 * NULL keeps the built-in LVGL fallback. */
void ui_pages_set_fonts(const lv_font_t *cn14, const lv_font_t *cn16,
                        const lv_font_t *cn18, const lv_font_t *cn24,
                        const lv_font_t *cn32, const lv_font_t *cn48);
void ui_pages_set_latin_font(const lv_font_t *latin);

/* Load the same BinFont assets used by simulator/fonts from an SD mount.
 * Must be called after LVGL is initialized and before ui_pages_create(). */
void ui_pages_load_sd_fonts(const char *mount_point);

/* 按字号取当前生效的中文字体（SD BinFont 优先，否则内置回退）。
 * 供待机层等覆盖层复用同一套字体，避免重复加载。Lifetime: 进程级。 */
const lv_font_t *ui_pages_font(uint8_t size);

/* Simulator-only convenience for deterministic visual snapshots. */
void ui_pages_debug_show_device(uint8_t id);
void ui_pages_debug_show_page(uint8_t id);
/* 立即跑 times 次实时刷新（跳过 1s 定时器），把需要时间积累的界面
 * （温度趋势折线）在 PC 上截出可看的形态。仅模拟器使用。 */
void ui_pages_debug_refresh(int times);
