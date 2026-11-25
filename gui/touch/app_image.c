/* gui/touch/app_image.c — просмотрщик изображений PNG для тач-оболочки.
 *
 * Работает в двух режимах:
 *   - MODE_LIST: показывает все .png файлы из RamFS (при запуске с рабочего стола).
 *   - MODE_VIEW: декодирует PNG через gui/png.c и отображает с масштабированием
 *                (FIT / 1:1 / зум 25%..400%), панорамированием пальцем и
 *                переключением фона для прозрачных изображений.
 *
 * Также открывается напрямую из файлового менеджера (app_image_open).
 * Вся арифметика целочисленная (на ARM64 запрещены FP/SIMD регистры).
 */
#include "touch_app.h"
#include "touch_theme.h"
#include "icon.h"
#include "gfxfb.h"
#include "hal_input.h"
#include "ramfs.h"
#include "png.h"

#define MODE_LIST 0
#define MODE_VIEW 1

#define BG_CHECKER 0
#define BG_DARK    1
#define BG_LIGHT   2

#define PNG_MAX_FILE_BYTES (1024u * 1024u)

extern const uint8_t g_sample_about_png[];
extern const size_t  g_sample_about_png_len;

static uint8_t  g_file_buf[PNG_MAX_FILE_BYTES];
static uint32_t g_img_pixels[PNG_MAX_WIDTH * PNG_MAX_HEIGHT];

static int g_x, g_y, g_w, g_h;
static int g_mode = MODE_LIST;
static int g_from_files = 0;

/* Состояние списка PNG */
static const ramfs_file_t *g_png_list[RAMFS_MAX_ENTRIES];
static int g_png_count = 0;
static int g_list_scroll = 0;
static int g_pressed_row = -1;
static int g_drag_y0 = 0;
static int g_drag_scroll0 = 0;
static int g_drag_dist = 0;

/* Состояние текущего изображения */
static char       g_path[RAMFS_MAX_PATH_LEN];
static png_info_t g_info;
static int        g_has_image = 0;
static const char *g_err_msg = 0;

/* Масштаб и панорамирование */
static int g_zoom_fit = 1;
static int g_zoom_pct = 100;
static int g_pan_x = 0;
static int g_pan_y = 0;
static int g_bg_mode = BG_CHECKER;

static int g_pan_x0 = 0, g_pan_y0 = 0;
static int g_pan_sx = 0, g_pan_sy = 0;
static int g_pressed_btn = -1;

enum {
    BTN_BACK = 0,
    BTN_FIT  = 1,
    BTN_ZOUT = 2,
    BTN_ZIN  = 3,
    BTN_BG   = 4
};

/* ---------------- Строки и расширения ---------------- */

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') name = p + 1;
    return name;
}

static int ends_with_png(const char *name) {
    int n = 0;
    while (name[n]) n++;
    if (n < 4) return 0;
    char a = name[n - 4], b = name[n - 3], c = name[n - 2], d = name[n - 1];
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (d >= 'A' && d <= 'Z') d += 'a' - 'A';
    return a == '.' && b == 'p' && c == 'n' && d == 'g';
}

static void collect_pngs_dir(const char *dir) {
    const ramfs_file_t *entries[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(dir, entries, RAMFS_MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        if (entries[i]->is_dir) {
            collect_pngs_dir(entries[i]->path);
        } else if (ends_with_png(entries[i]->name) ||
                   png_is_png(entries[i]->data, ramfs_inline_len(entries[i]))) {
            if (g_png_count < RAMFS_MAX_ENTRIES)
                g_png_list[g_png_count++] = entries[i];
        }
    }
}

static void refresh_png_list(void) {
    g_png_count = 0;
    collect_pngs_dir("/");
    int total_rows = 1 + g_png_count; /* 0 = встроенный about.png */
    int max_scroll = total_rows * TM.touch - (g_h - TM.touch);
    if (max_scroll < 0) max_scroll = 0;
    if (g_list_scroll > max_scroll) g_list_scroll = max_scroll;
    if (g_list_scroll < 0) g_list_scroll = 0;
}

/* ---------------- Загрузка и декодирование PNG ---------------- */

static void decode_png_buf(const uint8_t *buf, size_t len, const char *path) {
    str_copy(g_path, path, sizeof(g_path));
    g_has_image = 0;
    g_err_msg   = 0;
    g_zoom_fit  = 1;
    g_zoom_pct  = 100;
    g_pan_x     = 0;
    g_pan_y     = 0;

    int err = png_decode(buf, len, g_img_pixels,
                         PNG_MAX_WIDTH, PNG_MAX_HEIGHT, &g_info);
    if (err != PNG_OK) {
        g_err_msg = png_strerror(err);
        return;
    }
    g_has_image = 1;
}

static void load_png(const char *path) {
    str_copy(g_path, path, sizeof(g_path));
    g_has_image = 0;
    g_err_msg   = 0;
    g_zoom_fit  = 1;
    g_zoom_pct  = 100;
    g_pan_x     = 0;
    g_pan_y     = 0;

    const ramfs_file_t *f = ramfs_lookup(path);
    if (!f || f->is_dir) {
        g_err_msg = "FILE NOT FOUND";
        return;
    }
    if (f->size == 0) {
        g_err_msg = "EMPTY FILE";
        return;
    }
    if (f->size > PNG_MAX_FILE_BYTES) {
        g_err_msg = "FILE TOO LARGE (>1MB)";
        return;
    }

    size_t got = 0;
    while (got < f->size) {
        long r = ramfs_read(path, got, g_file_buf + got, f->size - got);
        if (r <= 0) {
            g_err_msg = "READ ERROR";
            return;
        }
        got += (size_t)r;
    }

    decode_png_buf(g_file_buf, got, path);
}

void app_image_open(const char *path, int from_files) {
    g_from_files = from_files;
    g_mode = MODE_VIEW;
    load_png(path);
    touch_ui_open("IMAGE");
}

/* ---------------- Геометрия интерфейса ---------------- */

static int header_h(void) { return TM.touch; }
static int bar_h(void)    { return TM.touch + TM.pad; }

static void back_btn_rect(int *x, int *y, int *w, int *h) {
    *x = g_x; *y = g_y; *w = TM.touch; *h = header_h();
}

static void fit_btn_rect(int *x, int *y, int *w, int *h) {
    *w = TM.touch * 2;
    *h = header_h() - TM.pad;
    *x = g_x + g_w - *w - TM.pad;
    *y = g_y + (header_h() - *h) / 2;
}

static void view_rect(int *x, int *y, int *w, int *h) {
    *x = g_x;
    *y = g_y + header_h();
    *w = g_w;
    *h = g_h - header_h() - bar_h();
    if (*h < TM.touch) *h = TM.touch;
}

static void bar_btn_rects(int *zx0, int *zy0, int *zw, int *zh,
                          int *ix0, int *iy0, int *iw, int *ih,
                          int *bx0, int *by0, int *bw, int *bh) {
    int by = g_y + g_h - bar_h();
    int bh_ = bar_h() - TM.pad;
    int y = by + (bar_h() - bh_) / 2;
    int btn_w = TM.touch + TM.pad * 2;

    *zx0 = g_x + TM.pad;
    *zy0 = y; *zw = btn_w; *zh = bh_;

    *ix0 = *zx0 + btn_w + TM.pad;
    *iy0 = y; *iw = btn_w; *ih = bh_;

    *bw = TM.touch + TM.pad * 2;
    *bh = bh_;
    *bx0 = g_x + g_w - *bw - TM.pad;
    *by0 = y;
}

/* Вычислить размер изображения на экране в текущем режиме масштаба. */
static void compute_scaled_size(int vw, int vh, int *dw, int *dh) {
    int iw = (int)g_info.width;
    int ih = (int)g_info.height;
    if (iw <= 0 || ih <= 0) { *dw = 0; *dh = 0; return; }

    if (g_zoom_fit) {
        int aw = vw - TM.pad * 2;
        int ah = vh - TM.pad * 2;
        if (aw < 16) aw = vw;
        if (ah < 16) ah = vh;
        if ((int64_t)iw * ah <= (int64_t)ih * aw) {
            *dh = ah;
            *dw = (int)((int64_t)iw * ah / ih);
        } else {
            *dw = aw;
            *dh = (int)((int64_t)ih * aw / iw);
        }
    } else {
        *dw = iw * g_zoom_pct / 100;
        *dh = ih * g_zoom_pct / 100;
    }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

static void clamp_pan(int vw, int vh, int dw, int dh) {
    int max_x = (dw > vw) ? (dw - vw) / 2 + TM.pad : 0;
    int max_y = (dh > vh) ? (dh - vh) / 2 + TM.pad : 0;
    if (g_pan_x >  max_x) g_pan_x =  max_x;
    if (g_pan_x < -max_x) g_pan_x = -max_x;
    if (g_pan_y >  max_y) g_pan_y =  max_y;
    if (g_pan_y < -max_y) g_pan_y = -max_y;
}

/* ---------------- Отрисовка ---------------- */

static void draw_header(const char *title, int show_fit_btn) {
    int hh = header_h();
    hal_gfx_fill_rect(g_x, g_y, g_w, hh, GFX_UI_SURFACE_2);
    hal_gfx_fill_rect(g_x, g_y + hh - 1, g_w, 1, GFX_UI_DIVIDER);

    int bx, by, bw, bh;
    back_btn_rect(&bx, &by, &bw, &bh);
    int is = hh * 55 / 100;
    icon_draw(ICON_ARROW_BACK, bx + (bw - is) / 2, by + (bh - is) / 2, is,
              hal_gfx_palette_rgb(GFX_UI_TEXT));

    int tx = bx + bw + TM.pad;
    int max_tw = g_w - bw - TM.pad * 2;
    if (show_fit_btn) {
        int fx, fy, fw, fh;
        fit_btn_rect(&fx, &fy, &fw, &fh);
        touch_draw_button(fx, fy, fw, fh, g_zoom_fit ? "1:1" : "FIT",
                          g_pressed_btn == BTN_FIT,
                          GFX_UI_SURFACE, GFX_UI_SURFACE_3);
        max_tw -= fw + TM.pad * 2;
    }

    hal_gfx_set_clip(tx, g_y, max_tw > 0 ? max_tw : 1, hh);
    hal_gfx_draw_string_scaled(tx, g_y + (hh - FONT_H * TM.scale_small) / 2,
                               title, GFX_UI_TEXT, TM.scale_small);
    hal_gfx_reset_clip();
}

static void render_list(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    draw_header("PNG VIEWER", 0);

    int area_y = g_y + header_h();
    int area_h = g_h - header_h();

    int total_rows = 1 + g_png_count;
    int row_h = TM.touch;
    hal_gfx_set_clip(g_x, area_y, g_w, area_h);
    for (int i = 0; i < total_rows; i++) {
        int ry = area_y + i * row_h - g_list_scroll;
        if (ry + row_h < area_y || ry > area_y + area_h) continue;

        if (g_pressed_row == i)
            hal_gfx_fill_rect(g_x, ry, g_w, row_h, GFX_UI_SURFACE_2);

        const char *name = (i == 0) ? "about.png" : g_png_list[i - 1]->name;
        size_t      fsize = (i == 0) ? g_sample_about_png_len : g_png_list[i - 1]->size;

        int m = TM.touch / 2;
        int mx = g_x + TM.pad * 2;
        int my = ry + (row_h - m) / 2;
        icon_draw(ICON_FILE, mx, my, m,
                  hal_gfx_palette_rgb(i == 0 ? GFX_UI_ACCENT2 : GFX_UI_ACCENT));

        hal_gfx_draw_string_scaled(mx + m + TM.pad * 2,
                                   ry + (row_h - FONT_H * TM.scale_small) / 2,
                                   name, GFX_UI_TEXT, TM.scale_small);

        char buf[16], num[12];
        int pos = touch_strcat(buf, 0, sizeof(buf),
                               touch_itoa((int)fsize, num, 0));
        touch_strcat(buf, pos, sizeof(buf), "B");
        int tw = hal_gfx_string_width(buf, TM.scale_small);
        hal_gfx_draw_string_scaled(g_x + g_w - TM.pad * 2 - tw,
                                   ry + (row_h - FONT_H * TM.scale_small) / 2,
                                   buf, GFX_UI_TEXT_DIM, TM.scale_small);

        hal_gfx_fill_rect(g_x + TM.pad, ry + row_h - 1, g_w - TM.pad * 2, 1,
                          GFX_UI_DIVIDER);
    }
    hal_gfx_reset_clip();
}

static uint32_t blend_rgb(uint32_t bg, uint32_t fg, uint32_t a) {
    uint32_t inv = 255u - a;
    uint32_t r = (((fg >> 16) & 0xFFu) * a + ((bg >> 16) & 0xFFu) * inv) / 255u;
    uint32_t g = (((fg >>  8) & 0xFFu) * a + ((bg >>  8) & 0xFFu) * inv) / 255u;
    uint32_t b = (( fg        & 0xFFu) * a + ( bg        & 0xFFu) * inv) / 255u;
    return (r << 16) | (g << 8) | b;
}

static void render_image_pixels(int vx, int vy, int vw, int vh,
                                int dw, int dh) {
    int dst_x = vx + (vw - dw) / 2 + g_pan_x;
    int dst_y = vy + (vh - dh) / 2 + g_pan_y;

    int x0 = dst_x < vx ? vx : dst_x;
    int y0 = dst_y < vy ? vy : dst_y;
    int x1 = dst_x + dw > vx + vw ? vx + vw : dst_x + dw;
    int y1 = dst_y + dh > vy + vh ? vy + vh : dst_y + dh;
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t *bb = gfxfb_backbuffer32();
    int fb_w = gfxfb_width();
    int iw = (int)g_info.width;
    int ih = (int)g_info.height;

    uint32_t c_dark_bg = hal_gfx_palette_rgb(GFX_UI_BG);
    uint32_t c_chk_a   = GFX_RGB(38, 42, 52);
    uint32_t c_chk_b   = GFX_RGB(56, 62, 74);

    for (int sy = y0; sy < y1; sy++) {
        int iy = (int)((int64_t)(sy - dst_y) * ih / dh);
        if (iy < 0) iy = 0;
        if (iy >= ih) iy = ih - 1;
        const uint32_t *src_row = &g_img_pixels[(size_t)iy * (size_t)iw];
        uint32_t *dst_row = &bb[(size_t)sy * (size_t)fb_w];

        for (int sx = x0; sx < x1; sx++) {
            int ix = (int)((int64_t)(sx - dst_x) * iw / dw);
            if (ix < 0) ix = 0;
            if (ix >= iw) ix = iw - 1;

            uint32_t px = src_row[ix];
            uint32_t a  = px >> 24;
            if (a == 255u) {
                dst_row[sx] = px & 0x00FFFFFFu;
            } else if (a > 0u) {
                uint32_t bg;
                if (g_bg_mode == BG_CHECKER) {
                    int tile = (((sx - dst_x) >> 3) + ((sy - dst_y) >> 3)) & 1;
                    bg = tile ? c_chk_a : c_chk_b;
                } else if (g_bg_mode == BG_LIGHT) {
                    bg = 0x00FFFFFFu;
                } else {
                    bg = c_dark_bg;
                }
                dst_row[sx] = blend_rgb(bg, px, a);
            } else {
                if (g_bg_mode == BG_CHECKER) {
                    int tile = (((sx - dst_x) >> 3) + ((sy - dst_y) >> 3)) & 1;
                    dst_row[sx] = tile ? c_chk_a : c_chk_b;
                } else if (g_bg_mode == BG_LIGHT) {
                    dst_row[sx] = 0x00FFFFFFu;
                }
            }
        }
    }
    gfxfb_mark_dirty(x0, y0, x1 - x0, y1 - y0);
}

static void render_view(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    draw_header(base_name(g_path), g_has_image);

    int vx, vy, vw, vh;
    view_rect(&vx, &vy, &vw, &vh);

    if (!g_has_image) {
        hal_gfx_draw_string_centered(vx, vy + vh / 2 - FONT_H * TM.scale_small / 2,
                                     vw, g_err_msg ? g_err_msg : "NO IMAGE",
                                     GFX_UI_WARN, TM.scale_small);
        return;
    }

    int dw, dh;
    compute_scaled_size(vw, vh, &dw, &dh);
    clamp_pan(vw, vh, dw, dh);

    render_image_pixels(vx, vy, vw, vh, dw, dh);

    /* Нижняя панель управления */
    int by = g_y + g_h - bar_h();
    hal_gfx_fill_rect(g_x, by, g_w, bar_h(), GFX_UI_SURFACE_2);
    hal_gfx_fill_rect(g_x, by, g_w, 1, GFX_UI_DIVIDER);

    int zx0, zy0, zw, zh, ix0, iy0, iw, ih, bx0, by0, bw, bh;
    bar_btn_rects(&zx0, &zy0, &zw, &zh, &ix0, &iy0, &iw, &ih, &bx0, &by0, &bw, &bh);

    touch_draw_button(zx0, zy0, zw, zh, "-", g_pressed_btn == BTN_ZOUT,
                      GFX_UI_SURFACE, GFX_UI_SURFACE_3);
    touch_draw_button(ix0, iy0, iw, ih, "+", g_pressed_btn == BTN_ZIN,
                      GFX_UI_SURFACE, GFX_UI_SURFACE_3);

    const char *bg_lbl = g_bg_mode == BG_CHECKER ? "CHK"
                       : g_bg_mode == BG_LIGHT   ? "WHT" : "DRK";
    touch_draw_button(bx0, by0, bw, bh, bg_lbl, g_pressed_btn == BTN_BG,
                      GFX_UI_SURFACE, GFX_UI_SURFACE_3);

    /* Статус по центру между "+" и "BG": WxH TYPE PCT% */
    char info[48], num[12];
    int p = 0;
    p = touch_strcat(info, p, sizeof(info), touch_itoa((int)g_info.width, num, 0));
    p = touch_strcat(info, p, sizeof(info), "x");
    p = touch_strcat(info, p, sizeof(info), touch_itoa((int)g_info.height, num, 0));
    p = touch_strcat(info, p, sizeof(info), " ");
    p = touch_strcat(info, p, sizeof(info), png_color_name(g_info.color_type));
    if (!g_zoom_fit) {
        p = touch_strcat(info, p, sizeof(info), " ");
        p = touch_strcat(info, p, sizeof(info), touch_itoa(g_zoom_pct, num, 0));
        p = touch_strcat(info, p, sizeof(info), "%");
    }

    int sx = ix0 + iw + TM.pad;
    int sw = bx0 - sx - TM.pad;
    if (sw > 0) {
        hal_gfx_set_clip(sx, by, sw, bar_h());
        hal_gfx_draw_string_centered(sx, by + (bar_h() - FONT_H * TM.scale_small) / 2,
                                     sw, info, GFX_UI_TEXT_DIM, TM.scale_small);
        hal_gfx_reset_clip();
    }
}

/* ---------------- Жизненный цикл и ввод ---------------- */

static void image_init(void) {
    if (g_mode == MODE_LIST) refresh_png_list();
}

static void image_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    if (g_mode == MODE_LIST) refresh_png_list();
}

static void image_render(void) {
    if (g_mode == MODE_VIEW) render_view();
    else                     render_list();
}

static void zoom_step(int dir) {
    static const int steps[] = { 25, 50, 75, 100, 150, 200, 300, 400 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));

    if (g_zoom_fit) {
        int vx, vy, vw, vh, dw, dh;
        view_rect(&vx, &vy, &vw, &vh);
        compute_scaled_size(vw, vh, &dw, &dh);
        int cur_pct = (int)g_info.width > 0 ? dw * 100 / (int)g_info.width : 100;
        g_zoom_fit = 0;
        g_zoom_pct = cur_pct;
    }

    int idx = 3; /* 100% */
    if (dir > 0) {
        for (int i = 0; i < n; i++) {
            if (steps[i] > g_zoom_pct) { idx = i; break; }
            idx = n - 1;
        }
    } else {
        idx = 0;
        for (int i = n - 1; i >= 0; i--) {
            if (steps[i] < g_zoom_pct) { idx = i; break; }
        }
    }
    g_zoom_pct = steps[idx];
}

static void image_on_touch(int type, int x, int y) {
    int hx, hy, hw, hh;
    back_btn_rect(&hx, &hy, &hw, &hh);

    if (g_mode == MODE_LIST) {
        int area_y = g_y + header_h();
        if (type == HAL_EV_POINTER_DOWN) {
            g_drag_y0 = y;
            g_drag_scroll0 = g_list_scroll;
            g_drag_dist = 0;
            g_pressed_row = (y >= area_y) ? (y - area_y + g_list_scroll) / TM.touch : -1;
            return;
        }
        if (type == HAL_EV_POINTER_MOVE) {
            int dy = y - g_drag_y0;
            if (dy < 0) dy = -dy;
            if (dy > g_drag_dist) g_drag_dist = dy;
            if (g_drag_dist > TM.touch / 3) {
                g_pressed_row = -1;
                g_list_scroll = g_drag_scroll0 - (y - g_drag_y0);
                int max_scroll = (1 + g_png_count) * TM.touch - (g_h - header_h());
                if (max_scroll < 0) max_scroll = 0;
                if (g_list_scroll > max_scroll) g_list_scroll = max_scroll;
                if (g_list_scroll < 0) g_list_scroll = 0;
            }
            return;
        }
        if (type != HAL_EV_POINTER_UP) return;
        int row = g_pressed_row;
        g_pressed_row = -1;
        if (g_drag_dist > TM.touch / 3) return;
        if (touch_hit(x, y, hx, hy, hw, hh)) {
            touch_ui_go_home();
            return;
        }
        if (row == 0) {
            g_from_files = 0;
            g_mode = MODE_VIEW;
            decode_png_buf(g_sample_about_png, g_sample_about_png_len, "/about.png");
        } else if (row >= 1 && row <= g_png_count) {
            g_from_files = 0;
            g_mode = MODE_VIEW;
            load_png(g_png_list[row - 1]->path);
        }
        return;
    }

    /* MODE_VIEW */
    int fx, fy, fw, fh;
    fit_btn_rect(&fx, &fy, &fw, &fh);
    int zx0, zy0, zw, zh, ix0, iy0, iw, ih, bx0, by0, bw, bh;
    bar_btn_rects(&zx0, &zy0, &zw, &zh, &ix0, &iy0, &iw, &ih, &bx0, &by0, &bw, &bh);

    if (type == HAL_EV_POINTER_DOWN) {
        g_pressed_btn = -1;
        if (touch_hit(x, y, hx, hy, hw, hh)) g_pressed_btn = BTN_BACK;
        else if (g_has_image && touch_hit(x, y, fx, fy, fw, fh))   g_pressed_btn = BTN_FIT;
        else if (g_has_image && touch_hit(x, y, zx0, zy0, zw, zh)) g_pressed_btn = BTN_ZOUT;
        else if (g_has_image && touch_hit(x, y, ix0, iy0, iw, ih)) g_pressed_btn = BTN_ZIN;
        else if (g_has_image && touch_hit(x, y, bx0, by0, bw, bh)) g_pressed_btn = BTN_BG;
        g_pan_sx = x; g_pan_sy = y;
        g_pan_x0 = g_pan_x; g_pan_y0 = g_pan_y;
        return;
    }

    if (type == HAL_EV_POINTER_MOVE) {
        if (g_pressed_btn < 0 && g_has_image && y >= g_y + header_h() && y < g_y + g_h - bar_h()) {
            int vx, vy, vw, vh, dw, dh;
            view_rect(&vx, &vy, &vw, &vh);
            compute_scaled_size(vw, vh, &dw, &dh);
            g_pan_x = g_pan_x0 + (x - g_pan_sx);
            g_pan_y = g_pan_y0 + (y - g_pan_sy);
            clamp_pan(vw, vh, dw, dh);
        }
        return;
    }

    if (type != HAL_EV_POINTER_UP) return;
    int btn = g_pressed_btn;
    g_pressed_btn = -1;

    if (btn == BTN_BACK && touch_hit(x, y, hx, hy, hw, hh)) {
        if (g_from_files) {
            touch_ui_open("FILES");
        } else {
            g_mode = MODE_LIST;
            refresh_png_list();
        }
    } else if (btn == BTN_FIT && touch_hit(x, y, fx, fy, fw, fh)) {
        g_zoom_fit = !g_zoom_fit;
        if (!g_zoom_fit) g_zoom_pct = 100;
        g_pan_x = 0; g_pan_y = 0;
    } else if (btn == BTN_ZOUT && touch_hit(x, y, zx0, zy0, zw, zh)) {
        zoom_step(-1);
    } else if (btn == BTN_ZIN && touch_hit(x, y, ix0, iy0, iw, ih)) {
        zoom_step(1);
    } else if (btn == BTN_BG && touch_hit(x, y, bx0, by0, bw, bh)) {
        g_bg_mode = (g_bg_mode + 1) % 3;
    }
}

static void image_on_key(int key) {
    if (g_mode != MODE_VIEW || !g_has_image) return;
    if (key == '+' || key == '=') zoom_step(1);
    else if (key == '-')           zoom_step(-1);
    else if (key == 'f' || key == 'F') {
        g_zoom_fit = !g_zoom_fit;
        if (!g_zoom_fit) g_zoom_pct = 100;
        g_pan_x = 0; g_pan_y = 0;
    }
}

static int image_wants_keyboard(void) { return 0; }

const touch_app_t app_image = {
    .name   = "IMAGE",
    .glyph  = "PNG",
    .icon   = 0,
    .color  = GFX_UI_ACCENT2,
    .color2 = GFX_UI_ACCENT2_DARK,
    .init   = image_init,
    .layout = image_layout,
    .render = image_render,
    .on_touch = image_on_touch,
    .on_key = image_on_key,
    .wants_keyboard = image_wants_keyboard
};
