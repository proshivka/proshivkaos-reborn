/* gui/wm.c — оконный менеджер proshivkaOS NEXT (стили Windows XP и Windows 11).
 * Рабочий стол при старте чист — окна открываются через меню "Пуск".
 * Мышь/тачпад: клик — фокус/меню, тяга за title bar — перенос окна,
 * тяга за правый нижний угол — ресайз. Tab переключает фокус клавиатурой
 * между уже открытыми окнами.
 */
#include "hal_gfx.h"
#include "window.h"
#include "wallpaper.h"
#include "cursor.h"
#include "icon.h"
#include "terminal_app.h"
#include "settings_app.h"
#include "mouse.h"
#include "rtc.h"
#include "terminal_net.h"

int keyboard_poll(void);          /* arch/x86/keyboard.c */

#define FOCUS_NONE     0
#define FOCUS_TERMINAL 1
#define FOCUS_SETTINGS 2

static gui_window_t term_win;
static gui_window_t set_win;
static terminal_app_t term;
static settings_app_t settings;

static uint8_t desktop_bg = GFX_CYAN;
static int desktop_wallpaper = 1;
static int theme = GUI_THEME_XP_LIGHT;
static int screen_w = 320, screen_h = 200;
static int mouse_x, mouse_y;
static int start_menu_open = 0;
static int focus = FOCUS_NONE;

static int is_dark_theme(void) {
    return theme == GUI_THEME_XP_DARK || theme == GUI_THEME_WIN11_DARK;
}

static void layout_windows(void) {
    int tb_h = gui_taskbar_height(screen_h, theme);

    if (term_win.maximized) {
        term_win.x = 0; term_win.y = 0;
        term_win.w = screen_w; term_win.h = screen_h - tb_h;
    } else {
        if (term_win.x + term_win.w > screen_w) term_win.x = screen_w - term_win.w;
        if (term_win.x < 0) term_win.x = 0;
        if (term_win.y + term_win.h > screen_h - tb_h)
            term_win.y = screen_h - tb_h - term_win.h;
        if (term_win.y < 0) term_win.y = 0;
    }

    if (set_win.maximized) {
        set_win.x = 0; set_win.y = 0;
        set_win.w = screen_w; set_win.h = screen_h - tb_h;
    } else {
        if (set_win.x + set_win.w > screen_w) set_win.x = screen_w - set_win.w;
        if (set_win.x < 0) set_win.x = 0;
        if (set_win.y + set_win.h > screen_h - tb_h)
            set_win.y = screen_h - tb_h - set_win.h;
        if (set_win.y < 0) set_win.y = 0;
    }
}

static void apply_theme(void) {
    gui_set_theme(theme);
    gui_theme_sync_palette(theme);

    if (theme == GUI_THEME_WIN11_DARK) {
        gconsole_set_colors(&term.console,     GFX_W11_DARK_TEXT, GFX_W11_DARK_TERM);
        gconsole_set_colors(&settings.console, GFX_W11_DARK_TEXT, GFX_W11_DARK_BG);
    } else if (theme == GUI_THEME_WIN11_LIGHT) {
        gconsole_set_colors(&term.console,     GFX_W11_LIGHT_TEXT, GFX_W11_LIGHT_TERM);
        gconsole_set_colors(&settings.console, GFX_W11_LIGHT_TEXT, GFX_W11_LIGHT_BG);
    } else if (theme == GUI_THEME_XP_DARK) {
        gconsole_set_colors(&term.console,     GFX_WHITE, GFX_DARK_GREY);
        gconsole_set_colors(&settings.console, GFX_WHITE, GFX_DARK_GREY);
    } else {
        gconsole_set_colors(&term.console,     GFX_BLACK, GFX_WHITE);
        gconsole_set_colors(&settings.console, GFX_BLACK, GFX_LIGHT_GREY);
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

static void draw_one_window(const gui_window_t *win, gconsole_t *gc, int dark) {
    if (!win->open || win->minimized) return;
    gui_draw_window_chrome(win, dark);
    gconsole_invalidate(gc);
    gconsole_render(gc);
}

static void render_frame(void) {
    int dark = is_dark_theme();
    gui_set_cursor_pos(mouse_x, mouse_y);

    if (desktop_wallpaper)
        gui_draw_wallpaper_themed(screen_w, screen_h, theme);
    else
        gui_draw_desktop(desktop_bg);

    /* Рисуем сначала неактивное окно, затем сфокусированное — чтобы оно
     * всегда лежало поверх при перекрытии. */
    if (focus == FOCUS_TERMINAL) {
        draw_one_window(&set_win,  &settings.console, dark);
        draw_one_window(&term_win, &term.console,     dark);
    } else {
        draw_one_window(&term_win, &term.console,     dark);
        draw_one_window(&set_win,  &settings.console, dark);
    }

    int tb_count = (term_win.open ? 1 : 0) + (set_win.open ? 1 : 0);
    int y, mo, d, h = 0, mi = 0, s;
    if (theme == GUI_THEME_WIN11_LIGHT || theme == GUI_THEME_WIN11_DARK)
        rtc_read(&y, &mo, &d, &h, &mi, &s);

    gui_draw_taskbar(screen_w, screen_h, tb_count, start_menu_open, h, mi);

    int tb_index = 0;
    if (term_win.open) {
        gui_draw_taskbar_button(screen_w, screen_h, tb_index++, tb_count, "TERMINAL",
                                ICON_TERMINAL,
                                focus == FOCUS_TERMINAL && !term_win.minimized,
                                dark);
    }
    if (set_win.open) {
        gui_draw_taskbar_button(screen_w, screen_h, tb_index++, tb_count, "SETTINGS",
                                ICON_SETTINGS,
                                focus == FOCUS_SETTINGS && !set_win.minimized,
                                dark);
    }

    if (start_menu_open)
        gui_draw_start_menu(screen_w, screen_h, dark);

    gui_draw_cursor(mouse_x, mouse_y);

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

/* --- Один проход цикла событий --------------------------------------------
 * Вынесен из gui_main, потому что вызывается и изнутри сетевых команд: пока
 * ping или http ждут ответа, окно не должно замирать — оно продолжает
 * реагировать на мышь, перерисовываться и принимать Ctrl+C (см. net_pump_cb). */
static int dragging = 0, resizing = 0;
static gui_window_t *drag_target = 0;
static gui_window_t *resize_target = 0;
static int need_render = 0;
static unsigned long last_clock_sec = 0;
static int last_clock_min = -1;

static void gui_request_redraw(void) { need_render = 1; }

static void set_focus_win(int new_focus) {
    focus = new_focus;
    term_win.focused = (focus == FOCUS_TERMINAL);
    set_win.focused  = (focus == FOCUS_SETTINGS);
}

static int handle_window_click(gui_window_t *win, int win_focus, int other_focus,
                               const gui_window_t *other_win) {
    if (!win->open || win->minimized) return 0;

    if (gui_point_in_close_button(mouse_x, mouse_y, win)) {
        win->open = 0;
        if (focus == win_focus)
            set_focus_win(other_win->open ? other_focus : FOCUS_NONE);
        else
            set_focus_win(focus);
        return 1;
    }
    if (gui_point_in_maximize_button(mouse_x, mouse_y, win)) {
        toggle_maximize(win);
        set_focus_win(win_focus);
        return 1;
    }
    if (gui_point_in_minimize_button(mouse_x, mouse_y, win)) {
        win->minimized = 1;
        return 1;
    }
    if (!win->maximized && gui_point_in_resize_grip(mouse_x, mouse_y, win)) {
        resizing = 1;
        resize_target = win;
        set_focus_win(win_focus);
        return 1;
    }
    if (!win->maximized && gui_point_in_titlebar(mouse_x, mouse_y, win)) {
        dragging = 1;
        drag_target = win;
        set_focus_win(win_focus);
        return 1;
    }
    if (gui_point_in_rect(mouse_x, mouse_y, win->x, win->y, win->w, win->h)) {
        set_focus_win(win_focus);
        return 1;
    }
    return 0;
}

static void gui_step(void) {
    rtc_uptime_tick();

    if (theme == GUI_THEME_WIN11_LIGHT || theme == GUI_THEME_WIN11_DARK) {
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

    int handled_something = 0;

    int c = keyboard_poll();
    if (c != -1) {
        handled_something = 1;

        if (c == '\t') {
            if (focus == FOCUS_TERMINAL && set_win.open) set_focus_win(FOCUS_SETTINGS);
            else if (focus == FOCUS_SETTINGS && term_win.open) set_focus_win(FOCUS_TERMINAL);
            else if (term_win.open) set_focus_win(FOCUS_TERMINAL);
            else if (set_win.open)  set_focus_win(FOCUS_SETTINGS);
        } else if (focus == FOCUS_TERMINAL) {
            terminal_app_handle_char(&term, (char)c);
        } else if (focus == FOCUS_SETTINGS) {
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
                resize_target->w += mev.dx;
                resize_target->h += mev.dy;
                if (resize_target->w < MIN_WIN_W) resize_target->w = MIN_WIN_W;
                if (resize_target->h < MIN_WIN_H) resize_target->h = MIN_WIN_H;
            } else if (start_menu_open) {
                int ix, iy, iw, ih;
                gui_start_menu_item_rect(screen_w, screen_h, 0, &ix, &iy, &iw, &ih);
                if (gui_point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                    term_win.open = 1;
                    term_win.minimized = 0;
                    set_focus_win(FOCUS_TERMINAL);
                } else {
                    gui_start_menu_item_rect(screen_w, screen_h, 1, &ix, &iy, &iw, &ih);
                    if (gui_point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                        set_win.open = 1;
                        set_win.minimized = 0;
                        set_focus_win(FOCUS_SETTINGS);
                    }
                }
                start_menu_open = 0;
            } else {
                int tb_count = (term_win.open ? 1 : 0) + (set_win.open ? 1 : 0);
                int sx, sy, sw, sh;
                gui_start_button_rect(screen_w, screen_h, tb_count, &sx, &sy, &sw, &sh);

                int term_tb_index = -1, set_tb_index = -1;
                int idx = 0;
                if (term_win.open) term_tb_index = idx++;
                if (set_win.open)  set_tb_index  = idx++;

                int term_tb_x = 0, term_tb_y = 0, term_tb_w = 0, term_tb_h = 0;
                int set_tb_x = 0, set_tb_y = 0, set_tb_w = 0, set_tb_h = 0;
                if (term_tb_index >= 0)
                    gui_taskbar_button_rect(screen_w, screen_h, term_tb_index, tb_count,
                                            &term_tb_x, &term_tb_y, &term_tb_w, &term_tb_h);
                if (set_tb_index >= 0)
                    gui_taskbar_button_rect(screen_w, screen_h, set_tb_index, tb_count,
                                            &set_tb_x, &set_tb_y, &set_tb_w, &set_tb_h);

                if (gui_point_in_rect(mouse_x, mouse_y, sx, sy, sw, sh)) {
                    start_menu_open = 1;

                } else if (term_tb_index >= 0 &&
                           gui_point_in_rect(mouse_x, mouse_y, term_tb_x, term_tb_y, term_tb_w, term_tb_h)) {
                    if (term_win.minimized) {
                        term_win.minimized = 0;
                        set_focus_win(FOCUS_TERMINAL);
                    } else if (focus == FOCUS_TERMINAL) {
                        term_win.minimized = 1;
                    } else {
                        set_focus_win(FOCUS_TERMINAL);
                    }

                } else if (set_tb_index >= 0 &&
                           gui_point_in_rect(mouse_x, mouse_y, set_tb_x, set_tb_y, set_tb_w, set_tb_h)) {
                    if (set_win.minimized) {
                        set_win.minimized = 0;
                        set_focus_win(FOCUS_SETTINGS);
                    } else if (focus == FOCUS_SETTINGS) {
                        set_win.minimized = 1;
                    } else {
                        set_focus_win(FOCUS_SETTINGS);
                    }

                } else if (focus == FOCUS_TERMINAL) {
                    if (!handle_window_click(&term_win, FOCUS_TERMINAL, FOCUS_SETTINGS, &set_win))
                        handle_window_click(&set_win, FOCUS_SETTINGS, FOCUS_TERMINAL, &term_win);
                } else {
                    if (!handle_window_click(&set_win, FOCUS_SETTINGS, FOCUS_TERMINAL, &term_win))
                        handle_window_click(&term_win, FOCUS_TERMINAL, FOCUS_SETTINGS, &set_win);
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

/* Вызывается из сетевых циклов ожидания. Возвращает не 0, если терминал
 * получил Ctrl+C — тогда команда прерывается. */
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

    term_win.x = 8;   term_win.y = 8;  term_win.w = 150; term_win.h = 150;
    set_win.x  = 165; set_win.y = 8;   set_win.w  = 140; set_win.h  = 150;
    term_win.title = "TERMINAL";
    set_win.title  = "SETTINGS";
    term_win.focused = 0;
    set_win.focused  = 0;
    term_win.open = 0;    /* рабочий стол при старте чист */
    set_win.open  = 0;
    term_win.minimized = 0; set_win.minimized = 0;
    term_win.maximized = 0; set_win.maximized = 0;

    terminal_app_init(&term, &term_win);
    settings_app_init(&settings, &set_win, &desktop_bg);
    apply_theme();

    render_frame();

    terminal_net_install(gui_request_redraw, net_pump_cb);

    for (;;)
        gui_step();
}
