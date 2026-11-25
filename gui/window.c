/* gui/window.c — оконный хром, панель задач и меню "Пуск".
 * Поддерживает два стиля:
 *   - Windows XP (Luna) + скевоморфизм (по умолчанию)
 *   - Windows 11 (Fluent / Mica): скруглённые окна с мягкой тенью,
 *     плоские кнопки управления с hover, полупрозрачная панель задач
 *     с центрированными иконками и часами, плавающее меню "Пуск".
 */
#include "window.h"
#include "gfxfb.h"
#include "icon.h"

#define WIN_RADIUS  3
#define BTN_RADIUS  2

static int g_theme = GUI_THEME_XP_LIGHT;
static int g_mouse_x = -1, g_mouse_y = -1;

static int is_win11(void) {
    return g_theme == GUI_THEME_WIN11_LIGHT || g_theme == GUI_THEME_WIN11_DARK;
}

static int is_win11_dark(void) {
    return g_theme == GUI_THEME_WIN11_DARK;
}

void gui_set_theme(int theme) {
    g_theme = theme;
}

void gui_set_cursor_pos(int x, int y) {
    g_mouse_x = x;
    g_mouse_y = y;
}

int gui_taskbar_height(int screen_h, int theme) {
    if (theme == GUI_THEME_WIN11_LIGHT || theme == GUI_THEME_WIN11_DARK)
        return (screen_h < 400) ? 16 : 22;
    return TASKBAR_H;
}

static int win11_title_h(void) {
    return (hal_gfx_height() < 400) ? 12 : 16;
}

static int win11_pad(void) {
    return (hal_gfx_width() < 400) ? 3 : 4;
}

static int win11_radius(const gui_window_t *win) {
    if (win && win->maximized) return 0;
    return (hal_gfx_width() < 400) ? 5 : 7;
}

static void draw_rounded_outline(int x, int y, int w, int h, uint8_t color, int radius) {
    for (int i = radius; i < w - radius; i++) {
        hal_gfx_put_pixel(x + i, y, color);
        hal_gfx_put_pixel(x + i, y + h - 1, color);
    }
    for (int j = radius; j < h - radius; j++) {
        hal_gfx_put_pixel(x, y + j, color);
        hal_gfx_put_pixel(x + w - 1, y + j, color);
    }
}

void gui_draw_desktop(uint8_t bg_color) {
    hal_gfx_clear(bg_color);
}

/* Единая геометрия кнопок заголовка (0 = close, 1 = max, 2 = min). */
static void win_btn_rect(const gui_window_t *win, int which, int *bx, int *by, int *bw, int *bh) {
    if (is_win11()) {
        int big = (hal_gfx_width() >= 400);
        int w = big ? 18 : 12;
        int h = win11_title_h() - 2;
        int gap = 1;
        int right_margin = win->maximized ? 2 : (big ? 5 : 4);
        *bw = w;
        *bh = h;
        *by = win->y + 1;
        *bx = win->x + win->w - right_margin - (which + 1) * w - which * gap;
    } else {
        *bw = 8;
        *bh = 8;
        *by = win->y + BORDER + 1;
        *bx = win->x + win->w - BORDER - 9 - which * 9;
    }
}

static void draw_win11_caption_glyphs(int bx, int by, int bw, int bh, int which,
                                      int maximized, uint32_t rgb) {
    int cx = bx + bw / 2;
    int cy = by + bh / 2;
    if (which == 2) {
        /* Свернуть: горизонтальная черта */
        hal_gfx_fill_rect_rgb(cx - 3, cy, 6, 1, rgb);
    } else if (which == 1) {
        /* Развернуть / восстановить */
        if (maximized) {
            hal_gfx_fill_rect_rgb(cx - 2, cy - 3, 5, 1, rgb);
            hal_gfx_fill_rect_rgb(cx + 2, cy - 3, 1, 5, rgb);
            hal_gfx_fill_rect_rgb(cx - 3, cy - 1, 5, 1, rgb);
            hal_gfx_fill_rect_rgb(cx - 3, cy + 3, 5, 1, rgb);
            hal_gfx_fill_rect_rgb(cx - 3, cy - 1, 1, 5, rgb);
            hal_gfx_fill_rect_rgb(cx + 1, cy - 1, 1, 5, rgb);
        } else {
            hal_gfx_fill_rect_rgb(cx - 3, cy - 3, 6, 1, rgb);
            hal_gfx_fill_rect_rgb(cx - 3, cy + 2, 6, 1, rgb);
            hal_gfx_fill_rect_rgb(cx - 3, cy - 3, 1, 6, rgb);
            hal_gfx_fill_rect_rgb(cx + 2, cy - 3, 1, 6, rgb);
        }
    } else {
        /* Закрыть: крестик 5x5 */
        for (int i = -2; i <= 2; i++) {
            gfxfb_put_rgb(cx + i, cy + i, rgb);
            gfxfb_put_rgb(cx + i, cy - i, rgb);
        }
    }
}

static void draw_win11_window_chrome(const gui_window_t *win) {
    int dark = is_win11_dark();
    int r = win11_radius(win);
    int th = win11_title_h();

    if (!win->maximized)
        hal_gfx_drop_shadow(win->x, win->y, win->w, win->h, r, 4);

    uint8_t body_idx   = dark ? GFX_W11_DARK_BG   : GFX_W11_LIGHT_BG;
    uint8_t border_idx = dark ? GFX_W11_BORDER_DK : GFX_W11_BORDER_LT;
    uint8_t text_idx   = win->focused
        ? (dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT)
        : (dark ? GFX_W11_DARK_DIM  : GFX_W11_LIGHT_DIM);

    hal_gfx_fill_rounded_rect(win->x, win->y, win->w, win->h, body_idx, r);

    /* Лёгкая подсветка шапки, когда окно в фокусе (эффект Mica-поверхности) */
    if (!win->focused) {
        uint32_t dim_tint = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(255, 255, 255);
        gfxfb_blend_rect_rgb(win->x + 1, win->y + 1, win->w - 2, th, dim_tint, dark ? 6 : 70);
    } else {
        uint32_t top_hi = GFX_RGB(255, 255, 255);
        int rr = r > 1 ? r : 1;
        gfxfb_blend_rect_rgb(win->x + rr, win->y + 1, win->w - 2 * rr, 1, top_hi, dark ? 28 : 170);
    }

    if (win->maximized)
        hal_gfx_draw_rect(win->x, win->y, win->w, win->h, border_idx);
    else
        hal_gfx_draw_rounded_rect(win->x, win->y, win->w, win->h, border_idx, r);

    /* Тонкий разделитель под title bar */
    uint32_t sep_col = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(0, 0, 0);
    gfxfb_blend_rect_rgb(win->x + 2, win->y + th + 1, win->w - 4, 1, sep_col, dark ? 18 : 16);

    /* Иконка приложения и заголовок */
    int pad = win11_pad();
    int icon_sz = (th >= 16) ? 10 : 8;
    int icon_id = (win->title && win->title[0] == 'S') ? ICON_SETTINGS : ICON_TERMINAL;
    uint32_t icon_rgb = hal_gfx_palette_rgb(
        win->focused ? (dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT) : text_idx);
    icon_draw(icon_id, win->x + pad + 1, win->y + 1 + (th - icon_sz) / 2, icon_sz, icon_rgb);

    hal_gfx_draw_string_transparent(win->x + pad + icon_sz + 4,
                                    win->y + 1 + (th - FONT_H) / 2,
                                    win->title, text_idx);

    /* Кнопки управления окном (close / max / min) с hover-подсветкой */
    uint32_t fg_rgb = hal_gfx_palette_rgb(text_idx);
    for (int which = 0; which < 3; which++) {
        int bx, by, bw, bh;
        win_btn_rect(win, which, &bx, &by, &bw, &bh);
        int hover = gui_point_in_rect(g_mouse_x, g_mouse_y, bx, by, bw, bh);

        if (which == 0 && hover) {
            hal_gfx_fill_rounded_rect_rgb(bx, by, bw, bh,
                                          hal_gfx_palette_rgb(GFX_W11_CLOSE_RED), 3, 255);
            draw_win11_caption_glyphs(bx, by, bw, bh, which, win->maximized, GFX_RGB(255, 255, 255));
        } else {
            if (hover) {
                uint32_t hcol = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(0, 0, 0);
                hal_gfx_fill_rounded_rect_rgb(bx, by, bw, bh, hcol, 3, dark ? 28 : 18);
            }
            draw_win11_caption_glyphs(bx, by, bw, bh, which, win->maximized, fg_rgb);
        }
    }

    /* Аккуратный resize grip в правом нижнем углу */
    if (!win->maximized) {
        int gx = win->x + win->w - RESIZE_GRIP;
        int gy = win->y + win->h - RESIZE_GRIP;
        for (int i = 0; i < 3; i++) {
            hal_gfx_put_pixel(gx + RESIZE_GRIP - 2 - i, gy + RESIZE_GRIP - 2, border_idx);
            hal_gfx_put_pixel(gx + RESIZE_GRIP - 2, gy + RESIZE_GRIP - 2 - i, border_idx);
        }
    }
}

void gui_draw_window_chrome(const gui_window_t *win, int dark_theme) {
    if (is_win11()) {
        draw_win11_window_chrome(win);
        return;
    }

    uint8_t body_color = dark_theme ? GFX_DARK_GREY : GFX_XP_BODY;

    hal_gfx_fill_rounded_rect(win->x + 3, win->y + 3, win->w, win->h, GFX_BLACK, WIN_RADIUS);
    hal_gfx_fill_rounded_rect(win->x, win->y, win->w, win->h, body_color, WIN_RADIUS);
    draw_rounded_outline(win->x, win->y, win->w, win->h, GFX_XP_BORDER, WIN_RADIUS);

    for (int i = WIN_RADIUS; i < win->w - WIN_RADIUS; i++)
        hal_gfx_put_pixel(win->x + i, win->y + 1, dark_theme ? GFX_LIGHT_GREY : GFX_WHITE);
    for (int j = WIN_RADIUS; j < win->h - WIN_RADIUS; j++)
        hal_gfx_put_pixel(win->x + 1, win->y + j, dark_theme ? GFX_LIGHT_GREY : GFX_WHITE);

    int tb_x = win->x + BORDER;
    int tb_y = win->y + BORDER;
    int tb_w = win->w - BORDER * 2;

    if (win->focused)
        hal_gfx_dither_gradient_v(tb_x, tb_y, tb_w, TITLE_BAR_H, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK);
    else
        hal_gfx_dither_gradient_v(tb_x, tb_y, tb_w, TITLE_BAR_H, GFX_LIGHT_GREY, GFX_DARK_GREY);

    hal_gfx_draw_string_transparent(tb_x + 3, tb_y + 1, win->title, GFX_WHITE);

    int close_x, max_x, min_x, by, bw, bh;
    win_btn_rect(win, 0, &close_x, &by, &bw, &bh);
    win_btn_rect(win, 1, &max_x,   &by, &bw, &bh);
    win_btn_rect(win, 2, &min_x,   &by, &bw, &bh);

    hal_gfx_draw_glossy_button(close_x, by, 8, 8, GFX_LIGHT_RED, GFX_XP_CLOSE_RED, GFX_BLACK, BTN_RADIUS);
    hal_gfx_draw_char(close_x, by, 'X', GFX_WHITE, GFX_XP_CLOSE_RED);

    hal_gfx_draw_glossy_button(max_x, by, 8, 8, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_draw_rect(max_x + 2, by + 2, 4, 4, GFX_WHITE);

    hal_gfx_draw_glossy_button(min_x, by, 8, 8, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_fill_rect(min_x + 2, by + 5, 4, 1, GFX_WHITE);

    int gx = win->x + win->w - RESIZE_GRIP;
    int gy = win->y + win->h - RESIZE_GRIP;
    for (int i = 0; i < 3; i++) {
        hal_gfx_put_pixel(gx + RESIZE_GRIP - 1 - i, gy + RESIZE_GRIP - 1, GFX_XP_BORDER);
        hal_gfx_put_pixel(gx + RESIZE_GRIP - 1, gy + RESIZE_GRIP - 1 - i, GFX_XP_BORDER);
    }
}

void gui_window_content_rect(const gui_window_t *win, int *cx, int *cy, int *cw, int *ch) {
    if (is_win11()) {
        int pad = win11_pad();
        int th = win11_title_h();
        *cx = win->x + pad;
        *cy = win->y + th + 3;
        *cw = win->w - pad * 2;
        *ch = win->h - th - 3 - pad;
        if (*cw < 0) *cw = 0;
        if (*ch < 0) *ch = 0;
        return;
    }

    *cx = win->x + BORDER;
    *cy = win->y + BORDER + TITLE_BAR_H + 1;
    *cw = win->w - BORDER * 2;
    *ch = win->h - BORDER * 2 - TITLE_BAR_H - 1;
}

int gui_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

int gui_point_in_titlebar(int px, int py, const gui_window_t *win) {
    if (is_win11())
        return gui_point_in_rect(px, py, win->x + 1, win->y + 1, win->w - 2, win11_title_h());
    return gui_point_in_rect(px, py, win->x + BORDER, win->y + BORDER,
                              win->w - BORDER * 2, TITLE_BAR_H);
}

int gui_point_in_close_button(int px, int py, const gui_window_t *win) {
    int bx, by, bw, bh;
    win_btn_rect(win, 0, &bx, &by, &bw, &bh);
    return gui_point_in_rect(px, py, bx, by, bw, bh);
}

int gui_point_in_maximize_button(int px, int py, const gui_window_t *win) {
    int bx, by, bw, bh;
    win_btn_rect(win, 1, &bx, &by, &bw, &bh);
    return gui_point_in_rect(px, py, bx, by, bw, bh);
}

int gui_point_in_minimize_button(int px, int py, const gui_window_t *win) {
    int bx, by, bw, bh;
    win_btn_rect(win, 2, &bx, &by, &bw, &bh);
    return gui_point_in_rect(px, py, bx, by, bw, bh);
}

int gui_point_in_resize_grip(int px, int py, const gui_window_t *win) {
    int gx = win->x + win->w - RESIZE_GRIP;
    int gy = win->y + win->h - RESIZE_GRIP;
    return gui_point_in_rect(px, py, gx, gy, RESIZE_GRIP, RESIZE_GRIP);
}

/* ---------------- Панель задач ---------------- */

static void win11_taskbar_layout(int screen_w, int screen_h, int tb_count,
                                 int *group_x, int *bar_y, int *tb_h,
                                 int *sw, int *bw, int *gap) {
    *tb_h = gui_taskbar_height(screen_h, g_theme);
    *bar_y = screen_h - *tb_h;
    *sw = (screen_h < 400) ? 16 : 22;
    *bw = (screen_w < 400) ? 80 : 96;
    *gap = (screen_w < 400) ? 4 : 6;

    int total_w = *sw;
    if (tb_count > 0)
        total_w += *gap + tb_count * (*bw) + (tb_count - 1) * (*gap);
    *group_x = (screen_w - total_w) / 2;
}

void gui_start_button_rect(int screen_w, int screen_h, int tb_count, int *x, int *y, int *w, int *h) {
    if (is_win11()) {
        int gx, bar_y, tb_h, sw, bw, gap;
        win11_taskbar_layout(screen_w, screen_h, tb_count, &gx, &bar_y, &tb_h, &sw, &bw, &gap);
        *x = gx;
        *y = bar_y + 2;
        *w = sw;
        *h = tb_h - 4;
        return;
    }

    (void)screen_w;
    (void)tb_count;
    *x = 2;
    *y = screen_h - TASKBAR_H + 2;
    *w = 46;
    *h = TASKBAR_H - 4;
}

static void draw_win11_logo(int cx, int cy, int q, int gap, int dark) {
    uint32_t c0 = dark ? GFX_RGB(115, 215, 255) : GFX_RGB(0, 120, 212);
    uint32_t c1 = dark ? GFX_RGB( 92, 198, 252) : GFX_RGB(20, 138, 228);
    uint32_t c2 = dark ? GFX_RGB( 76, 184, 246) : GFX_RGB(0, 103, 192);
    uint32_t c3 = dark ? GFX_RGB( 60, 168, 236) : GFX_RGB(12, 118, 208);
    int total = q * 2 + gap;
    int x0 = cx - total / 2;
    int y0 = cy - total / 2;
    hal_gfx_fill_rect_rgb(x0,           y0,           q, q, c0);
    hal_gfx_fill_rect_rgb(x0 + q + gap, y0,           q, q, c1);
    hal_gfx_fill_rect_rgb(x0,           y0 + q + gap, q, q, c2);
    hal_gfx_fill_rect_rgb(x0 + q + gap, y0 + q + gap, q, q, c3);
}

static void format_hhmm(int hour, int min, char out[6]) {
    if (hour < 0) hour = 0;
    if (hour > 23) hour %= 24;
    if (min < 0) min = 0;
    if (min > 59) min %= 60;
    out[0] = (char)('0' + (hour / 10));
    out[1] = (char)('0' + (hour % 10));
    out[2] = ':';
    out[3] = (char)('0' + (min / 10));
    out[4] = (char)('0' + (min % 10));
    out[5] = '\0';
}

void gui_draw_taskbar(int screen_w, int screen_h, int tb_count, int start_open, int hour, int min) {
    if (is_win11()) {
        int dark = is_win11_dark();
        int tb_h = gui_taskbar_height(screen_h, g_theme);
        int bar_y = screen_h - tb_h;

        uint32_t bar_rgb = hal_gfx_palette_rgb(dark ? GFX_W11_TASKBAR_DK : GFX_W11_TASKBAR_LT);
        gfxfb_blend_rect_rgb(0, bar_y, screen_w, tb_h, bar_rgb, dark ? 226 : 216);

        uint32_t line_rgb = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(160, 175, 195);
        gfxfb_blend_rect_rgb(0, bar_y, screen_w, 1, line_rgb, dark ? 36 : 160);

        int sx, sy, sw, sh;
        gui_start_button_rect(screen_w, screen_h, tb_count, &sx, &sy, &sw, &sh);
        int hover = gui_point_in_rect(g_mouse_x, g_mouse_y, sx, sy, sw, sh);
        if (start_open || hover) {
            uint32_t pill = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(255, 255, 255);
            hal_gfx_fill_rounded_rect_rgb(sx, sy, sw, sh, pill, 3,
                                          start_open ? (dark ? 40 : 210) : (dark ? 24 : 150));
        }

        int q = (screen_h < 400) ? 4 : 5;
        int lgap = (screen_h < 400) ? 1 : 2;
        draw_win11_logo(sx + sw / 2, sy + sh / 2, q, lgap, dark);

        /* Системный трей: часы справа */
        char clk[6];
        format_hhmm(hour, min, clk);
        int clk_x = screen_w - 5 * FONT_W - 6;
        int clk_y = bar_y + (tb_h - FONT_H) / 2;
        hal_gfx_draw_string_transparent(clk_x, clk_y, clk,
                                        dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT);
        return;
    }

    (void)tb_count;
    (void)start_open;
    (void)hour;
    (void)min;
    int bar_y = screen_h - TASKBAR_H;

    hal_gfx_dither_gradient_v(0, bar_y, screen_w, TASKBAR_H, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK);

    int sx, sy, sw, sh;
    gui_start_button_rect(screen_w, screen_h, 0, &sx, &sy, &sw, &sh);
    hal_gfx_draw_glossy_button(sx, sy, sw, sh, GFX_LIGHT_GREEN, GFX_XP_START_GREEN, GFX_BLACK, BTN_RADIUS + 1);
    hal_gfx_draw_string_transparent(sx + 5, sy + (sh - FONT_H) / 2, "START", GFX_WHITE);
}

void gui_taskbar_button_rect(int screen_w, int screen_h, int index, int tb_count,
                             int *x, int *y, int *w, int *h) {
    if (is_win11()) {
        int gx, bar_y, tb_h, sw, bw, gap;
        win11_taskbar_layout(screen_w, screen_h, tb_count, &gx, &bar_y, &tb_h, &sw, &bw, &gap);
        *x = gx + sw + gap + index * (bw + gap);
        *y = bar_y + 2;
        *w = bw;
        *h = tb_h - 4;
        return;
    }

    int sx, sy, sw, sh;
    gui_start_button_rect(screen_w, screen_h, tb_count, &sx, &sy, &sw, &sh);
    (void)screen_w;

    *x = sx + sw + 6 + index * (TASKBAR_BUTTON_W + 4);
    *y = sy;
    *w = TASKBAR_BUTTON_W;
    *h = sh;
}

void gui_draw_taskbar_button(int screen_w, int screen_h, int index, int tb_count,
                             const char *title, int icon_id, int active, int dark_theme) {
    int x, y, w, h;
    gui_taskbar_button_rect(screen_w, screen_h, index, tb_count, &x, &y, &w, &h);

    if (is_win11()) {
        int dark = is_win11_dark();
        int hover = gui_point_in_rect(g_mouse_x, g_mouse_y, x, y, w, h);

        if (active || hover) {
            uint32_t pill = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(255, 255, 255);
            uint32_t alpha = active ? (dark ? 36 : 205) : (dark ? 20 : 130);
            hal_gfx_fill_rounded_rect_rgb(x, y, w, h, pill, 3, alpha);
        } else {
            uint32_t subtle = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(255, 255, 255);
            hal_gfx_fill_rounded_rect_rgb(x, y, w, h, subtle, 3, dark ? 10 : 75);
        }

        int isz = (h >= 16) ? 12 : 9;
        uint32_t irgb = hal_gfx_palette_rgb(dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT);
        icon_draw(icon_id, x + 3, y + (h - isz) / 2, isz, irgb);

        int tx = x + 3 + isz + 3;
        int ty = y + (h - FONT_H) / 2;
        hal_gfx_draw_string_transparent(tx, ty, title,
                                        dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT);

        /* Нижний индикатор окна: длинная синяя/голубая пилюля у активного, короткая точка у свернутого */
        int ind_w = active ? (w >= 90 ? 18 : 14) : 6;
        int ind_h = (h >= 16) ? 2 : 1;
        uint32_t ind_col = active
            ? hal_gfx_palette_rgb(dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT)
            : hal_gfx_palette_rgb(dark ? GFX_W11_DARK_DIM  : GFX_W11_LIGHT_DIM);
        hal_gfx_fill_rect_rgb(x + (w - ind_w) / 2, y + h - ind_h, ind_w, ind_h, ind_col);
        return;
    }

    (void)icon_id;
    uint8_t top = active ? GFX_XP_TITLE_LIGHT : (dark_theme ? GFX_DARK_GREY : GFX_LIGHT_GREY);
    uint8_t bot = active ? GFX_XP_TITLE_DARK  : (dark_theme ? GFX_BLACK     : GFX_DARK_GREY);

    hal_gfx_draw_glossy_button(x, y, w, h, top, bot, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_draw_string_transparent(x + 3, y + (h - FONT_H) / 2, title, GFX_WHITE);
}

/* ---------------- Меню "Пуск" ---------------- */

static void start_menu_geometry(int screen_w, int screen_h,
                                int *mx, int *my, int *mw, int *mh, int *item_h) {
    if (is_win11()) {
        int big = (screen_w >= 400);
        int tb_h = gui_taskbar_height(screen_h, g_theme);
        *mw = big ? 216 : 156;
        *mh = big ? 122 : 88;
        *item_h = big ? 24 : 18;
        *mx = (screen_w - *mw) / 2;
        *my = screen_h - tb_h - (big ? 8 : 4) - *mh;
        return;
    }

    (void)screen_w;
    *item_h = 12;
    *mx = 2;
    *mw = 90;
    *mh = (*item_h) * START_MENU_ITEMS + 4;
    *my = screen_h - TASKBAR_H - (*mh);
}

void gui_draw_start_menu(int screen_w, int screen_h, int dark_theme) {
    int mx, my, mw, mh, item_h;
    start_menu_geometry(screen_w, screen_h, &mx, &my, &mw, &mh, &item_h);

    if (is_win11()) {
        int dark = is_win11_dark();
        int big = (screen_w >= 400);
        int r = big ? 8 : 5;
        int pad = big ? 10 : 6;

        hal_gfx_drop_shadow(mx, my, mw, mh, r, 5);

        uint8_t body_idx   = dark ? GFX_W11_DARK_BG   : GFX_W11_LIGHT_BG;
        uint8_t border_idx = dark ? GFX_W11_BORDER_DK : GFX_W11_BORDER_LT;
        uint8_t text_idx   = dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT;
        uint8_t dim_idx    = dark ? GFX_W11_DARK_DIM  : GFX_W11_LIGHT_DIM;
        uint8_t accent_idx = dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT;

        hal_gfx_fill_rounded_rect(mx, my, mw, mh, body_idx, r);
        hal_gfx_draw_rounded_rect(mx, my, mw, mh, border_idx, r);

        /* Декоративная строка поиска сверху */
        int search_h = big ? 16 : 12;
        int sy = my + pad;
        uint32_t search_bg = dark ? GFX_RGB(20, 22, 28) : GFX_RGB(255, 255, 255);
        hal_gfx_fill_rounded_rect_rgb(mx + pad, sy, mw - pad * 2, search_h, search_bg, search_h / 2, 255);
        hal_gfx_fill_rect_rgb(mx + pad + 4, sy + search_h - 1, mw - pad * 2 - 8, 1,
                              hal_gfx_palette_rgb(accent_idx));
        hal_gfx_draw_string_transparent(mx + pad + 6, sy + (search_h - FONT_H) / 2, "SEARCH", dim_idx);

        /* Заголовок секции */
        int label_y = sy + search_h + (big ? 7 : 4);
        hal_gfx_draw_string_transparent(mx + pad + 2, label_y, "PINNED", dim_idx);

        /* Пункты приложений */
        static const char *const names[START_MENU_ITEMS] = { "TERMINAL", "SETTINGS" };
        static const int icons[START_MENU_ITEMS] = { ICON_TERMINAL, ICON_SETTINGS };
        for (int i = 0; i < START_MENU_ITEMS; i++) {
            int ix, iy, iw, ih;
            gui_start_menu_item_rect(screen_w, screen_h, i, &ix, &iy, &iw, &ih);
            int hover = gui_point_in_rect(g_mouse_x, g_mouse_y, ix, iy, iw, ih);
            if (hover) {
                uint32_t hcol = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(0, 0, 0);
                hal_gfx_fill_rounded_rect_rgb(ix, iy, iw, ih, hcol, 4, dark ? 24 : 16);
            }
            int isz = big ? 14 : 11;
            int box = isz + 4;
            hal_gfx_fill_rounded_rect_rgb(ix + 4, iy + (ih - box) / 2, box, box,
                                          hal_gfx_palette_rgb(accent_idx), 3, dark ? 44 : 28);
            icon_draw(icons[i], ix + 6, iy + (ih - isz) / 2, isz,
                      hal_gfx_palette_rgb(accent_idx));
            hal_gfx_draw_string_transparent(ix + 8 + box + 2, iy + (ih - FONT_H) / 2,
                                            names[i], text_idx);
        }

        /* Нижний футер меню "Пуск" */
        int footer_h = big ? 20 : 14;
        uint32_t fsep = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(0, 0, 0);
        gfxfb_blend_rect_rgb(mx + 4, my + mh - footer_h, mw - 8, 1, fsep, dark ? 24 : 20);
        hal_gfx_draw_string_transparent(mx + pad + 2, my + mh - footer_h + (footer_h - FONT_H) / 2,
                                        "PROSHIVKAOS", dim_idx);
        return;
    }

    (void)screen_w;
    uint8_t body_color = dark_theme ? GFX_DARK_GREY : GFX_XP_BODY;
    uint8_t text_color = dark_theme ? GFX_WHITE : GFX_BLACK;

    hal_gfx_fill_rounded_rect(mx, my, mw, mh, body_color, BTN_RADIUS + 1);
    draw_rounded_outline(mx, my, mw, mh, GFX_XP_BORDER, BTN_RADIUS + 1);

    hal_gfx_draw_string(mx + 3, my + 2, "TERMINAL", text_color, body_color);
    hal_gfx_draw_string(mx + 3, my + 2 + item_h, "SETTINGS", text_color, body_color);
}

void gui_start_menu_item_rect(int screen_w, int screen_h, int index, int *x, int *y, int *w, int *h) {
    int mx, my, mw, mh, item_h;
    start_menu_geometry(screen_w, screen_h, &mx, &my, &mw, &mh, &item_h);

    if (is_win11()) {
        int big = (screen_w >= 400);
        int pad = big ? 10 : 6;
        int search_h = big ? 16 : 12;
        int list_top = my + pad + search_h + (big ? 7 : 4) + FONT_H + (big ? 5 : 3);
        *x = mx + pad;
        *w = mw - pad * 2;
        *h = item_h;
        *y = list_top + index * (item_h + 2);
        return;
    }

    (void)mh;
    *x = mx;
    *w = mw;
    *h = item_h;
    *y = my + 2 + index * item_h;
}
