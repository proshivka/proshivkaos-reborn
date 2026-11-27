/* gui/wm.c — оконный менеджер proshivkaOS NEXT (стили Windows XP и Windows 11).
 * Рабочий стол при старте чист — окна открываются через меню "Пуск".
 * Мышь/тачпад: клик — фокус/меню, тяга за title bar — перенос окна,
 * тяга за правый нижний угол — ресайз. Tab переключает фокус клавиатурой
 * между окнами (а внутри терминала выполняет автодополнение).
 */
#include "hal_gfx.h"
#include "window.h"
#include "wallpaper.h"
#include "cursor.h"
#include "icon.h"
#include "terminal_app.h"
#include "settings_app.h"
#include "files_app.h"
#include "image_app.h"
#include "mouse.h"
#include "rtc.h"
#include "terminal_net.h"

int keyboard_poll(void);          /* arch/x86/keyboard.c */
int keyboard_get_layout(void);    /* arch/x86/keyboard.c */

#define WIN_TERMINAL 0
#define WIN_FILES    1
#define WIN_IMAGE    2
#define WIN_SETTINGS 3
#define WIN_COUNT    4
#define FOCUS_NONE   (-1)

static gui_window_t wins[WIN_COUNT];
static int z_order[WIN_COUNT] = { WIN_TERMINAL, WIN_FILES, WIN_IMAGE, WIN_SETTINGS };
static int win_icons[WIN_COUNT] = { ICON_TERMINAL, ICON_FOLDER, ICON_PUBLIC, ICON_SETTINGS };

static terminal_app_t term;
static settings_app_t settings;
static files_app_t    files_app;
static image_app_t    img_app;

static uint8_t desktop_bg = GFX_CYAN;
static int desktop_wallpaper = 1;
static int theme = GUI_THEME_XP_LIGHT;
static int ui_scale = 1;
static int screen_w = 320, screen_h = 200;
static int mouse_x, mouse_y;
static int start_menu_open = 0;
static int calendar_open = 0;
static int cal_year = 2026, cal_month = 1;
static int last_layout = 0;
static int focus = FOCUS_NONE;

static int is_dark_theme(void) {
    return theme == GUI_THEME_XP_DARK || theme == GUI_THEME_WIN11_DARK;
}

static void set_focus_win(int new_focus) {
    focus = new_focus;
    for (int i = 0; i < WIN_COUNT; i++)
        wins[i].focused = (i == focus);

    if (new_focus >= 0 && new_focus < WIN_COUNT) {
        int pos = -1;
        for (int i = 0; i < WIN_COUNT; i++)
            if (z_order[i] == new_focus) { pos = i; break; }
        if (pos >= 0) {
            for (int i = pos; i < WIN_COUNT - 1; i++)
                z_order[i] = z_order[i + 1];
            z_order[WIN_COUNT - 1] = new_focus;
        }
    }
}

static void clamp_window(gui_window_t *win, int tb_h) {
    if (win->maximized) {
        win->x = 0; win->y = 0;
        win->w = screen_w; win->h = screen_h - tb_h;
    } else {
        if (win->w > screen_w) win->w = screen_w;
        if (win->h > screen_h - tb_h) win->h = screen_h - tb_h;
        if (win->x + win->w > screen_w) win->x = screen_w - win->w;
        if (win->x < 0) win->x = 0;
        if (win->y + win->h > screen_h - tb_h)
            win->y = screen_h - tb_h - win->h;
        if (win->y < 0) win->y = 0;
    }
}

static void layout_windows(void) {
    int tb_h = gui_taskbar_height(screen_h, theme);
    for (int i = 0; i < WIN_COUNT; i++)
        clamp_window(&wins[i], tb_h);
}

static void apply_theme(void) {
    int dark = is_dark_theme();
    gui_set_theme(theme);
    gui_theme_sync_palette(theme);

    uint8_t fg, term_bg, ui_bg;
    if (theme == GUI_THEME_WIN11_DARK) {
        fg = GFX_W11_DARK_TEXT; term_bg = GFX_W11_DARK_TERM; ui_bg = GFX_W11_DARK_BG;
    } else if (theme == GUI_THEME_WIN11_LIGHT) {
        fg = GFX_W11_LIGHT_TEXT; term_bg = GFX_W11_LIGHT_TERM; ui_bg = GFX_W11_LIGHT_BG;
    } else if (theme == GUI_THEME_XP_DARK) {
        fg = GFX_WHITE; term_bg = GFX_DARK_GREY; ui_bg = GFX_DARK_GREY;
    } else {
        fg = GFX_BLACK; term_bg = GFX_WHITE; ui_bg = GFX_LIGHT_GREY;
    }

    gconsole_set_colors(&term.console, fg, term_bg);
    gconsole_set_colors(&settings.console, fg, ui_bg);
    files_app_set_theme(&files_app, dark, ui_scale, fg, term_bg);
    image_app_set_theme(&img_app, dark, ui_scale);

    layout_windows();
}

static void apply_ui_scale(int new_scale) {
    if (new_scale < 1) new_scale = 1;
    if (new_scale > 3) new_scale = 3;
    ui_scale = new_scale;

    gui_set_scale(ui_scale);
    gconsole_set_scale(&term.console, ui_scale);
    gconsole_set_scale(&settings.console, ui_scale);
    apply_theme();

    static const int base_w[WIN_COUNT] = { 150, 160, 170, 140 };
    static const int base_h[WIN_COUNT] = { 150, 150, 150, 150 };
    int tb_h = gui_taskbar_height(screen_h, theme);
    int max_h = screen_h - tb_h;
    if (max_h < MIN_WIN_H) max_h = MIN_WIN_H;

    for (int i = 0; i < WIN_COUNT; i++) {
        if (!wins[i].maximized) {
            int w = base_w[i] * ui_scale;
            int h = base_h[i] * ui_scale;
            if (w > screen_w) w = screen_w;
            if (h > max_h) h = max_h;
            wins[i].w = w;
            wins[i].h = h;
        }
    }

    layout_windows();
}

static void toggle_maximize(gui_window_t *win) {
    int tb_h = gui_taskbar_height(screen_h, theme);
    if (win->maximized) {
        win->x = win->saved_x; win->y = win->saved_y;
        win->w = win->saved_w; win->h = win->saved_h;
        win->maximized = 0;
    } else {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
        win->x = 0; win->y = 0;
        win->w = screen_w; win->h = screen_h - tb_h;
        win->maximized = 1;
    }
}

static void draw_window_by_id(int id, int dark) {
    const gui_window_t *win = &wins[id];
    if (!win->open || win->minimized) return;
    gui_draw_window_chrome(win, dark);
    if (id == WIN_TERMINAL) {
        gconsole_invalidate(&term.console);
        gconsole_render(&term.console);
    } else if (id == WIN_SETTINGS) {
        gconsole_invalidate(&settings.console);
        gconsole_render(&settings.console);
    } else if (id == WIN_FILES) {
        files_app_render(&files_app);
    } else if (id == WIN_IMAGE) {
        image_app_render(&img_app);
    }
}

static void render_frame(void) {
    int dark = is_dark_theme();
    gui_set_cursor_pos(mouse_x, mouse_y);

    if (desktop_wallpaper)
        gui_draw_wallpaper_themed(screen_w, screen_h, theme);
    else
        gui_draw_desktop(desktop_bg);

    for (int i = 0; i < WIN_COUNT; i++)
        draw_window_by_id(z_order[i], dark);

    int tb_count = 0;
    for (int i = 0; i < WIN_COUNT; i++)
        if (wins[i].open) tb_count++;

    int y = 2026, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    rtc_read(&y, &mo, &d, &h, &mi, &s);

    gui_draw_taskbar(screen_w, screen_h, tb_count, start_menu_open, h, mi, keyboard_get_layout());

    int tb_index = 0;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (wins[i].open) {
            gui_draw_taskbar_button(screen_w, screen_h, tb_index++, tb_count,
                                    wins[i].title, win_icons[i],
                                    focus == i && !wins[i].minimized, dark);
        }
    }

    if (start_menu_open)
        gui_draw_start_menu(screen_w, screen_h, dark);

    if (calendar_open)
        gui_draw_calendar(screen_w, screen_h, cal_year, cal_month, y, mo, d, dark);

    gui_draw_cursor_scaled(mouse_x, mouse_y, ui_scale);

    hal_gfx_present();
}

static void apply_resolution(int w, int h) {
    if (!hal_gfx_set_resolution(w, h))
        return;

    screen_w = hal_gfx_width();
    screen_h = hal_gfx_height();
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    layout_windows();

    render_frame();
}

static void open_image_from_files(const char *path) {
    wins[WIN_IMAGE].open = 1;
    wins[WIN_IMAGE].minimized = 0;
    set_focus_win(WIN_IMAGE);
    image_app_open(&img_app, path);
}

static int dragging = 0, resizing = 0;
static gui_window_t *drag_target = 0;
static gui_window_t *resize_target = 0;
static int need_render = 0;
static unsigned long last_clock_sec = 0;
static int last_clock_min = -1;

static void gui_request_redraw(void) { need_render = 1; }

static int find_fallback_focus(int exclude) {
    for (int i = WIN_COUNT - 1; i >= 0; i--) {
        int id = z_order[i];
        if (id != exclude && wins[id].open && !wins[id].minimized)
            return id;
    }
    return FOCUS_NONE;
}

static int handle_window_click(int id) {
    gui_window_t *win = &wins[id];
    if (!win->open || win->minimized) return 0;

    if (gui_point_in_close_button(mouse_x, mouse_y, win)) {
        win->open = 0;
        if (focus == id) set_focus_win(find_fallback_focus(id));
        else set_focus_win(focus);
        return 1;
    }
    if (gui_point_in_maximize_button(mouse_x, mouse_y, win)) {
        toggle_maximize(win);
        set_focus_win(id);
        return 1;
    }
    if (gui_point_in_minimize_button(mouse_x, mouse_y, win)) {
        win->minimized = 1;
        if (focus == id) set_focus_win(find_fallback_focus(id));
        return 1;
    }
    if (!win->maximized && gui_point_in_resize_grip(mouse_x, mouse_y, win)) {
        resizing = 1;
        resize_target = win;
        set_focus_win(id);
        return 1;
    }
    if (!win->maximized && gui_point_in_titlebar(mouse_x, mouse_y, win)) {
        dragging = 1;
        drag_target = win;
        set_focus_win(id);
        return 1;
    }
    if (gui_point_in_rect(mouse_x, mouse_y, win->x, win->y, win->w, win->h)) {
        set_focus_win(id);
        if (id == WIN_FILES) files_app_handle_click(&files_app, mouse_x, mouse_y);
        else if (id == WIN_IMAGE) image_app_handle_click(&img_app, mouse_x, mouse_y);
        return 1;
    }
    return 0;
}

static void gui_step(void) {
    rtc_uptime_tick();

    {
        unsigned long sec = rtc_uptime_seconds();
        if (sec != last_clock_sec) {
            last_clock_sec = sec;
            int y, mo, d, h, mi, s;
            rtc_read(&y, &mo, &d, &h, &mi, &s);
            if (mi != last_clock_min) {
                last_clock_min = mi;
                need_render = 1;
            }
        }
    }

    if (wins[WIN_IMAGE].open && !wins[WIN_IMAGE].minimized && image_app_tick(&img_app))
        need_render = 1;

    int handled_something = 0;

    int c = keyboard_poll();
    {
        int cur_layout = keyboard_get_layout();
        if (cur_layout != last_layout) {
            last_layout = cur_layout;
            need_render = 1;
        }
    }
    if (c != -1) {
        handled_something = 1;

        if (c == 27 && calendar_open) {
            calendar_open = 0;
        } else if (c == '\t' && focus != WIN_TERMINAL) {
            for (int step = 1; step <= WIN_COUNT; step++) {
                int cand = (focus < 0 ? 0 : focus + step) % WIN_COUNT;
                if (wins[cand].open && !wins[cand].minimized) {
                    set_focus_win(cand);
                    break;
                }
            }
        } else if (focus == WIN_TERMINAL) {
            terminal_app_handle_key(&term, c);
        } else if (focus == WIN_FILES) {
            files_app_handle_key(&files_app, c);
        } else if (focus == WIN_IMAGE) {
            image_app_handle_key(&img_app, c);
        } else if (focus == WIN_SETTINGS && c >= 0 && c <= 0xFF) {
            int r = settings_app_handle_char(&settings, (char)c);
            if (r == SETTINGS_RES_320x200) apply_resolution(320, 200);
            else if (r == SETTINGS_RES_640x480) apply_resolution(640, 480);
            else if (r == SETTINGS_RES_800x600) apply_resolution(800, 600);
            else if (r == SETTINGS_WALLPAPER) desktop_wallpaper = 1;
            else if (r == SETTINGS_COLOR_CHANGED) desktop_wallpaper = 0;
            else if (r == SETTINGS_THEME_LIGHT)      { theme = GUI_THEME_XP_LIGHT;    apply_theme(); }
            else if (r == SETTINGS_THEME_DARK)       { theme = GUI_THEME_XP_DARK;     apply_theme(); }
            else if (r == SETTINGS_THEME_WIN11)      { theme = GUI_THEME_WIN11_LIGHT; desktop_wallpaper = 1; apply_theme(); }
            else if (r == SETTINGS_THEME_WIN11_DARK) { theme = GUI_THEME_WIN11_DARK;  desktop_wallpaper = 1; apply_theme(); }
            else if (r == SETTINGS_SCALE_1)          { apply_ui_scale(1); }
            else if (r == SETTINGS_SCALE_2)          { apply_ui_scale(2); }
            else if (r == SETTINGS_SCALE_3)          { apply_ui_scale(3); }
        }
    }

    mouse_event_t mev;
    if (mouse_poll(&mev)) {
        handled_something = 1;

        mouse_x += mev.dx;
        mouse_y += mev.dy;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= screen_w) mouse_x = screen_w - 1;
        if (mouse_y >= screen_h) mouse_y = screen_h - 1;

        if (mev.left) {
            if (dragging) {
                drag_target->x += mev.dx;
                drag_target->y += mev.dy;
            } else if (resizing) {
                int min_w = MIN_WIN_W * ui_scale;
                int min_h = MIN_WIN_H * ui_scale;
                if (min_w > screen_w) min_w = screen_w;
                if (min_h > screen_h - gui_taskbar_height(screen_h, theme))
                    min_h = screen_h - gui_taskbar_height(screen_h, theme);
                resize_target->w += mev.dx;
                resize_target->h += mev.dy;
                if (resize_target->w < min_w) resize_target->w = min_w;
                if (resize_target->h < min_h) resize_target->h = min_h;
            } else if (start_menu_open) {
                for (int i = 0; i < START_MENU_ITEMS; i++) {
                    int ix, iy, iw, ih;
                    gui_start_menu_item_rect(screen_w, screen_h, i, &ix, &iy, &iw, &ih);
                    if (gui_point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                        wins[i].open = 1;
                        wins[i].minimized = 0;
                        set_focus_win(i);
                        break;
                    }
                }
                start_menu_open = 0;
            } else {
                int tb_count = 0;
                for (int i = 0; i < WIN_COUNT; i++)
                    if (wins[i].open) tb_count++;

                int sx, sy, sw, sh;
                gui_start_button_rect(screen_w, screen_h, tb_count, &sx, &sy, &sw, &sh);

                int cx, cy, cw, ch;
                gui_clock_rect(screen_w, screen_h, &cx, &cy, &cw, &ch);

                if (calendar_open) {
                    if (gui_calendar_handle_click(screen_w, screen_h, mouse_x, mouse_y,
                                                  &cal_year, &cal_month)) {
                        /* клик внутри календаря обработан */
                    } else {
                        calendar_open = 0;
                    }
                } else if (gui_point_in_rect(mouse_x, mouse_y, cx, cy, cw, ch)) {
                    int y, mo, d, h, mi, s;
                    rtc_read(&y, &mo, &d, &h, &mi, &s);
                    cal_year = y;
                    cal_month = mo;
                    calendar_open = 1;
                    start_menu_open = 0;
                } else if (gui_point_in_rect(mouse_x, mouse_y, sx, sy, sw, sh)) {
                    start_menu_open = 1;
                } else {
                    int tb_hit = 0;
                    int tb_idx = 0;
                    for (int i = 0; i < WIN_COUNT; i++) {
                        if (!wins[i].open) continue;
                        int bx, by, bw, bh;
                        gui_taskbar_button_rect(screen_w, screen_h, tb_idx++, tb_count,
                                                &bx, &by, &bw, &bh);
                        if (gui_point_in_rect(mouse_x, mouse_y, bx, by, bw, bh)) {
                            if (wins[i].minimized) {
                                wins[i].minimized = 0;
                                set_focus_win(i);
                            } else if (focus == i) {
                                wins[i].minimized = 1;
                                set_focus_win(find_fallback_focus(i));
                            } else {
                                set_focus_win(i);
                            }
                            tb_hit = 1;
                            break;
                        }
                    }
                    if (!tb_hit) {
                        for (int i = WIN_COUNT - 1; i >= 0; i--) {
                            if (handle_window_click(z_order[i])) break;
                        }
                    }
                }
            }
        } else {
            dragging = 0;
            resizing = 0;
            drag_target = 0;
            resize_target = 0;
        }
    }

    if (handled_something || need_render) {
        need_render = 0;
        render_frame();
    }
}

static int net_pump_cb(void) {
    gui_step();
    return terminal_app_cancel_requested(&term);
}

void gui_main(void) {
    hal_gfx_init();
    mouse_init();

    screen_w = hal_gfx_width();
    screen_h = hal_gfx_height();
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    wins[WIN_TERMINAL] = (gui_window_t){   8,  8, 150, 150, "Терминал",    0, 0, 0, 0, 0, 0, 0, 0 };
    wins[WIN_FILES]    = (gui_window_t){  30, 16, 160, 150, "Проводник",   0, 0, 0, 0, 0, 0, 0, 0 };
    wins[WIN_IMAGE]    = (gui_window_t){  55, 12, 170, 150, "Изображения", 0, 0, 0, 0, 0, 0, 0, 0 };
    wins[WIN_SETTINGS] = (gui_window_t){ 165,  8, 140, 150, "Настройки",   0, 0, 0, 0, 0, 0, 0, 0 };

    terminal_app_init(&term, &wins[WIN_TERMINAL]);
    files_app_init(&files_app, &wins[WIN_FILES], open_image_from_files);
    image_app_init(&img_app, &wins[WIN_IMAGE]);
    settings_app_init(&settings, &wins[WIN_SETTINGS], &desktop_bg);
    apply_theme();

    render_frame();

    terminal_net_install(gui_request_redraw, net_pump_cb);

    for (;;)
        gui_step();
}
