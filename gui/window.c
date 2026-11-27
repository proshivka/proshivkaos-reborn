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
static int g_scale = 1;
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

void gui_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    g_scale = scale;
}

void gui_set_cursor_pos(int x, int y) {
    g_mouse_x = x;
    g_mouse_y = y;
}

int gui_taskbar_height(int screen_h, int theme) {
    if (theme == GUI_THEME_WIN11_LIGHT || theme == GUI_THEME_WIN11_DARK)
        return ((screen_h < 400) ? 16 : 22) * g_scale;
    return TASKBAR_H * g_scale;
}

static int xp_title_h(void) {
    return TITLE_BAR_H * g_scale;
}

static int xp_border(void) {
    return BORDER + (g_scale - 1);
}

static int resize_grip_sz(void) {
    return RESIZE_GRIP * g_scale;
}

static int win11_title_h(void) {
    return ((hal_gfx_height() < 400) ? 12 : 16) * g_scale;
}

static int win11_pad(void) {
    return ((hal_gfx_width() < 400) ? 3 : 4) * g_scale;
}

static int win11_radius(const gui_window_t *win) {
    if (win && win->maximized) return 0;
    return ((hal_gfx_width() < 400) ? 5 : 7) * g_scale;
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
        int w = (big ? 18 : 12) * g_scale;
        int h = win11_title_h() - 2 * g_scale;
        int gap = g_scale;
        int right_margin = win->maximized ? 2 * g_scale : (big ? 5 : 4) * g_scale;
        *bw = w;
        *bh = h;
        *by = win->y + g_scale;
        *bx = win->x + win->w - right_margin - (which + 1) * w - which * gap;
    } else {
        int b = xp_border();
        int sz = 8 * g_scale;
        *bw = sz;
        *bh = sz;
        *by = win->y + b + g_scale;
        *bx = win->x + win->w - b - (sz + g_scale) - which * (sz + g_scale);
    }
}

static void draw_win11_caption_glyphs(int bx, int by, int bw, int bh, int which,
                                      int maximized, uint32_t rgb) {
    int cx = bx + bw / 2;
    int cy = by + bh / 2;
    int s = g_scale;
    if (which == 2) {
        /* Свернуть: горизонтальная черта */
        hal_gfx_fill_rect_rgb(cx - 3 * s, cy, 6 * s, s, rgb);
    } else if (which == 1) {
        /* Развернуть / восстановить */
        if (maximized) {
            hal_gfx_fill_rect_rgb(cx - 2 * s, cy - 3 * s, 5 * s, s, rgb);
            hal_gfx_fill_rect_rgb(cx + 2 * s, cy - 3 * s, s, 5 * s, rgb);
            hal_gfx_fill_rect_rgb(cx - 3 * s, cy - 1 * s, 5 * s, s, rgb);
            hal_gfx_fill_rect_rgb(cx - 3 * s, cy + 3 * s, 5 * s, s, rgb);
            hal_gfx_fill_rect_rgb(cx - 3 * s, cy - 1 * s, s, 5 * s, rgb);
            hal_gfx_fill_rect_rgb(cx + 1 * s, cy - 1 * s, s, 5 * s, rgb);
        } else {
            hal_gfx_fill_rect_rgb(cx - 3 * s, cy - 3 * s, 6 * s, s, rgb);
            hal_gfx_fill_rect_rgb(cx - 3 * s, cy + 2 * s, 6 * s, s, rgb);
            hal_gfx_fill_rect_rgb(cx - 3 * s, cy - 3 * s, s, 6 * s, rgb);
            hal_gfx_fill_rect_rgb(cx + 2 * s, cy - 3 * s, s, 6 * s, rgb);
        }
    } else {
        /* Закрыть: крестик */
        for (int i = -2 * s; i <= 2 * s; i++) {
            hal_gfx_fill_rect_rgb(cx + i, cy + i, s, s, rgb);
            hal_gfx_fill_rect_rgb(cx + i, cy - i, s, s, rgb);
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
    int icon_sz = ((th >= 16 * g_scale) ? 10 : 8) * g_scale;
    int icon_id = ICON_TERMINAL;
    if (win->title && (unsigned char)win->title[0] == 0xD0) {
        uint8_t b1 = (unsigned char)win->title[1];
        if (b1 == 0x9D) icon_id = ICON_SETTINGS;      /* Н - Настройки */
        else if (b1 == 0x9F) icon_id = ICON_FOLDER;   /* П - Проводник */
        else if (b1 == 0x98) icon_id = ICON_PUBLIC;   /* И - Изображения */
    } else if (win->title && win->title[0] == 'S') {
        icon_id = ICON_SETTINGS;
    }
    uint32_t icon_rgb = hal_gfx_palette_rgb(
        win->focused ? (dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT) : text_idx);
    icon_draw(icon_id, win->x + pad + 1, win->y + 1 + (th - icon_sz) / 2, icon_sz, icon_rgb);

    hal_gfx_draw_string_scaled(win->x + pad + icon_sz + 4 * g_scale,
                               win->y + 1 + (th - FONT_H * g_scale) / 2,
                               win->title, text_idx, g_scale);

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
        int rg = resize_grip_sz();
        int gx = win->x + win->w - rg;
        int gy = win->y + win->h - rg;
        for (int i = 0; i < 3 * g_scale; i++) {
            hal_gfx_put_pixel(gx + rg - 2 - i, gy + rg - 2, border_idx);
            hal_gfx_put_pixel(gx + rg - 2, gy + rg - 2 - i, border_idx);
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

    int b = xp_border();
    int th = xp_title_h();
    int tb_x = win->x + b;
    int tb_y = win->y + b;
    int tb_w = win->w - b * 2;

    if (win->focused)
        hal_gfx_dither_gradient_v(tb_x, tb_y, tb_w, th, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK);
    else
        hal_gfx_dither_gradient_v(tb_x, tb_y, tb_w, th, GFX_LIGHT_GREY, GFX_DARK_GREY);

    hal_gfx_draw_string_scaled(tb_x + 3 * g_scale, tb_y + (th - FONT_H * g_scale) / 2,
                               win->title, GFX_WHITE, g_scale);

    int close_x, max_x, min_x, by, bw, bh;
    win_btn_rect(win, 0, &close_x, &by, &bw, &bh);
    win_btn_rect(win, 1, &max_x,   &by, &bw, &bh);
    win_btn_rect(win, 2, &min_x,   &by, &bw, &bh);

    hal_gfx_draw_glossy_button(close_x, by, bw, bh, GFX_LIGHT_RED, GFX_XP_CLOSE_RED, GFX_BLACK, BTN_RADIUS);
    hal_gfx_draw_string_centered(close_x, by + (bh - FONT_H * g_scale) / 2, bw, "X", GFX_WHITE, g_scale);

    hal_gfx_draw_glossy_button(max_x, by, bw, bh, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_draw_rect(max_x + 2 * g_scale, by + 2 * g_scale, bw - 4 * g_scale, bh - 4 * g_scale, GFX_WHITE);

    hal_gfx_draw_glossy_button(min_x, by, bw, bh, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_fill_rect(min_x + 2 * g_scale, by + bh - 3 * g_scale, bw - 4 * g_scale, g_scale, GFX_WHITE);

    int rg = resize_grip_sz();
    int gx = win->x + win->w - rg;
    int gy = win->y + win->h - rg;
    for (int i = 0; i < 3 * g_scale; i++) {
        hal_gfx_put_pixel(gx + rg - 1 - i, gy + rg - 1, GFX_XP_BORDER);
        hal_gfx_put_pixel(gx + rg - 1, gy + rg - 1 - i, GFX_XP_BORDER);
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

    int b = xp_border();
    int th = xp_title_h();
    *cx = win->x + b;
    *cy = win->y + b + th + 1;
    *cw = win->w - b * 2;
    *ch = win->h - b * 2 - th - 1;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

int gui_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

int gui_point_in_titlebar(int px, int py, const gui_window_t *win) {
    if (is_win11())
        return gui_point_in_rect(px, py, win->x + 1, win->y + 1, win->w - 2, win11_title_h());
    int b = xp_border();
    return gui_point_in_rect(px, py, win->x + b, win->y + b,
                              win->w - b * 2, xp_title_h());
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
    int rg = resize_grip_sz();
    int gx = win->x + win->w - rg;
    int gy = win->y + win->h - rg;
    return gui_point_in_rect(px, py, gx, gy, rg, rg);
}

/* ---------------- Панель задач ---------------- */

static void win11_taskbar_layout(int screen_w, int screen_h, int tb_count,
                                 int *group_x, int *bar_y, int *tb_h,
                                 int *sw, int *bw, int *gap) {
    *tb_h = gui_taskbar_height(screen_h, g_theme);
    *bar_y = screen_h - *tb_h;
    *sw = ((screen_h < 400) ? 16 : 22) * g_scale;
    *bw = ((screen_w < 400) ? 80 : 96) * g_scale;
    *gap = ((screen_w < 400) ? 4 : 6) * g_scale;

    int total_w = *sw;
    if (tb_count > 0)
        total_w += *gap + tb_count * (*bw) + (tb_count - 1) * (*gap);
    int max_w = screen_w - 60 * g_scale;
    if (tb_count > 0 && total_w > max_w && max_w > *sw) {
        *bw = (max_w - *sw - tb_count * (*gap)) / tb_count;
        if (*bw < 24) *bw = 24;
        total_w = *sw + *gap + tb_count * (*bw) + (tb_count - 1) * (*gap);
    }
    *group_x = (screen_w - total_w) / 2;
}

void gui_start_button_rect(int screen_w, int screen_h, int tb_count, int *x, int *y, int *w, int *h) {
    if (is_win11()) {
        int gx, bar_y, tb_h, sw, bw, gap;
        win11_taskbar_layout(screen_w, screen_h, tb_count, &gx, &bar_y, &tb_h, &sw, &bw, &gap);
        *x = gx;
        *y = bar_y + 2 * g_scale;
        *w = sw;
        *h = tb_h - 4 * g_scale;
        return;
    }

    (void)screen_w;
    (void)tb_count;
    int tb_h = TASKBAR_H * g_scale;
    *x = 2 * g_scale;
    *y = screen_h - tb_h + 2 * g_scale;
    *w = 46 * g_scale;
    *h = tb_h - 4 * g_scale;
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

void gui_clock_rect(int screen_w, int screen_h, int *x, int *y, int *w, int *h) {
    int tb_h = gui_taskbar_height(screen_h, g_theme);
    int bar_y = screen_h - tb_h;
    int cw = hal_gfx_string_width("00:00", g_scale) + 8 * g_scale;
    *w = cw;
    *h = tb_h;
    *x = screen_w - cw - 2 * g_scale;
    *y = bar_y;
}

void gui_draw_taskbar(int screen_w, int screen_h, int tb_count, int start_open,
                      int hour, int min, int ru_layout) {
    char clk[6];
    format_hhmm(hour, min, clk);
    const char *lang = ru_layout ? "RU" : "EN";

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

        int q = ((screen_h < 400) ? 4 : 5) * g_scale;
        int lgap = ((screen_h < 400) ? 1 : 2) * g_scale;
        draw_win11_logo(sx + sw / 2, sy + sh / 2, q, lgap, dark);

        /* Системный трей: часы и раскладка справа */
        uint8_t txt_idx = dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT;
        int clk_w = hal_gfx_string_width(clk, g_scale);
        int clk_x = screen_w - clk_w - 6 * g_scale;
        int ty = bar_y + (tb_h - FONT_H * g_scale) / 2;
        hal_gfx_draw_string_scaled(clk_x, ty, clk, txt_idx, g_scale);

        int lang_w = hal_gfx_string_width(lang, g_scale);
        int lang_x = clk_x - lang_w - 8 * g_scale;
        hal_gfx_draw_string_scaled(lang_x, ty, lang, txt_idx, g_scale);
        return;
    }

    (void)tb_count;
    (void)start_open;
    int tb_h = TASKBAR_H * g_scale;
    int bar_y = screen_h - tb_h;

    hal_gfx_dither_gradient_v(0, bar_y, screen_w, tb_h, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK);

    int sx, sy, sw, sh;
    gui_start_button_rect(screen_w, screen_h, 0, &sx, &sy, &sw, &sh);
    hal_gfx_draw_glossy_button(sx, sy, sw, sh, GFX_LIGHT_GREEN, GFX_XP_START_GREEN, GFX_BLACK, BTN_RADIUS + 1);
    hal_gfx_draw_string_centered(sx, sy + (sh - FONT_H * g_scale) / 2, sw, "Пуск", GFX_WHITE, g_scale);

    /* Трей справа: раскладка и часы */
    int clk_w = hal_gfx_string_width(clk, g_scale);
    int clk_x = screen_w - clk_w - 6 * g_scale;
    int ty = bar_y + (tb_h - FONT_H * g_scale) / 2;
    hal_gfx_draw_string_scaled(clk_x, ty, clk, GFX_WHITE, g_scale);

    int lang_w = hal_gfx_string_width(lang, g_scale);
    int lang_x = clk_x - lang_w - 7 * g_scale;
    hal_gfx_draw_string_scaled(lang_x, ty, lang, GFX_WHITE, g_scale);
}

void gui_taskbar_button_rect(int screen_w, int screen_h, int index, int tb_count,
                             int *x, int *y, int *w, int *h) {
    if (is_win11()) {
        int gx, bar_y, tb_h, sw, bw, gap;
        win11_taskbar_layout(screen_w, screen_h, tb_count, &gx, &bar_y, &tb_h, &sw, &bw, &gap);
        *x = gx + sw + gap + index * (bw + gap);
        *y = bar_y + 2 * g_scale;
        *w = bw;
        *h = tb_h - 4 * g_scale;
        return;
    }

    int sx, sy, sw, sh;
    gui_start_button_rect(screen_w, screen_h, tb_count, &sx, &sy, &sw, &sh);
    int bw = TASKBAR_BUTTON_W * g_scale;
    int gap = 4 * g_scale;
    if (tb_count > 0) {
        int avail = screen_w - (sx + sw + 6 * g_scale) - 54 * g_scale;
        int max_bw = (avail - (tb_count - 1) * gap) / tb_count;
        if (max_bw < bw && max_bw >= 20) bw = max_bw;
    }

    *x = sx + sw + 6 * g_scale + index * (bw + gap);
    *y = sy;
    *w = bw;
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
            hal_gfx_fill_rounded_rect_rgb(x, y, w, h, pill, 3 * g_scale, alpha);
        } else {
            uint32_t subtle = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(255, 255, 255);
            hal_gfx_fill_rounded_rect_rgb(x, y, w, h, subtle, 3 * g_scale, dark ? 10 : 75);
        }

        int isz = ((h >= 16 * g_scale) ? 12 : 9) * g_scale;
        uint32_t irgb = hal_gfx_palette_rgb(dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT);
        icon_draw(icon_id, x + 3 * g_scale, y + (h - isz) / 2, isz, irgb);

        int tx = x + 3 * g_scale + isz + 3 * g_scale;
        int ty = y + (h - FONT_H * g_scale) / 2;
        hal_gfx_draw_string_scaled(tx, ty, title,
                                   dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT, g_scale);

        /* Нижний индикатор окна: длинная синяя/голубая пилюля у активного, короткая точка у свернутого */
        int ind_w = (active ? (w >= 90 * g_scale ? 18 : 14) : 6) * g_scale;
        int ind_h = ((h >= 16 * g_scale) ? 2 : 1) * g_scale;
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
    hal_gfx_draw_string_scaled(x + 3 * g_scale, y + (h - FONT_H * g_scale) / 2, title, GFX_WHITE, g_scale);
}

/* ---------------- Меню "Пуск" ---------------- */

static void start_menu_geometry(int screen_w, int screen_h,
                                int *mx, int *my, int *mw, int *mh, int *item_h) {
    if (is_win11()) {
        int big = (screen_w >= 400);
        int tb_h = gui_taskbar_height(screen_h, g_theme);
        *mw = (big ? 216 : 156) * g_scale;
        if (*mw > screen_w - 8) *mw = screen_w - 8;
        *mh = (big ? 174 : 126) * g_scale;
        if (*mh > screen_h - tb_h - 4) *mh = screen_h - tb_h - 4;
        *item_h = (big ? 24 : 18) * g_scale;
        *mx = (screen_w - *mw) / 2;
        *my = screen_h - tb_h - (big ? 8 : 4) * g_scale - *mh;
        if (*my < 2) *my = 2;
        return;
    }

    (void)screen_w;
    *item_h = 12 * g_scale;
    *mx = 2 * g_scale;
    *mw = 90 * g_scale;
    if (*mw > screen_w - 4) *mw = screen_w - 4;
    *mh = (*item_h) * START_MENU_ITEMS + 4 * g_scale;
    *my = screen_h - TASKBAR_H * g_scale - (*mh);
    if (*my < 2) *my = 2;
}

void gui_draw_start_menu(int screen_w, int screen_h, int dark_theme) {
    int mx, my, mw, mh, item_h;
    start_menu_geometry(screen_w, screen_h, &mx, &my, &mw, &mh, &item_h);

    if (is_win11()) {
        int dark = is_win11_dark();
        int big = (screen_w >= 400);
        int r = (big ? 8 : 5) * g_scale;
        int pad = (big ? 10 : 6) * g_scale;

        hal_gfx_drop_shadow(mx, my, mw, mh, r, 5);

        uint8_t body_idx   = dark ? GFX_W11_DARK_BG   : GFX_W11_LIGHT_BG;
        uint8_t border_idx = dark ? GFX_W11_BORDER_DK : GFX_W11_BORDER_LT;
        uint8_t text_idx   = dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT;
        uint8_t dim_idx    = dark ? GFX_W11_DARK_DIM  : GFX_W11_LIGHT_DIM;
        uint8_t accent_idx = dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT;

        hal_gfx_fill_rounded_rect(mx, my, mw, mh, body_idx, r);
        hal_gfx_draw_rounded_rect(mx, my, mw, mh, border_idx, r);

        /* Декоративная строка поиска сверху */
        int search_h = (big ? 16 : 12) * g_scale;
        int sy = my + pad;
        uint32_t search_bg = dark ? GFX_RGB(20, 22, 28) : GFX_RGB(255, 255, 255);
        hal_gfx_fill_rounded_rect_rgb(mx + pad, sy, mw - pad * 2, search_h, search_bg, search_h / 2, 255);
        hal_gfx_fill_rect_rgb(mx + pad + 4 * g_scale, sy + search_h - 1, mw - pad * 2 - 8 * g_scale, 1,
                              hal_gfx_palette_rgb(accent_idx));
        hal_gfx_draw_string_scaled(mx + pad + 6 * g_scale, sy + (search_h - FONT_H * g_scale) / 2,
                                   "Поиск", dim_idx, g_scale);

        /* Заголовок секции */
        int label_y = sy + search_h + (big ? 7 : 4) * g_scale;
        hal_gfx_draw_string_scaled(mx + pad + 2 * g_scale, label_y, "Закреплено", dim_idx, g_scale);

        /* Пункты приложений */
        static const char *const names[START_MENU_ITEMS] = {
            "Терминал", "Проводник", "Изображения", "Настройки"
        };
        static const int icons[START_MENU_ITEMS] = {
            ICON_TERMINAL, ICON_FOLDER, ICON_PUBLIC, ICON_SETTINGS
        };
        for (int i = 0; i < START_MENU_ITEMS; i++) {
            int ix, iy, iw, ih;
            gui_start_menu_item_rect(screen_w, screen_h, i, &ix, &iy, &iw, &ih);
            int hover = gui_point_in_rect(g_mouse_x, g_mouse_y, ix, iy, iw, ih);
            if (hover) {
                uint32_t hcol = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(0, 0, 0);
                hal_gfx_fill_rounded_rect_rgb(ix, iy, iw, ih, hcol, 4 * g_scale, dark ? 24 : 16);
            }
            int isz = (big ? 14 : 11) * g_scale;
            int box = isz + 4 * g_scale;
            hal_gfx_fill_rounded_rect_rgb(ix + 4 * g_scale, iy + (ih - box) / 2, box, box,
                                          hal_gfx_palette_rgb(accent_idx), 3 * g_scale, dark ? 44 : 28);
            icon_draw(icons[i], ix + 6 * g_scale, iy + (ih - isz) / 2, isz,
                      hal_gfx_palette_rgb(accent_idx));
            hal_gfx_draw_string_scaled(ix + 8 * g_scale + box + 2 * g_scale,
                                       iy + (ih - FONT_H * g_scale) / 2,
                                       names[i], text_idx, g_scale);
        }

        /* Нижний футер меню "Пуск" */
        int footer_h = (big ? 20 : 14) * g_scale;
        uint32_t fsep = dark ? GFX_RGB(255, 255, 255) : GFX_RGB(0, 0, 0);
        gfxfb_blend_rect_rgb(mx + 4 * g_scale, my + mh - footer_h, mw - 8 * g_scale, 1, fsep, dark ? 24 : 20);
        hal_gfx_draw_string_scaled(mx + pad + 2 * g_scale,
                                   my + mh - footer_h + (footer_h - FONT_H * g_scale) / 2,
                                   "PROSHIVKAOS", dim_idx, g_scale);
        return;
    }

    (void)screen_w;
    uint8_t body_color = dark_theme ? GFX_DARK_GREY : GFX_XP_BODY;
    uint8_t text_color = dark_theme ? GFX_WHITE : GFX_BLACK;

    hal_gfx_fill_rounded_rect(mx, my, mw, mh, body_color, BTN_RADIUS + 1);
    draw_rounded_outline(mx, my, mw, mh, GFX_XP_BORDER, BTN_RADIUS + 1);

    static const char *const xp_names[START_MENU_ITEMS] = {
        "Терминал", "Проводник", "Изображения", "Настройки"
    };
    for (int i = 0; i < START_MENU_ITEMS; i++) {
        hal_gfx_draw_string_scaled(mx + 5 * g_scale,
                                   my + 2 * g_scale + i * item_h + (item_h - FONT_H * g_scale) / 2,
                                   xp_names[i], text_color, g_scale);
    }
}

void gui_start_menu_item_rect(int screen_w, int screen_h, int index, int *x, int *y, int *w, int *h) {
    int mx, my, mw, mh, item_h;
    start_menu_geometry(screen_w, screen_h, &mx, &my, &mw, &mh, &item_h);

    if (is_win11()) {
        int big = (screen_w >= 400);
        int pad = (big ? 10 : 6) * g_scale;
        int search_h = (big ? 16 : 12) * g_scale;
        int list_top = my + pad + search_h + (big ? 7 : 4) * g_scale + FONT_H * g_scale + (big ? 5 : 3) * g_scale;
        *x = mx + pad;
        *w = mw - pad * 2;
        *h = item_h;
        *y = list_top + index * (item_h + 2 * g_scale);
        return;
    }

    (void)mh;
    *x = mx;
    *w = mw;
    *h = item_h;
    *y = my + 2 * g_scale + index * item_h;
}

/* ---------------- Календарь ---------------- */

static int cal_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int cal_days_in_month(int y, int m) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    if (m == 2 && cal_is_leap(y)) return 29;
    return dm[m - 1];
}

/* День недели 0=Пн .. 6=Вс для (y, m, 1). */
static int cal_first_weekday_mon(int y, int m) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y;
    if (m < 3) yy--;
    int w0 = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + 1) % 7; /* 0=Вс..6=Сб */
    return (w0 + 6) % 7;
}

static void calendar_geometry(int screen_w, int screen_h,
                              int *cx, int *cy, int *cw, int *ch) {
    int tb_h = gui_taskbar_height(screen_h, g_theme);
    *cw = 132 * g_scale;
    *ch = 112 * g_scale;
    if (*cw > screen_w - 4) *cw = screen_w - 4;
    if (*ch > screen_h - tb_h - 4) *ch = screen_h - tb_h - 4;
    *cx = screen_w - *cw - 4 * g_scale;
    *cy = screen_h - tb_h - *ch - 4 * g_scale;
    if (*cx < 2) *cx = 2;
    if (*cy < 2) *cy = 2;
}

static void fmt_month_year(int year, int month, char *out, int cap) {
    static const char *const months[12] = {
        "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
        "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"
    };
    const char *mn = (month >= 1 && month <= 12) ? months[month - 1] : "?";
    int p = 0;
    while (*mn && p < cap - 1) out[p++] = *mn++;
    if (p < cap - 1) out[p++] = ' ';
    char tmp[8];
    int n = 0, y = year < 0 ? 0 : year;
    do { tmp[n++] = (char)('0' + y % 10); y /= 10; } while (y && n < 8);
    while (n > 0 && p < cap - 1) out[p++] = tmp[--n];
    out[p] = '\0';
}

void gui_draw_calendar(int screen_w, int screen_h, int year, int month,
                       int today_y, int today_m, int today_d, int dark_theme) {
    int cx, cy, cw, ch;
    calendar_geometry(screen_w, screen_h, &cx, &cy, &cw, &ch);

    uint8_t body_idx, border_idx, text_idx, dim_idx, accent_idx;
    if (is_win11()) {
        int dark = is_win11_dark();
        body_idx   = dark ? GFX_W11_DARK_BG   : GFX_W11_LIGHT_BG;
        border_idx = dark ? GFX_W11_BORDER_DK : GFX_W11_BORDER_LT;
        text_idx   = dark ? GFX_W11_DARK_TEXT : GFX_W11_LIGHT_TEXT;
        dim_idx    = dark ? GFX_W11_DARK_DIM  : GFX_W11_LIGHT_DIM;
        accent_idx = dark ? GFX_W11_ACCENT_LT : GFX_W11_ACCENT;
        hal_gfx_drop_shadow(cx, cy, cw, ch, 6 * g_scale, 4);
        hal_gfx_fill_rounded_rect(cx, cy, cw, ch, body_idx, 6 * g_scale);
        hal_gfx_draw_rounded_rect(cx, cy, cw, ch, border_idx, 6 * g_scale);
    } else {
        body_idx   = dark_theme ? GFX_DARK_GREY : GFX_XP_BODY;
        border_idx = GFX_XP_BORDER;
        text_idx   = dark_theme ? GFX_WHITE : GFX_BLACK;
        dim_idx    = dark_theme ? GFX_LIGHT_GREY : GFX_DARK_GREY;
        accent_idx = GFX_XP_TITLE_LIGHT;
        hal_gfx_fill_rounded_rect(cx + 2, cy + 2, cw, ch, GFX_BLACK, 3);
        hal_gfx_fill_rounded_rect(cx, cy, cw, ch, body_idx, 3);
        draw_rounded_outline(cx, cy, cw, ch, border_idx, 3);
    }

    int pad = 6 * g_scale;
    int hdr_y = cy + pad;
    int hdr_h = 14 * g_scale;

    char title[32];
    fmt_month_year(year, month, title, sizeof(title));
    hal_gfx_draw_string_scaled(cx + pad, hdr_y + (hdr_h - FONT_H * g_scale) / 2,
                               title, text_idx, g_scale);

    int btn_w = 14 * g_scale;
    int prev_x = cx + cw - pad - btn_w * 2 - 2 * g_scale;
    int next_x = cx + cw - pad - btn_w;
    hal_gfx_draw_string_centered(prev_x, hdr_y + (hdr_h - FONT_H * g_scale) / 2,
                                 btn_w, "<", text_idx, g_scale);
    hal_gfx_draw_string_centered(next_x, hdr_y + (hdr_h - FONT_H * g_scale) / 2,
                                 btn_w, ">", text_idx, g_scale);

    int grid_x = cx + pad;
    int grid_w = cw - pad * 2;
    int col_w = grid_w / 7;
    int wd_y = hdr_y + hdr_h + 2 * g_scale;

    static const char *const wdays[7] = { "Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс" };
    for (int c = 0; c < 7; c++) {
        uint8_t wc = (c >= 5) ? dim_idx : text_idx;
        hal_gfx_draw_string_centered(grid_x + c * col_w, wd_y, col_w, wdays[c], wc, g_scale);
    }

    int days_y = wd_y + FONT_H * g_scale + 3 * g_scale;
    int row_h = (cy + ch - pad - days_y) / 6;
    if (row_h < FONT_H * g_scale) row_h = FONT_H * g_scale;

    int first_wd = cal_first_weekday_mon(year, month);
    int n_days = cal_days_in_month(year, month);

    for (int d = 1; d <= n_days; d++) {
        int slot = first_wd + d - 1;
        int r = slot / 7;
        int c = slot % 7;
        if (r >= 6) break;

        int cell_x = grid_x + c * col_w;
        int cell_y = days_y + r * row_h;
        int is_today = (year == today_y && month == today_m && d == today_d);

        if (is_today) {
            hal_gfx_fill_rounded_rect_rgb(cell_x + 1, cell_y, col_w - 2, row_h - 1,
                                          hal_gfx_palette_rgb(accent_idx), 2 * g_scale, 255);
        }

        char dbuf[4];
        if (d >= 10) {
            dbuf[0] = (char)('0' + d / 10);
            dbuf[1] = (char)('0' + d % 10);
            dbuf[2] = '\0';
        } else {
            dbuf[0] = (char)('0' + d);
            dbuf[1] = '\0';
        }
        uint8_t dc = is_today ? GFX_WHITE : ((c >= 5) ? dim_idx : text_idx);
        hal_gfx_draw_string_centered(cell_x, cell_y + (row_h - FONT_H * g_scale) / 2,
                                     col_w, dbuf, dc, g_scale);
    }
}

int gui_calendar_handle_click(int screen_w, int screen_h, int mx, int my,
                              int *year, int *month) {
    int cx, cy, cw, ch;
    calendar_geometry(screen_w, screen_h, &cx, &cy, &cw, &ch);
    if (!gui_point_in_rect(mx, my, cx, cy, cw, ch)) return 0;

    int pad = 6 * g_scale;
    int hdr_y = cy + pad;
    int hdr_h = 14 * g_scale;
    int btn_w = 14 * g_scale;
    int prev_x = cx + cw - pad - btn_w * 2 - 2 * g_scale;
    int next_x = cx + cw - pad - btn_w;

    if (gui_point_in_rect(mx, my, prev_x, hdr_y, btn_w, hdr_h)) {
        (*month)--;
        if (*month < 1) { *month = 12; (*year)--; }
    } else if (gui_point_in_rect(mx, my, next_x, hdr_y, btn_w, hdr_h)) {
        (*month)++;
        if (*month > 12) { *month = 1; (*year)++; }
    }
    return 1;
}
