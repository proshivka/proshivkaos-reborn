/* gui/image_app.c — оконный просмотрщик изображений (PNG и GIF).
 * Поддерживает файлы из RamFS и встроенные образцы (about.png, ineffa.gif, dr.gif),
 * анимацию GIF, переключение режима Авто / 1:1 и фона (шашечка / тёмный / светлый).
 */
#include "image_app.h"
#include "gfxfb.h"
#include "icon.h"
#include "png.h"
#include "gif.h"
#include "hal_time.h"
#include "hal.h"

#define MODE_LIST 0
#define MODE_VIEW 1

#define BG_CHECKER 0
#define BG_DARK    1
#define BG_LIGHT   2

#define KIND_PNG 1
#define KIND_GIF 2

extern const uint8_t g_sample_about_png[];
extern const size_t  g_sample_about_png_len;
extern const uint8_t g_sample_ineffa_gif[];
extern const size_t  g_sample_ineffa_gif_len;

static uint8_t       g_file_buf[RAMFS_MAX_BIG_SIZE];
static uint32_t      g_img_pixels[PNG_MAX_WIDTH * PNG_MAX_HEIGHT];
static uint32_t      g_gif_restore[GIF_MAX_PIXELS];
static gif_decoder_t g_gif;
static gif_info_t    g_gif_info;

static const ramfs_file_t *g_list[RAMFS_MAX_ENTRIES];
static int g_list_count = 0;

static char        g_cur_path[RAMFS_MAX_PATH_LEN];
static uint32_t    g_img_w = 0, g_img_h = 0;
static int         g_kind = KIND_PNG;
static int         g_has_image = 0;
static int         g_gif_playing = 0;
static uint64_t    g_gif_next_ms = 0;
static const char *g_err_msg = 0;
static int         g_zoom_fit = 1;
static int         g_bg_mode = BG_CHECKER;

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int ends_with_ci(const char *name, const char *ext) {
    int n = 0, e = 0;
    while (name[n]) n++;
    while (ext[e]) e++;
    if (n < e) return 0;
    for (int i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

static void collect_dir(const char *dir) {
    const ramfs_file_t *entries[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(dir, entries, RAMFS_MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        if (entries[i]->is_dir) {
            collect_dir(entries[i]->path);
        } else if (ends_with_ci(entries[i]->name, ".png") ||
                   ends_with_ci(entries[i]->name, ".gif") ||
                   png_is_png(entries[i]->data, ramfs_inline_len(entries[i])) ||
                   gif_is_gif(entries[i]->data, ramfs_inline_len(entries[i]))) {
            if (g_list_count < RAMFS_MAX_ENTRIES)
                g_list[g_list_count++] = entries[i];
        }
    }
}

static void refresh_list(void) {
    g_list_count = 0;
    collect_dir("/");
}

static void decode_bytes(const uint8_t *data, size_t len) {
    g_has_image = 0;
    g_gif_playing = 0;
    g_err_msg = 0;

    if (gif_is_gif(data, len)) {
        g_kind = KIND_GIF;
        int rc = gif_decoder_open(&g_gif, data, len,
                                  g_img_pixels, PNG_MAX_WIDTH, PNG_MAX_HEIGHT,
                                  g_gif_restore, GIF_MAX_PIXELS, &g_gif_info);
        if (rc < 0) { g_err_msg = gif_strerror(rc); return; }
        rc = gif_decoder_next(&g_gif, &g_gif_info);
        if (rc != GIF_FRAME) { g_err_msg = rc < 0 ? gif_strerror(rc) : "Пустой GIF"; return; }
        g_img_w = g_gif_info.width;
        g_img_h = g_gif_info.height;
        g_has_image = 1;
        g_gif_playing = 1;
        g_gif_next_ms = hal_time_ms() + (g_gif_info.delay_ms ? g_gif_info.delay_ms : 100);
        return;
    }

    if (png_is_png(data, len)) {
        g_kind = KIND_PNG;
        png_info_t info;
        int rc = png_decode(data, len, g_img_pixels, PNG_MAX_WIDTH, PNG_MAX_HEIGHT, &info);
        if (rc != PNG_OK) { g_err_msg = png_strerror(rc); return; }
        g_img_w = info.width;
        g_img_h = info.height;
        g_has_image = 1;
        return;
    }

    g_err_msg = "Неподдерживаемый формат";
}

void image_app_init(image_app_t *app, const gui_window_t *win) {
    app->win = win;
    app->dark = 0;
    app->scale = 1;
    app->mode = MODE_LIST;
    app->scroll = 0;
    app->selected = 0;
    refresh_list();
}

void image_app_set_theme(image_app_t *app, int dark, int scale) {
    app->dark = dark;
    app->scale = (scale < 1) ? 1 : scale;
}

void image_app_open(image_app_t *app, const char *path) {
    str_copy(g_cur_path, path, sizeof(g_cur_path));
    app->mode = MODE_VIEW;
    g_zoom_fit = 1;

    if (str_eq(path, ":about.png")) {
        decode_bytes(g_sample_about_png, g_sample_about_png_len);
        return;
    }
    if (str_eq(path, ":ineffa.gif")) {
        decode_bytes(g_sample_ineffa_gif, g_sample_ineffa_gif_len);
        return;
    }

    const ramfs_file_t *f = ramfs_lookup(path);
    if (!f || f->is_dir) {
        g_has_image = 0;
        g_gif_playing = 0;
        g_err_msg = "Файл не найден";
        return;
    }
    long n = ramfs_read(path, 0, g_file_buf, sizeof(g_file_buf));
    if (n <= 0) {
        g_has_image = 0;
        g_gif_playing = 0;
        g_err_msg = "Ошибка чтения файла";
        return;
    }
    decode_bytes(g_file_buf, (size_t)n);
}

static void open_by_index(image_app_t *app, int idx) {
    if (idx == 0) image_app_open(app, ":about.png");
    else if (idx == 1) image_app_open(app, ":ineffa.gif");
    else if (idx >= 2 && idx - 2 < g_list_count)
        image_app_open(app, g_list[idx - 2]->path);
}

static void format_dims(uint32_t w, uint32_t h, char *out, int cap) {
    char tmp[12];
    int p = 0;
    uint32_t vals[2] = { w, h };
    for (int k = 0; k < 2; k++) {
        if (k == 1 && p < cap - 1) out[p++] = 'x';
        uint32_t v = vals[k];
        int n = 0;
        do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 12);
        while (n > 0 && p < cap - 1) out[p++] = tmp[--n];
    }
    out[p] = '\0';
}

static void draw_image_pixels(int vx, int vy, int vw, int vh) {
    if (vw <= 0 || vh <= 0 || g_img_w == 0 || g_img_h == 0) return;

    int dw, dh;
    if (g_zoom_fit) {
        if (g_img_w * (uint32_t)vh <= g_img_h * (uint32_t)vw) {
            dh = vh;
            dw = (int)((g_img_w * (uint32_t)vh) / g_img_h);
        } else {
            dw = vw;
            dh = (int)((g_img_h * (uint32_t)vw) / g_img_w);
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    } else {
        dw = (int)g_img_w;
        dh = (int)g_img_h;
    }

    int ox = vx + (vw - dw) / 2;
    int oy = vy + (vh - dh) / 2;

    gfxfb_set_clip(vx, vy, vw, vh);
    for (int dy = 0; dy < dh; dy++) {
        int py = oy + dy;
        if (py < vy || py >= vy + vh) continue;
        uint32_t sy = (uint32_t)dy * g_img_h / (uint32_t)dh;
        if (sy >= g_img_h) sy = g_img_h - 1;
        const uint32_t *row = &g_img_pixels[sy * g_img_w];

        for (int dx = 0; dx < dw; dx++) {
            int px = ox + dx;
            if (px < vx || px >= vx + vw) continue;
            uint32_t sx = (uint32_t)dx * g_img_w / (uint32_t)dw;
            if (sx >= g_img_w) sx = g_img_w - 1;
            uint32_t argb = row[sx];
            uint32_t a = argb >> 24;

            uint32_t bg_rgb;
            if (g_bg_mode == BG_DARK)       bg_rgb = GFX_RGB(24, 26, 32);
            else if (g_bg_mode == BG_LIGHT) bg_rgb = GFX_RGB(245, 247, 250);
            else {
                int cell = ((dx >> 3) ^ (dy >> 3)) & 1;
                bg_rgb = cell ? GFX_RGB(190, 194, 200) : GFX_RGB(228, 232, 238);
            }

            if (a == 255) {
                gfxfb_put_rgb(px, py, argb & 0xFFFFFFu);
            } else if (a == 0) {
                gfxfb_put_rgb(px, py, bg_rgb);
            } else {
                uint32_t r = (((argb >> 16) & 0xFF) * a + ((bg_rgb >> 16) & 0xFF) * (255 - a)) / 255;
                uint32_t g = (((argb >> 8)  & 0xFF) * a + ((bg_rgb >> 8)  & 0xFF) * (255 - a)) / 255;
                uint32_t b = ((argb & 0xFF)         * a + (bg_rgb & 0xFF)         * (255 - a)) / 255;
                gfxfb_put_rgb(px, py, GFX_RGB(r, g, b));
            }
        }
    }
    gfxfb_reset_clip();
}

void image_app_render(image_app_t *app) {
    int cx, cy, cw, ch;
    gui_window_content_rect(app->win, &cx, &cy, &cw, &ch);
    if (cw <= 0 || ch <= 0) return;

    int s = app->scale;
    uint8_t bg_col = app->dark ? GFX_DARK_GREY : GFX_WHITE;
    uint8_t fg_col = app->dark ? GFX_WHITE : GFX_BLACK;
    uint8_t bar_col = app->dark ? GFX_BLACK : GFX_LIGHT_GREY;
    uint8_t sel_col = app->dark ? GFX_BLUE : GFX_XP_TITLE_LIGHT;

    hal_gfx_fill_rect(cx, cy, cw, ch, bg_col);
    gfxfb_set_clip(cx, cy, cw, ch);

    int bar_h = 12 * s;
    hal_gfx_fill_rect(cx, cy, cw, bar_h, bar_col);

    if (app->mode == MODE_LIST) {
        refresh_list();
        hal_gfx_draw_string_scaled(cx + 4 * s, cy + (bar_h - FONT_H * s) / 2,
                                   "Изображения (PNG/GIF)", fg_col, s);

        int row_h = 12 * s;
        int total = 2 + g_list_count;
        int list_y = cy + bar_h + 2 * s;
        int vis_rows = (ch - bar_h - 2 * s) / row_h;
        if (vis_rows < 1) vis_rows = 1;
        if (app->scroll > total - vis_rows) app->scroll = total - vis_rows;
        if (app->scroll < 0) app->scroll = 0;

        static const char *const sample_names[2] = {
            "about.png (образец)",
            "ineffa.gif (образец)"
        };

        for (int i = app->scroll; i < total; i++) {
            int ry = list_y + (i - app->scroll) * row_h;
            if (ry + row_h > cy + ch) break;
            int is_sel = (i == app->selected);
            if (is_sel) hal_gfx_fill_rect(cx, ry, cw, row_h, sel_col);
            uint8_t tc = is_sel ? GFX_WHITE : fg_col;
            icon_draw(ICON_FILE, cx + 3 * s, ry + (row_h - 8 * s) / 2, 8 * s,
                      hal_gfx_palette_rgb(tc));
            const char *name = (i < 2) ? sample_names[i] : g_list[i - 2]->name;
            hal_gfx_draw_string_scaled(cx + 14 * s, ry + (row_h - FONT_H * s) / 2,
                                       name, tc, s);
        }
        gfxfb_reset_clip();
        return;
    }

    /* MODE_VIEW: панель управления */
    int bw_back = 36 * s;
    int bw_fit  = 34 * s;
    int bw_bg   = 28 * s;

    hal_gfx_draw_string_scaled(cx + 3 * s, cy + (bar_h - FONT_H * s) / 2,
                               "< Назад", fg_col, s);
    hal_gfx_draw_string_scaled(cx + bw_back + 4 * s, cy + (bar_h - FONT_H * s) / 2,
                               g_zoom_fit ? "[Авто]" : "[1:1]", fg_col, s);
    hal_gfx_draw_string_scaled(cx + bw_back + bw_fit + 4 * s, cy + (bar_h - FONT_H * s) / 2,
                               "[Фон]", fg_col, s);

    if (g_has_image) {
        char dims[24];
        format_dims(g_img_w, g_img_h, dims, sizeof(dims));
        int dw = hal_gfx_string_width(dims, s);
        if (cx + cw - dw - 3 * s > cx + bw_back + bw_fit + bw_bg)
            hal_gfx_draw_string_scaled(cx + cw - dw - 3 * s, cy + (bar_h - FONT_H * s) / 2,
                                       dims, fg_col, s);
    }
    gfxfb_reset_clip();

    int vy = cy + bar_h;
    int vh = ch - bar_h;
    if (g_has_image) {
        draw_image_pixels(cx, vy, cw, vh);
    } else if (g_err_msg) {
        gfxfb_set_clip(cx, vy, cw, vh);
        hal_gfx_draw_string_centered(cx, vy + vh / 2 - FONT_H * s / 2, cw,
                                     g_err_msg, GFX_LIGHT_RED, s);
        gfxfb_reset_clip();
    }
}

int image_app_handle_click(image_app_t *app, int mx, int my) {
    int cx, cy, cw, ch;
    gui_window_content_rect(app->win, &cx, &cy, &cw, &ch);
    if (!gui_point_in_rect(mx, my, cx, cy, cw, ch)) return 0;

    int s = app->scale;
    int bar_h = 12 * s;

    if (app->mode == MODE_LIST) {
        int row_h = 12 * s;
        int list_y = cy + bar_h + 2 * s;
        if (my < list_y) return 0;
        int idx = app->scroll + (my - list_y) / row_h;
        int total = 2 + g_list_count;
        if (idx >= 0 && idx < total) {
            app->selected = idx;
            open_by_index(app, idx);
            return 1;
        }
        return 0;
    }

    if (my < cy + bar_h) {
        int bw_back = 36 * s;
        int bw_fit  = 34 * s;
        int bw_bg   = 28 * s;
        if (mx < cx + bw_back) {
            app->mode = MODE_LIST;
            g_gif_playing = 0;
            return 1;
        }
        if (mx < cx + bw_back + bw_fit) {
            g_zoom_fit = !g_zoom_fit;
            return 1;
        }
        if (mx < cx + bw_back + bw_fit + bw_bg) {
            g_bg_mode = (g_bg_mode + 1) % 3;
            return 1;
        }
    }
    return 0;
}

int image_app_handle_key(image_app_t *app, int key) {
    if (app->mode == MODE_LIST) {
        int total = 2 + g_list_count;
        if (key == HAL_KEY_UP && app->selected > 0) { app->selected--; return 1; }
        if (key == HAL_KEY_DOWN && app->selected < total - 1) { app->selected++; return 1; }
        if (key == '\n' || key == '\r') { open_by_index(app, app->selected); return 1; }
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < total) { app->selected = idx; open_by_index(app, idx); return 1; }
        }
        return 0;
    }

    if (key == 27 || key == '\b' || key == 127) {
        app->mode = MODE_LIST;
        g_gif_playing = 0;
        return 1;
    }
    if (key == ' ' || key == 'f' || key == 'F') {
        g_zoom_fit = !g_zoom_fit;
        return 1;
    }
    if (key == 'b' || key == 'B') {
        g_bg_mode = (g_bg_mode + 1) % 3;
        return 1;
    }
    return 0;
}

int image_app_tick(image_app_t *app) {
    if (app->mode != MODE_VIEW || !g_has_image || g_kind != KIND_GIF || !g_gif_playing)
        return 0;
    uint64_t now = hal_time_ms();
    if (now < g_gif_next_ms) return 0;
    int rc = gif_decoder_next(&g_gif, &g_gif_info);
    if (rc == GIF_FRAME) {
        g_gif_next_ms = now + (g_gif_info.delay_ms ? g_gif_info.delay_ms : 100);
        return 1;
    }
    g_gif_playing = 0;
    return 0;
}
