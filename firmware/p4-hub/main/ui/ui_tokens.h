/*
 * ui_tokens.h — design tokens, single source: design-system/smart-home-hub/MASTER.md
 *
 * 每个常量与 token 库（Penpot "Tokens" set / HTML CSS variables）一一对应。
 * 改值必须与设计文档和 LVGL 页面同步。
 */
#pragma once
#include <stdint.h>

/* ── color tokens ── */
#define TK_COLOR_BACKGROUND   0x0F172A  /* color/background  屏幕底色        */
#define TK_COLOR_FOREGROUND   0xF8FAFC  /* color/foreground  主文字          */
#define TK_COLOR_PRIMARY      0x1E293B  /* color/primary     主结构色        */
#define TK_COLOR_SECONDARY    0x334155  /* color/secondary   次级结构色      */
#define TK_COLOR_ACCENT       0x22C55E  /* color/accent      主操作/选中/在线 */
#define TK_COLOR_ON_ACCENT    0x0F172A  /* color/on-accent   accent 上深字    */
#define TK_COLOR_CARD         0x1B2336  /* color/card        不透明卡片底     */
#define TK_COLOR_CARD_FG      0xF8FAFC  /* color/card-fg     卡片文字        */
#define TK_COLOR_MUTED        0x272F42  /* color/muted       弱化区块/芯片底  */
#define TK_COLOR_MUTED_FG     0x94A3B8  /* color/muted-fg    次级文字         */
#define TK_COLOR_BORDER       0x475569  /* color/border      普通描边         */
#define TK_COLOR_DANGER       0xEF4444  /* color/danger      告警（唯一大面积饱和色） */
#define TK_COLOR_ACCENT_BLUE  0x60A5FA  /* color/accent-blue 制冷/湿度语义   */
#define TK_COLOR_ACCENT_WARM  0xF59E0B  /* color/accent-warm 暖气/窗帘语义    */
#define TK_COLOR_LINK_STALE   0x64748B  /* color/link-stale  链路 DOWN 灰态   */

/* 玻璃材质 alpha（LVGL: 白色 + 独立 opa；0-255） */
#define TK_GLASS_STROKE_OPA   51   /* color/stroke-glass  white @ 20% 卡片描边 */
#define TK_GLASS_EDGE_OPA     77   /* color/edge-light    white @ 30% 顶缘高光 */

/* ── radius tokens ── */
#define TK_RADIUS_SM  8   /* 按钮/芯片/胶囊 */
#define TK_RADIUS_MD  12  /* 卡片          */
#define TK_RADIUS_LG  16  /* 面板/Dock/Toast */

/* ── layout tokens ── */
#define TK_CANVAS_W     1024
#define TK_CANVAS_H     600
#define TK_STATUSBAR_H  64
#define TK_DOCK_H       64
#define TK_PAGE_MARGIN  32
#define TK_CARD_GAP     16
#define TK_CHIP_SIZE    40
#define TK_TOUCH_MIN    44

/* ── 触控热区放大：toggle 视觉 44×24，命中区扩到 44×44（Penpot touch-target/44） ── */
#define TK_TOGGLE_W     44
#define TK_TOGGLE_H     24
#define TK_TOGGLE_HIT_PAD 10
