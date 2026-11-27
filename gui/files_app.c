/* gui/files_app.c — оконный проводник для просмотра файлов в RamFS. */
#include "files_app.h"
#include "gfxfb.h"
#include "icon.h"
#include "png.h"
#include "gif.h"
#include "hal.h"

#define MODE_LIST 0
#define MODE_VIEW 1

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int ends_with_ci(const char *name, const char *ext) {
    int n = str_len(name), e = str_len(ext);
    if (n < e) return 0;
    for (int i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

static int is_image_file(const ramfs_file_t *f) {
    if (ends_with_ci(f->name, ".png") || ends_with_ci(f->name, ".gif")) return 1;
    size_t ilen = ramfs_inline_len(f);
    return png_is_png(f->data, ilen) || gif_is_gif(f->data, ilen);
}

static void format_size(size_t sz, char *out, int cap) {
    char tmp[16];
    int n = 0;
    uint32_t v = (uint32_t)sz;
    char unit = 'B';
    if (v >= 1024u) { v /= 1024u; unit = 'K'; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 15);
    int p = 0;
    while (n > 0 && p < cap - 2) out[p++] = tmp[--n];
    if (p < cap - 1) out[p++] = unit;
    out[p] = '\0';
}

static void go_parent(files_app_t *app) {
    int len = str_len(app->cwd);
    int last = 0;
    for (int i = 0; i < len; i++) if (app->cwd[i] == '/') last = i;
    if (last == 0) str_copy(app->cwd, "/", sizeof(app->cwd));
    else app->cwd[last] = '\0';
    app->scroll = 0;
    app->selected = 0;
}

static void open_entry(files_app_t *app, const ramfs_file_t *f) {
    if (f->is_dir) {
        str_copy(app->cwd, f->path, sizeof(app->cwd));
        app->scroll = 0;
        app->selected = 0;
        return;
    }
    if (is_image_file(f) && app->on_open_image) {
        app->on_open_image(f->path);
        return;
    }
    str_copy(app->view_name, f->name, sizeof(app->view_name));
    app->mode = MODE_VIEW;
    gconsole_clear(&app->viewer);

    uint8_t chunk[256];
    size_t off = 0;
    int any = 0;
    for (;;) {
        long n = ramfs_read(f->path, off, chunk, sizeof(chunk));
        if (n <= 0) break;
        any = 1;
        for (long i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            gconsole_putc(&app->viewer,
                          (c < 32 && c != '\n' && c != '\t') || c == 127 ? '.' : (char)c);
        }
        off += (size_t)n;
    }
    if (!any) gconsole_write(&app->viewer, "(пустой файл)");
    gconsole_invalidate(&app->viewer);
}

void files_app_init(files_app_t *app, const gui_window_t *win, files_open_image_fn cb) {
    app->win = win;
    gconsole_init(&app->viewer, win, GFX_BLACK, GFX_WHITE);
    str_copy(app->cwd, "/", sizeof(app->cwd));
    app->view_name[0] = '\0';
    app->dark = 0;
    app->scale = 1;
    app->mode = MODE_LIST;
    app->scroll = 0;
    app->selected = 0;
    app->on_open_image = cb;
}

void files_app_set_theme(files_app_t *app, int dark, int scale, uint8_t fg, uint8_t bg) {
    app->dark = dark;
    app->scale = (scale < 1) ? 1 : scale;
    gconsole_set_scale(&app->viewer, app->scale);
    gconsole_set_colors(&app->viewer, fg, bg);
}

void files_app_render(files_app_t *app) {
    int cx, cy, cw, ch;
    gui_window_content_rect(app->win, &cx, &cy, &cw, &ch);
    if (cw <= 0 || ch <= 0) return;

    int s = app->scale;
    uint8_t bg_col  = app->dark ? GFX_DARK_GREY : GFX_WHITE;
    uint8_t fg_col  = app->dark ? GFX_WHITE : GFX_BLACK;
    uint8_t bar_col = app->dark ? GFX_BLACK : GFX_LIGHT_GREY;
    uint8_t sel_col = app->dark ? GFX_BLUE : GFX_XP_TITLE_LIGHT;

    int bar_h = 12 * s;

    gfxfb_set_clip(cx, cy, cw, ch);
    hal_gfx_fill_rect(cx, cy, cw, bar_h, bar_col);

    if (app->mode == MODE_VIEW) {
        hal_gfx_draw_string_scaled(cx + 3 * s, cy + (bar_h - FONT_H * s) / 2,
                                   "< Назад", fg_col, s);
        hal_gfx_draw_string_scaled(cx + 40 * s, cy + (bar_h - FONT_H * s) / 2,
                                   app->view_name, fg_col, s);
        gfxfb_reset_clip();

        gconsole_set_rect(&app->viewer, cx, cy + bar_h, cw, ch - bar_h);
        gconsole_invalidate(&app->viewer);
        gconsole_render(&app->viewer);
        return;
    }

    hal_gfx_fill_rect(cx, cy + bar_h, cw, ch - bar_h, bg_col);
    hal_gfx_draw_string_scaled(cx + 3 * s, cy + (bar_h - FONT_H * s) / 2,
                               "[..]", fg_col, s);
    hal_gfx_draw_string_scaled(cx + 22 * s, cy + (bar_h - FONT_H * s) / 2,
                               app->cwd, fg_col, s);

    const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(app->cwd, files, RAMFS_MAX_ENTRIES);

    int list_y = cy + bar_h + 2 * s;
    int row_h = 12 * s;
    int vis_rows = (ch - bar_h - 2 * s) / row_h;
    if (vis_rows < 1) vis_rows = 1;
    if (app->selected >= n && n > 0) app->selected = n - 1;
    if (app->scroll > n - vis_rows) app->scroll = n - vis_rows;
    if (app->scroll < 0) app->scroll = 0;

    if (n == 0) {
        hal_gfx_draw_string_centered(cx, list_y + (ch - bar_h) / 2 - FONT_H * s / 2,
                                     cw, "(пусто)", fg_col, s);
        gfxfb_reset_clip();
        return;
    }

    for (int i = app->scroll; i < n; i++) {
        int ry = list_y + (i - app->scroll) * row_h;
        if (ry + row_h > cy + ch) break;
        int is_sel = (i == app->selected);
        if (is_sel) hal_gfx_fill_rect(cx, ry, cw, row_h, sel_col);
        uint8_t tc = is_sel ? GFX_WHITE : fg_col;

        icon_draw(files[i]->is_dir ? ICON_FOLDER : ICON_FILE,
                  cx + 3 * s, ry + (row_h - 8 * s) / 2, 8 * s,
                  hal_gfx_palette_rgb(tc));
        hal_gfx_draw_string_scaled(cx + 14 * s, ry + (row_h - FONT_H * s) / 2,
                                   files[i]->name, tc, s);

        if (!files[i]->is_dir) {
            char sz[16];
            format_size(files[i]->size, sz, sizeof(sz));
            int sw = hal_gfx_string_width(sz, s);
            hal_gfx_draw_string_scaled(cx + cw - sw - 3 * s, ry + (row_h - FONT_H * s) / 2,
                                       sz, tc, s);
        }
    }
    gfxfb_reset_clip();
}

int files_app_handle_click(files_app_t *app, int mx, int my) {
    int cx, cy, cw, ch;
    gui_window_content_rect(app->win, &cx, &cy, &cw, &ch);
    if (!gui_point_in_rect(mx, my, cx, cy, cw, ch)) return 0;

    int s = app->scale;
    int bar_h = 12 * s;

    if (app->mode == MODE_VIEW) {
        if (my < cy + bar_h && mx < cx + 38 * s) {
            app->mode = MODE_LIST;
            return 1;
        }
        return 0;
    }

    if (my < cy + bar_h) {
        if (mx < cx + 22 * s) {
            go_parent(app);
            return 1;
        }
        return 0;
    }

    int row_h = 12 * s;
    int list_y = cy + bar_h + 2 * s;
    if (my < list_y) return 0;
    int idx = app->scroll + (my - list_y) / row_h;

    const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(app->cwd, files, RAMFS_MAX_ENTRIES);
    if (idx >= 0 && idx < n) {
        app->selected = idx;
        open_entry(app, files[idx]);
        return 1;
    }
    return 0;
}

int files_app_handle_key(files_app_t *app, int key) {
    if (app->mode == MODE_VIEW) {
        if (key == 27 || key == '\b' || key == 127) {
            app->mode = MODE_LIST;
            return 1;
        }
        return 0;
    }

    const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(app->cwd, files, RAMFS_MAX_ENTRIES);

    if (key == HAL_KEY_UP && app->selected > 0) {
        app->selected--;
        if (app->selected < app->scroll) app->scroll = app->selected;
        return 1;
    }
    if (key == HAL_KEY_DOWN && app->selected < n - 1) {
        app->selected++;
        return 1;
    }
    if (key == '\b' || key == 127) {
        go_parent(app);
        return 1;
    }
    if ((key == '\n' || key == '\r') && app->selected >= 0 && app->selected < n) {
        open_entry(app, files[app->selected]);
        return 1;
    }
    return 0;
}
