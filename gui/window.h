/* gui/window.h — оконный хром, панель задач и меню "Пуск".
 * Поддерживает два семейства тем:
 *   - Windows XP (Luna): светлая и тёмная (классический градиент + глянец)
 *   - Windows 11 (Fluent / Mica): светлая и тёмная (мягкие тени, скругления,
 *     центрированная панель задач с логотипом Win11 и часами, плавающее меню Пуск).
 */
#ifndef PROSHIVKAOS_WINDOW_H
#define PROSHIVKAOS_WINDOW_H

#include "hal_gfx.h"

#define TITLE_BAR_H  10
#define BORDER       2
#define TASKBAR_H    14
#define RESIZE_GRIP  6
#define MIN_WIN_W    60
#define MIN_WIN_H    40

#define GUI_THEME_XP_LIGHT    0
#define GUI_THEME_XP_DARK     1
#define GUI_THEME_WIN11_LIGHT 2
#define GUI_THEME_WIN11_DARK  3

/* Индексы палитры, выделяемые под интерфейс Windows 11 (слоты 32..46,
 * при этом 47 остаётся GFX_UI_SHADOW). */
enum {
    GFX_W11_LIGHT_BG   = 32,
    GFX_W11_LIGHT_TERM = 33,
    GFX_W11_LIGHT_TEXT = 34,
    GFX_W11_LIGHT_DIM  = 35,
    GFX_W11_DARK_BG    = 36,
    GFX_W11_DARK_TERM  = 37,
    GFX_W11_DARK_TEXT  = 38,
    GFX_W11_DARK_DIM   = 39,
    GFX_W11_ACCENT     = 40,
    GFX_W11_ACCENT_LT  = 41,
    GFX_W11_TASKBAR_LT = 42,
    GFX_W11_TASKBAR_DK = 43,
    GFX_W11_BORDER_LT  = 44,
    GFX_W11_BORDER_DK  = 45,
    GFX_W11_CLOSE_RED  = 46
};

typedef struct {
    int x, y, w, h;
    const char *title;
    int focused;
    int open;                /* 0 = окно скрыто (не открыто через Пуск) */
    int minimized;           /* 1 = свёрнуто (не рисуется, но есть в панели задач) */
    int maximized;           /* 1 = развёрнуто на весь экран */
    int saved_x, saved_y, saved_w, saved_h;   /* геометрия до разворота — для восстановления */
} gui_window_t;

void gui_set_theme(int theme);
void gui_set_scale(int scale);
void gui_set_cursor_pos(int x, int y);
int  gui_taskbar_height(int screen_h, int theme);

void gui_draw_desktop(uint8_t bg_color);
void gui_draw_window_chrome(const gui_window_t *win, int dark_theme);
void gui_draw_taskbar(int screen_w, int screen_h, int tb_count, int start_open,
                      int hour, int min, int ru_layout);
void gui_clock_rect(int screen_w, int screen_h, int *x, int *y, int *w, int *h);

/* Всплывающий календарь над часами */
void gui_draw_calendar(int screen_w, int screen_h, int year, int month,
                       int today_y, int today_m, int today_d, int dark_theme);
int  gui_calendar_handle_click(int screen_w, int screen_h, int mx, int my,
                               int *year, int *month);

/* Область содержимого окна (внутри рамки и под title bar) —
 * приложение рисует/пишет текст именно сюда. */
void gui_window_content_rect(const gui_window_t *win, int *cx, int *cy, int *cw, int *ch);

/* Хит-тесты для мыши */
int gui_point_in_rect(int px, int py, int rx, int ry, int rw, int rh);
int gui_point_in_titlebar(int px, int py, const gui_window_t *win);
int gui_point_in_close_button(int px, int py, const gui_window_t *win);
int gui_point_in_maximize_button(int px, int py, const gui_window_t *win);
int gui_point_in_minimize_button(int px, int py, const gui_window_t *win);
int gui_point_in_resize_grip(int px, int py, const gui_window_t *win);

/* Кнопка "Пуск" в панели задач */
void gui_start_button_rect(int screen_w, int screen_h, int tb_count, int *x, int *y, int *w, int *h);

/* Кнопки открытых окон в панели задач */
#define TASKBAR_BUTTON_W 60
void gui_taskbar_button_rect(int screen_w, int screen_h, int index, int tb_count,
                             int *x, int *y, int *w, int *h);
void gui_draw_taskbar_button(int screen_w, int screen_h, int index, int tb_count,
                             const char *title, int icon_id, int active, int dark_theme);

/* Всплывающее меню "Пуск": 4 пункта */
#define START_MENU_ITEMS 4
void gui_draw_start_menu(int screen_w, int screen_h, int dark_theme);
void gui_start_menu_item_rect(int screen_w, int screen_h, int index, int *x, int *y, int *w, int *h);

#endif
