/**
 * @file main.c — LVGL PC 模拟器入口(SDL2 宿主)
 * 渲染沉降后把屏幕快照导出为 PPM（radial/位图等视觉调试闭环），
 * 再进入事件循环（关窗退出）。
 */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui_pages.h"
#include "app_proto.h"
#include "storage.h"
#include "wifi.h"
#include "others/snapshot/lv_snapshot.h"
#include "misc/lv_grad.h"
#include "draw/lv_image_decoder_private.h"
#include "font/lv_binfont_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size;
    lv_font_t *font;
} runtime_font_t;

static runtime_font_t runtime_fonts[6];
static runtime_font_t runtime_latin_font;

static void dump_widget_state(void)
{
    lv_obj_t *screen = lv_screen_active();
    printf("[UI] screen children=%u\n", (unsigned)lv_obj_get_child_count(screen));
}

static lv_font_t *load_runtime_font_from(const char *dir, int slot, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if(!f) { printf("[FONT] missing %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0) { fclose(f); return NULL; }
    runtime_fonts[slot].data = (uint8_t *)malloc((size_t)n);
    runtime_fonts[slot].size = (size_t)n;
    if(!runtime_fonts[slot].data || fread(runtime_fonts[slot].data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(runtime_fonts[slot].data); runtime_fonts[slot].data = NULL; return NULL;
    }
    fclose(f);
    runtime_fonts[slot].font = lv_binfont_create_from_buffer(runtime_fonts[slot].data, (uint32_t)n);
    printf("[FONT] %s -> %s\n", name, runtime_fonts[slot].font ? "loaded" : "failed");
    return runtime_fonts[slot].font;
}
static lv_font_t *load_runtime_cn_font(int slot, const char *name)
{
    char dir[512]; snprintf(dir, sizeof(dir), "%s/cn_font", UI_SIMULATOR_FONT_DIR);
    return load_runtime_font_from(dir, slot, name);
}
static lv_font_t *load_runtime_latin_font(const char *name)
{
    char dir[512]; snprintf(dir, sizeof(dir), "%s/en_font", UI_SIMULATOR_FONT_DIR);
    snprintf((char *)dir + strlen(dir), sizeof(dir) - strlen(dir), "/%s", name);
    FILE *f = fopen(dir, "rb");
    if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    runtime_latin_font.data=(uint8_t*)malloc((size_t)n); runtime_latin_font.size=(size_t)n;
    if(!runtime_latin_font.data || fread(runtime_latin_font.data,1,(size_t)n,f)!=(size_t)n){fclose(f);return NULL;}
    fclose(f); runtime_latin_font.font=lv_binfont_create_from_buffer(runtime_latin_font.data,(uint32_t)n);
    printf("[FONT] %s -> %s\n", name, runtime_latin_font.font ? "loaded" : "failed");
    return runtime_latin_font.font;
}

static bool has_arg(int argc, char **argv, const char *arg)
{
    return argc > 1 && strcmp(argv[1], arg) == 0;
}

static const char *detail_arg(int argc, char **argv)
{
    return argc > 1 ? argv[1] : NULL;
}

static int page_arg(const char *arg)
{
    if(!arg || strncmp(arg, "--page-", 7) != 0) return -1;
    if(strcmp(arg, "--page-overview") == 0) return 0;
    if(strcmp(arg, "--page-environment") == 0) return 1;
    if(strcmp(arg, "--page-devices") == 0) return 2;
    if(strcmp(arg, "--page-automation") == 0) return 3;
    if(strcmp(arg, "--page-alerts") == 0) return 4;
    if(strcmp(arg, "--page-settings") == 0) return 5;
    if(strcmp(arg, "--page-network") == 0) return 6;
    if(strcmp(arg, "--page-display") == 0) return 7;
    return -1;
}

static void create_radial_diag(void)
{
    static lv_style_t style;
    static lv_grad_dsc_t grad;
    static const lv_color_t colors[] = {
        LV_COLOR_MAKE(0x22, 0xC5, 0x5E),
        LV_COLOR_MAKE(0x0F, 0x17, 0x2A),
    };
    static const lv_opa_t opa[] = { LV_OPA_COVER, LV_OPA_COVER };
    lv_style_init(&style);
    lv_grad_init_stops(&grad, colors, opa, NULL, 2);
    lv_grad_radial_init(&grad, LV_GRAD_CENTER, LV_GRAD_CENTER,
                        LV_GRAD_RIGHT, LV_GRAD_BOTTOM, LV_GRAD_EXTEND_PAD);
    lv_style_set_bg_grad(&style, &grad);
    lv_style_set_bg_opa(&style, LV_OPA_COVER);
    lv_style_set_radius(&style, 0);
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &style, 0);
    lv_obj_set_size(obj, 1024, 600);
    lv_obj_center(obj);
}

static void dump_ppm(const char *path)
{
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!buf) { printf("[DUMP] snapshot failed\n"); fflush(stdout); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { lv_draw_buf_destroy(buf); return; }
    uint32_t w = buf->header.w, h = buf->header.h;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    const uint16_t *px = (const uint16_t *)buf->data;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint16_t v = px[y * (buf->header.stride / 2) + x];
            uint8_t rgb[3] = {
                (uint8_t)(((v >> 11) & 0x1F) << 3),
                (uint8_t)(((v >> 5) & 0x3F) << 2),
                (uint8_t)((v & 0x1F) << 3),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    lv_draw_buf_destroy(buf);
    printf("[DUMP] wrote %s (%ux%u RGB565)\n", path, w, h);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[SIM] font-v2 1024x600 | font_dir=%s\n", UI_SIMULATOR_FONT_DIR);
    SDL_SetMainReady();
    bool radial_diag = has_arg(argc, argv, "--radial");
    bool radial_home = has_arg(argc, argv, "--radial-home");
    const char *detail = detail_arg(argc, argv);
    int requested_page = page_arg(detail);
    if (argc > 1) {
        /* Headless snapshots must not create a visible/flashy SDL window. */
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    }
    SDL_Init(SDL_INIT_VIDEO);

    lv_init();
    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    if (!disp) { printf("lv_sdl_window_create failed\n"); return 1; }
    lv_sdl_mouse_create();

    if (radial_diag) create_radial_diag();
    else {
        lv_font_t *cn14 = load_runtime_cn_font(0, "fontcn14.bin");
        lv_font_t *cn16 = load_runtime_cn_font(1, "fontcn16.bin");
        lv_font_t *cn18 = load_runtime_cn_font(2, "fontcn18.bin");
        lv_font_t *cn24 = load_runtime_cn_font(3, "fontcn24.bin");
        lv_font_t *cn32 = load_runtime_cn_font(4, "fontcn32.bin");
        lv_font_t *cn48 = load_runtime_cn_font(5, "fontcn48.bin");
        lv_font_t *latin14 = load_runtime_latin_font("font_pjs_14.bin");
        ui_pages_set_fonts(cn14, cn16, cn18, cn24, cn32, cn48);
        ui_pages_set_latin_font(latin14);
        storage_init();
        wifi_init();
        ui_pages_create(lv_screen_active());
        if(requested_page >= 0) ui_pages_debug_show_page((uint8_t)requested_page);
        if(radial_home) ui_pages_set_radial_background(true);
        if (detail && strcmp(detail, "--detail-curtain") == 0)
            ui_pages_debug_show_device(PROTO_DEV_LOCK);
        else if (detail && strcmp(detail, "--detail-ac") == 0)
            ui_pages_debug_show_device(PROTO_DEV_DHT11_TEMP);
        else if (detail && strcmp(detail, "--detail-lamp") == 0)
            ui_pages_debug_show_device(PROTO_DEV_RELAY1);
    }

    /* SIM_WARM=N: 先跑 N 次实时刷新（模拟器时钟来自 SDL 真实时间，
     * 1s 定时器在快照窗口内不会触发），把需要时间积累的温度趋势折线截出来。 */
    const char *warm = getenv("SIM_WARM");
    if (warm) ui_pages_debug_refresh(atoi(warm));

    for (int i = 0; i < 30; i++) {          /* 渲染沉降 */
        uint32_t next = lv_timer_handler();
        SDL_Delay(next < 5 ? 5 : next);
    }
    if (argc > 1) {
        /* 快照模式：dump 完立即退出（自动化调试用，不留窗口） */
        const char *out = (radial_diag || radial_home || requested_page >= 0 ||
                           (detail && strncmp(detail, "--detail-", 9) == 0)) ?
                          (argc > 2 ? argv[2] : "diag.ppm") : argv[1];
        dump_ppm(out);
        SDL_Quit();
        return 0;
    }

    int frames = 0;                          /* 交互模式：窗口保持，关窗即退 */
    while (frames++ < 6000) {
        uint32_t next = lv_timer_handler();
        SDL_Delay(next < 5 ? 5 : next);
        if((frames % 120) == 0) dump_widget_state();
    }
    return 0;
}
