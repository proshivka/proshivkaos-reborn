/* gui/touch/app_calc.c — калькулятор с крупными кнопками для тачскрина */

#include "touch_app.h"
#include "icon.h"
#include "touch_theme.h"
#include "hal_input.h"
#include "hal_gfx.h"

static int g_x, g_y, g_w, g_h;
static char g_expr[64] = "0";
static int  g_pos = 1;
static int  g_pressed_btn = -1;

#define ROWS 5
#define COLS 4

static const char *const g_btn_labels[ROWS][COLS] = {
    { "C", "(", ")", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", ".", "DEL", "=" }
};

static void calc_init(void) {
    g_expr[0] = '0';
    g_expr[1] = '\0';
    g_pos = 1;
    g_pressed_btn = -1;
}

static void calc_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
}

static void btn_rect(int r, int c, int *bx, int *by, int *bw, int *bh) {
    int top_margin = g_y + TM.touch * 3;
    int grid_h = g_h - TM.touch * 3 - TM.pad * 2;
    int slot_w = (g_w - TM.pad * 2) / COLS;
    int slot_h = grid_h / ROWS;

    *bw = slot_w - TM.gap;
    *bh = slot_h - TM.gap;
    *bx = g_x + TM.pad + c * slot_w + (slot_w - *bw) / 2;
    *by = top_margin + r * slot_h + (slot_h - *bh) / 2;
}

static void evaluate_simple(void) {
    /* Базовый парсер: первое число, операция, второе число */
    long num1 = 0, num2 = 0;
    char op = 0;
    char *p = g_expr;

    while (*p && (*p >= '0' && *p <= '9')) {
        num1 = num1 * 10 + (*p - '0');
        p++;
    }

    if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
        op = *p;
        p++;
        while (*p && (*p >= '0' && *p <= '9')) {
            num2 = num2 * 10 + (*p - '0');
            p++;
        }
    }

    long res = num1;
    if (op == '+') res = num1 + num2;
    else if (op == '-') res = num1 - num2;
    else if (op == '*') res = num1 * num2;
    else if (op == '/' && num2 != 0) res = num1 / num2;

    char num_buf[16];
    char *str = touch_itoa((int)res, num_buf, 0);
    int i = 0;
    while (str[i] && i < (int)sizeof(g_expr) - 1) {
        g_expr[i] = str[i];
        i++;
    }
    g_expr[i] = '\0';
    g_pos = i;
}

static void handle_click(const char *label) {
    if (label[0] == 'C' && label[1] == '\0') {
        g_expr[0] = '0';
        g_expr[1] = '\0';
        g_pos = 1;
    } else if (label[0] == 'D' && label[1] == 'E') {
        if (g_pos > 1) {
            g_pos--;
            g_expr[g_pos] = '\0';
        } else {
            g_expr[0] = '0';
            g_expr[1] = '\0';
            g_pos = 1;
        }
    } else if (label[0] == '=') {
        evaluate_simple();
    } else {
        if (g_pos == 1 && g_expr[0] == '0' && label[0] >= '0' && label[0] <= '9') {
            g_expr[0] = label[0];
        } else if (g_pos < (int)sizeof(g_expr) - 2) {
            g_expr[g_pos++] = label[0];
            g_expr[g_pos] = '\0';
        }
    }
}

static void calc_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    touch_draw_screen_header(g_x, g_y, g_w, "CALCULATOR");

    /* Поле ввода / результата */
    int disp_h = TM.touch * 2;
    int disp_x = g_x + TM.pad * 2;
    int disp_y = g_y + TM.touch + TM.pad;
    int disp_w = g_w - TM.pad * 4;

    touch_draw_card(disp_x, disp_y, disp_w, disp_h, GFX_UI_SURFACE);

    int tw = hal_gfx_string_width(g_expr, TM.scale + 1);
    hal_gfx_draw_string_scaled(disp_x + disp_w - TM.pad * 3 - tw,
                                disp_y + (disp_h - FONT_H * (TM.scale + 1)) / 2,
                                g_expr, GFX_UI_TEXT, TM.scale + 1);

    /* Сетка кнопок */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int bx, by, bw, bh;
            btn_rect(r, c, &bx, &by, &bw, &bh);

            int btn_idx = r * COLS + c;
            int pressed = (g_pressed_btn == btn_idx);
            const char *label = g_btn_labels[r][c];

            uint8_t top = GFX_UI_SURFACE_2;
            uint8_t bot = GFX_UI_SURFACE;

            if (c == 3 || (r == 4 && c == 3)) {
                top = GFX_UI_ACCENT;
                bot = GFX_UI_ACCENT_DARK;
            } else if (r == 0 && c == 0) {
                top = GFX_UI_WARN;
                bot = GFX_UI_SURFACE_2;
            }

            touch_draw_button(bx, by, bw, bh, label, pressed, top, bot);
        }
    }
}

static void calc_on_touch(int type, int x, int y) {
    if (type == HAL_EV_POINTER_DOWN) {
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                int bx, by, bw, bh;
                btn_rect(r, c, &bx, &by, &bw, &bh);
                if (touch_hit(x, y, bx, by, bw, bh)) {
                    g_pressed_btn = r * COLS + c;
                    return;
                }
            }
        }
    } else if (type == HAL_EV_POINTER_UP) {
        int bx, by, bw, bh;
        touch_header_back_rect(g_x, g_y, &bx, &by, &bw, &bh);
        if (touch_hit(x, y, bx, by, bw, bh)) {
            touch_ui_go_home();
            return;
        }

        if (g_pressed_btn >= 0) {
            int r = g_pressed_btn / COLS;
            int c = g_pressed_btn % COLS;
            handle_click(g_btn_labels[r][c]);
            g_pressed_btn = -1;
        }
    }
}

static void calc_on_key(int key) {
    if (key == 27) touch_ui_go_home();
    else if (key >= '0' && key <= '9') {
        char s[2] = { (char)key, '\0' };
        handle_click(s);
    } else if (key == '+' || key == '-' || key == '*' || key == '/') {
        char s[2] = { (char)key, '\0' };
        handle_click(s);
    } else if (key == '=' || key == '\n') {
        handle_click("=");
    } else if (key == '\b') {
        handle_click("DEL");
    }
}

static int calc_wants_keyboard(void) { return 0; }

const touch_app_t app_calc = {
    .name  = "CALC",
    .glyph = "+-=",
    .icon  = 0,
    .color = GFX_UI_WARN,
    .color2 = GFX_UI_SURFACE_2,
    .init  = calc_init,
    .layout = calc_layout,
    .render = calc_render,
    .invalidate = 0,
    .on_touch = calc_on_touch,
    .on_key = calc_on_key,
    .wants_keyboard = calc_wants_keyboard,
    .tick  = 0
};
