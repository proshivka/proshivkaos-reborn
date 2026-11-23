/* gui/touch/app_edit.c — текстовый редактор для тач-оболочки.
 *
 * Экран по макету: заголовок "EDIT: ИМЯ" со стрелкой назад, под ним поле
 * текста с кареткой, внизу — экранная клавиатуру рисует сама оболочка
 * (wants_keyboard), а кнопка SAVE пока зафиксирована посередине правого
 * края поля.
 *
 * Редактор работает с тем же RamFS, что и терминал и файловый менеджер:
 * файл целиком читается в буфер при открытии и целиком пишется обратно по
 * кнопке SAVE. Пока не нажали SAVE, файл не меняется, а в заголовке у имени
 * стоит звёздочка.
 *
 * Файл — UTF-8, в буфере лежат его байты как есть, а ходить по ним
 * приходится символами (utf8.h): каретка не встаёт посреди буквы, Backspace
 * стирает букву целиком. Рисуются латиница и кириллица; всё, чего нет в
 * шрифте, и битые байты — знаком "?", но в файле остаются как были.
 *
 * Перенос строк жёсткий, по ширине поля, как в просмотрщике файлов: так
 * положение каретки и касания считается без измерения слов.
 */
#include "touch_app.h"
#include "icon.h"
#include "touch_theme.h"
#include "hal_input.h"
#include "ramfs.h"
#include "utf8.h"

#define DEFAULT_PATH "/note.txt"

static int g_x, g_y, g_w, g_h;

static char g_buf[RAMFS_MAX_FILE_SIZE];
static int  g_len = 0;
static int  g_cur = 0;                  /* позиция каретки: 0..g_len        */
static char g_path[RAMFS_MAX_PATH_LEN]; /* пусто — файл ещё не выбран        */

static int g_dirty       = 0;
static int g_save_failed = 0;
static int g_from_files  = 0;           /* назад — в файловый менеджер       */

static int g_scroll = 0;                /* прокрутка текста, px              */

/* Жест: отличить прокрутку от касания, как в app_files.c. */
static int g_drag_start_y = 0;
static int g_drag_start_scroll = 0;
static int g_drag_distance = 0;
static int g_pressed_save = 0;

/* ---------------- Мелочи ---------------- */

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

/* ---------------- Геометрия ---------------- */

/* Текст — не мельче 2-го масштаба (см. touch_text_scale): на масштабе 1
 * вместо сглаженного шрифта пиксельный 8x8, в котором кириллицы нет. */
static int text_scale(void) { return touch_text_scale(); }
static int line_h(void) { return FONT_H * text_scale(); }
static int char_w(void) { return hal_gfx_cell_w(text_scale()); }

static int text_x(void) { return g_x + TM.pad * 2; }

/* Верх текста: под заголовком и небольшим отступом. */
static int text_top(void) { return g_y + TM.touch + TM.pad; }

/* Высота видимой части поля с текстом. */
static int view_h(void) { return g_y + g_h - text_top(); }

static int columns(void) {
    int n = (g_w - TM.pad * 4) / char_w();
    return n < 1 ? 1 : n;
}

/* Строка и столбец (в клетках) символа с номером idx при переносе по ширине. */
static void pos_of(int idx, int *line, int *col) {
    int n = columns();
    int l = 0, c = 0;
    int i = 0;
    while (i < idx && i < g_len) {
        uint32_t cp;
        i += utf8_decode(g_buf, g_len, i, &cp);
        if (cp == '\n') { l++; c = 0; continue; }
        c++;
        if (c >= n) { c = 0; l++; }
    }
    *line = l;
    *col = c;
}

/* Обратное: какой индекс буфера стоит в клетке (tl, tc). Тап правее конца
 * строки ставит каретку в её конец, ниже текста — в конец файла. */
static int index_at(int tl, int tc) {
    int n = columns();
    int l = 0, c = 0;
    int i = 0;
    while (i < g_len) {
        if (l == tl && c >= tc) return i;
        uint32_t cp;
        int len = utf8_decode(g_buf, g_len, i, &cp);
        if (cp == '\n') {
            if (l == tl) return i;
            l++; c = 0;
            i += len;
            continue;
        }
        i += len;
        c++;
        if (c >= n) {
            c = 0; l++;
            if (l > tl) return i;
        }
    }
    return g_len;
}

static void clamp_scroll(void) {
    int line, col;
    pos_of(g_len, &line, &col);
    int content = (line + 1) * line_h();
    int max = content - view_h();
    if (max < 0) max = 0;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0)   g_scroll = 0;
}

/* После набора каретка не должна уезжать за край поля. */
static void scroll_to_cursor(void) {
    int line, col;
    pos_of(g_cur, &line, &col);
    int top = line * line_h();
    if (top < g_scroll) g_scroll = top;
    if (top + line_h() > g_scroll + view_h()) g_scroll = top + line_h() - view_h();
    clamp_scroll();
}

/* Кнопка SAVE — по макету: крупная, у правого края. По вертикали пока
 * зафиксирована посередине поля с текстом. */
static void save_rect(int *x, int *y, int *w, int *h) {
    *w = TM.touch * 10 / 3;
    *h = TM.touch * 3 / 2;
    *x = g_x + g_w - *w - TM.pad;
    int area_top = g_y + TM.touch;
    *y = area_top + (g_y + g_h - area_top - *h) / 2;
}

static void back_rect(int *x, int *y, int *w, int *h) {
    *x = g_x; *y = g_y; *w = TM.touch; *h = TM.touch;
}

/* ---------------- Файл ---------------- */

static void load_file(const char *path) {
    str_copy(g_path, path, sizeof(g_path));

    g_len = 0;
    const ramfs_file_t *f = ramfs_lookup(path);
    if (f && !f->is_dir) {
        int n = (int)f->size;
        if (n > RAMFS_MAX_FILE_SIZE) n = RAMFS_MAX_FILE_SIZE;
        for (int i = 0; i < n; i++) g_buf[i] = (char)f->data[i];
        g_len = n;
    }
    g_cur = g_len;
    g_dirty = 0;
    g_save_failed = 0;
    g_scroll = 0;
    scroll_to_cursor();
}

static void save_file(void) {
    /* Файла может ещё не быть (новый документ) — тогда создаём. */
    int rc = ramfs_touch(g_path);
    if (rc == 0) rc = ramfs_write(g_path, g_buf, (size_t)g_len);

    g_save_failed = (rc != 0);
    if (rc == 0) g_dirty = 0;
}

void app_edit_open(const char *path, int from_files) {
    g_from_files = from_files;

    /* Тот же файл с несохранёнными правками — не затираем их: человек мог
       выйти по кнопке "домой" и вернуться. */
    int same = (g_path[0] != '\0');
    if (same) {
        const char *a = g_path, *b = path;
        while (*a && *b && *a == *b) { a++; b++; }
        same = (*a == *b);
    }
    if (!(same && g_dirty)) load_file(path);

    touch_ui_open("EDIT");
}

/* ---------------- Правка ---------------- */

/* Вставить символ (код Юникода) в позицию каретки. */
static void insert_cp(uint32_t cp) {
    char enc[4];
    int n = utf8_encode(cp, enc);
    if (n == 0 || g_len + n > RAMFS_MAX_FILE_SIZE) return;

    for (int i = g_len - 1; i >= g_cur; i--) g_buf[i + n] = g_buf[i];
    for (int i = 0; i < n; i++) g_buf[g_cur + i] = enc[i];
    g_cur += n;
    g_len += n;
    g_dirty = 1;
    g_save_failed = 0;
}

/* Стереть символ перед кареткой целиком, со всеми его байтами. */
static void delete_before(void) {
    if (g_cur <= 0) return;
    int start = utf8_prev(g_buf, g_len, g_cur);
    int n = g_cur - start;
    for (int i = start; i + n < g_len; i++) g_buf[i] = g_buf[i + n];
    g_cur = start;
    g_len -= n;
    g_dirty = 1;
    g_save_failed = 0;
}

/* ---------------- Приложение ---------------- */

static void edit_init(void) {
    /* Запущен с домашнего экрана, а не из файлового менеджера: открываем
       заметку по умолчанию. Если файл уже выбран, ничего не трогаем. */
    if (!g_path[0]) {
        g_from_files = 0;
        load_file(DEFAULT_PATH);
    }
}

static void edit_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    clamp_scroll();
    scroll_to_cursor();   /* клавиатура могла появиться и закрыть каретку */
}

static void draw_header(void) {
    int hh = TM.touch;
    hal_gfx_fill_rect(g_x, g_y, g_w, hh, GFX_UI_SURFACE_2);
    hal_gfx_fill_rect(g_x, g_y + hh - 1, g_w, 1, GFX_UI_DIVIDER);

    int is = hh * 55 / 100;
    icon_draw(ICON_ARROW_BACK, g_x + (hh - is) / 2, g_y + (hh - is) / 2, is,
              hal_gfx_palette_rgb(GFX_UI_TEXT));

    char title[RAMFS_MAX_PATH_LEN + 8];
    int pos = touch_strcat(title, 0, sizeof(title), "EDIT: ");
    pos = touch_strcat(title, pos, sizeof(title), base_name(g_path));
    if (g_dirty) touch_strcat(title, pos, sizeof(title), "*");

    hal_gfx_draw_string_scaled(g_x + hh + TM.pad,
                                g_y + (hh - line_h()) / 2,
                                title, GFX_UI_TEXT, TM.scale_small);
}

static void draw_text(void) {
    int n  = columns();
    int cw = char_w();
    int lh = line_h();
    int x0 = text_x();
    int y0 = text_top() - g_scroll;
    int vis_top = g_y + TM.touch;
    int vis_bot = g_y + g_h;

    int line = 0, col = 0;
    int caret_line = 0, caret_col = 0;

    int i = 0;
    for (;;) {
        if (i == g_cur) { caret_line = line; caret_col = col; }
        if (i >= g_len) break;

        uint32_t cp;
        i += utf8_decode(g_buf, g_len, i, &cp);
        if (cp == '\n') { line++; col = 0; continue; }

        int py = y0 + line * lh;
        if (py >= vis_top && py + lh <= vis_bot) {
            uint32_t shown = (cp < 32 || cp == 127 || cp == UTF8_BAD) ? '?' : cp;
            if (shown != ' ')
                hal_gfx_draw_cp_scaled(x0 + col * cw, py, shown,
                                        GFX_UI_TEXT, GFX_UI_BG, text_scale());
        }
        col++;
        if (col >= n) { col = 0; line++; }
    }

    /* Каретка — тонкая вертикальная черта. */
    int cy = y0 + caret_line * lh;
    if (cy >= vis_top && cy + lh <= vis_bot) {
        int cx = x0 + caret_col * cw;
        int bw = text_scale() > 1 ? text_scale() : 2;
        hal_gfx_fill_rect(cx, cy, bw, lh, GFX_UI_TEXT);
    }
}

static void draw_save_button(void) {
    int x, y, w, h;
    save_rect(&x, &y, &w, &h);

    const char *label = g_save_failed ? "ERROR" : (g_dirty ? "SAVE" : "SAVED");

    uint8_t top = g_pressed_save ? GFX_UI_SURFACE   : GFX_UI_SURFACE_2;
    uint8_t bot = g_pressed_save ? GFX_UI_SURFACE_2 : GFX_UI_SURFACE;
    if (g_save_failed) { top = GFX_UI_WARN; bot = GFX_UI_SURFACE; }

    hal_gfx_draw_glossy_button(x, y, w, h, top, bot, GFX_UI_DIVIDER, TM.radius * 2);
    hal_gfx_draw_string_centered(x, y + (h - FONT_H * TM.scale) / 2, w, label,
                                  g_dirty || g_save_failed ? GFX_UI_TEXT_BRIGHT : GFX_UI_TEXT_DIM,
                                  TM.scale);
}

static void edit_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    draw_text();
    draw_header();          /* поверх текста: он не заходит под заголовок */
    draw_save_button();
}

static void leave(void) {
    if (g_from_files) touch_ui_open("FILES");
    else              touch_ui_go_home();
}

static void edit_on_touch(int type, int x, int y) {
    int sx, sy, sw, sh;
    save_rect(&sx, &sy, &sw, &sh);

    if (type == HAL_EV_POINTER_DOWN) {
        g_drag_start_y = y;
        g_drag_start_scroll = g_scroll;
        g_drag_distance = 0;
        g_pressed_save = touch_hit(x, y, sx, sy, sw, sh);
        return;
    }

    if (type == HAL_EV_POINTER_MOVE) {
        int dy = y - g_drag_start_y;
        if (dy < 0) dy = -dy;
        if (dy > g_drag_distance) g_drag_distance = dy;

        if (g_drag_distance > TM.touch / 3) {
            g_pressed_save = 0;
            g_scroll = g_drag_start_scroll - (y - g_drag_start_y);
            clamp_scroll();
        }
        return;
    }

    if (type != HAL_EV_POINTER_UP) return;

    int was_save = g_pressed_save;
    g_pressed_save = 0;

    /* Тянули — значит прокручивали. */
    if (g_drag_distance > TM.touch / 3) return;

    int bx, by, bw, bh;
    back_rect(&bx, &by, &bw, &bh);
    if (touch_hit(x, y, bx, by, bw, bh)) { leave(); return; }

    if (was_save && touch_hit(x, y, sx, sy, sw, sh)) { save_file(); return; }

    /* Тап по полю — переставить каретку. */
    if (y >= g_y + TM.touch && !was_save) {
        int cw = char_w();
        int tl = (y - text_top() + g_scroll);
        tl = tl < 0 ? 0 : tl / line_h();
        int tc = (x - text_x() + cw / 2);
        tc = tc < 0 ? 0 : tc / cw;
        g_cur = index_at(tl, tc);
    }
}

static void edit_on_key(int key) {
    if (key == '\b' || key == 127) {
        delete_before();
    } else if (key == '\n' || key == '\r') {
        insert_cp('\n');
    } else if ((key >= 32 && key < 127) || (key >= 0xA0 && key < 0x10000)) {
        insert_cp((uint32_t)key);
    } else {
        return;
    }
    scroll_to_cursor();
}

static int edit_wants_keyboard(void) { return 1; }

const touch_app_t app_edit = {
    .name   = "EDIT",
    .glyph  = "TXT",
    .icon   = ICON_FILE + 1,
    .color  = GFX_UI_ACCENT2,
    .color2 = GFX_UI_ACCENT2_DARK,
    .init   = edit_init,
    .layout = edit_layout,
    .render = edit_render,
    .on_touch = edit_on_touch,
    .on_key = edit_on_key,
    .wants_keyboard = edit_wants_keyboard
};
