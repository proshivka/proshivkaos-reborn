/* gui/gconsole.c */
#include "gconsole.h"
#include "utf8.h"

static void push_new_line(gconsole_t *gc) {
    if (gc->line_count < GC_MAX_LINES) {
        gc->line_count++;
    } else {
        /* скроллбек заполнен — сдвигаем всё на одну строку вверх,
           теряем самую старую (как реальный терминал с ограниченной
           историей) */
        for (int i = 1; i < GC_MAX_LINES; i++) {
            gc->line_len[i - 1] = gc->line_len[i];
            for (int j = 0; j < gc->line_len[i]; j++) {
                gc->lines[i - 1][j] = gc->lines[i][j];
                gc->color[i - 1][j] = gc->color[i][j];
            }
        }
    }
    gc->line_len[gc->line_count - 1] = 0;
}

void gconsole_init(gconsole_t *gc, const gui_window_t *win, uint8_t fg, uint8_t bg) {
    gc->win = win;
    gc->fg = fg;
    gc->bg = bg;
    gc->ink = fg;
    gc->use_rect = 0;
    gc->rect_x = gc->rect_y = gc->rect_w = gc->rect_h = 0;
    gc->scale = 1;
    gc->shadow_valid = 0;
    gconsole_clear(gc);
}

void gconsole_set_rect(gconsole_t *gc, int x, int y, int w, int h) {
    gc->use_rect = 1;
    gc->rect_x = x; gc->rect_y = y;
    gc->rect_w = w; gc->rect_h = h;
}

void gconsole_set_scale(gconsole_t *gc, int scale) {
    gc->scale = (scale < 1) ? 1 : scale;
}

/* Единая точка получения области вывода: окно или явный прямоугольник. */
static void content_rect(const gconsole_t *gc, int *x, int *y, int *w, int *h) {
    if (gc->use_rect) {
        *x = gc->rect_x; *y = gc->rect_y;
        *w = gc->rect_w; *h = gc->rect_h;
    } else {
        gui_window_content_rect(gc->win, x, y, w, h);
    }
}

int gconsole_cols(const gconsole_t *gc) {
    int cx, cy, cw, ch;
    content_rect(gc, &cx, &cy, &cw, &ch);
    int cols = cw / hal_gfx_cell_w(gc->scale);
    return cols < 1 ? 1 : cols;
}

void gconsole_invalidate(gconsole_t *gc) { gc->shadow_valid = 0; }
int  gconsole_is_valid(const gconsole_t *gc) { return gc->shadow_valid; }

void gconsole_clear(gconsole_t *gc) {
    gc->line_count = 1;
    gc->line_len[0] = 0;
    gc->u8_left = 0;
}

void gconsole_set_colors(gconsole_t *gc, uint8_t fg, uint8_t bg) {
    gc->fg = fg;
    gc->bg = bg;
    gc->ink = fg;
}

void gconsole_set_ink(gconsole_t *gc, uint8_t color) {
    gc->ink = color;
}

void gconsole_putcp(gconsole_t *gc, uint32_t cp) {
    if (cp == '\n') {
        push_new_line(gc);
        return;
    }
    if (cp == '\b') {
        /* Одна клетка = один символ, как бы много байт он ни занимал. */
        int i = gc->line_count - 1;
        if (gc->line_len[i] > 0) gc->line_len[i]--;
        return;
    }
    if (cp > 0xFFFF) cp = UTF8_BAD;            /* за пределами BMP шрифта нет */

    int i = gc->line_count - 1;
    if (gc->line_len[i] >= GC_MAX_LINE_LEN) {
        push_new_line(gc);
        i = gc->line_count - 1;
    }
    int pos = gc->line_len[i]++;
    gc->lines[i][pos] = (uint16_t)cp;
    gc->color[i][pos] = gc->ink;
}

/* Поток байтов -> символы. Битая последовательность даёт один символ-
 * заменитель (рисуется как "?"), а байт, который её оборвал, разбирается
 * заново — так мусор в выводе не съедает следующий нормальный символ. */
void gconsole_putc(gconsole_t *gc, char c) {
    unsigned char b = (unsigned char)c;

    if (gc->u8_left) {
        if ((b & 0xC0) == 0x80) {
            gc->u8_cp = (gc->u8_cp << 6) | (b & 0x3F);
            if (--gc->u8_left == 0) gconsole_putcp(gc, gc->u8_cp);
            return;
        }
        gc->u8_left = 0;
        gconsole_putcp(gc, UTF8_BAD);
    }

    if (b < 0x80)                    gconsole_putcp(gc, b);
    else if (b >= 0xC2 && b <= 0xDF) { gc->u8_cp = b & 0x1F; gc->u8_left = 1; }
    else if (b >= 0xE0 && b <= 0xEF) { gc->u8_cp = b & 0x0F; gc->u8_left = 2; }
    else if (b >= 0xF0 && b <= 0xF4) { gc->u8_cp = b & 0x07; gc->u8_left = 3; }
    else                             gconsole_putcp(gc, UTF8_BAD);
}

void gconsole_write(gconsole_t *gc, const char *s) {
    while (*s) gconsole_putc(gc, *s++);
}

/* Сколько "визуальных" строк займёт логическая строка длины len при
   переносе по cols символов в ряд (минимум 1 — даже пустая строка
   занимает одну визуальную строку). */
static int wrapped_row_count(int len, int cols) {
    if (len <= 0) return 1;
    return (len + cols - 1) / cols;
}

/* Нарисовать клетку, если она отличается от того, что уже на экране. */
static void put_cell(gconsole_t *gc, int use_shadow, int full,
                     int cx, int cy, int cell_w, int cell_h, int scale,
                     int r, int c, uint16_t ch, uint8_t fg) {
    if (use_shadow && !full &&
        gc->shadow_ch[r][c] == ch &&
        (ch == ' ' || gc->shadow_fg[r][c] == fg))
        return;                         /* на экране уже это */

    /* Пробел на свежезалитом фоне рисовать незачем: это ровно фон. */
    if (!(full && ch == ' '))
        hal_gfx_draw_cp_scaled(cx + c * cell_w, cy + r * cell_h, ch, fg, gc->bg, scale);

    if (use_shadow) {
        gc->shadow_ch[r][c] = ch;
        gc->shadow_fg[r][c] = fg;
    }
}

void gconsole_render(gconsole_t *gc) {
    int cx, cy, cw, ch;
    content_rect(gc, &cx, &cy, &cw, &ch);

    int scale = (gc->scale < 1) ? 1 : gc->scale;
    int cell_w = hal_gfx_cell_w(scale);
    int cell_h = FONT_H * scale;

    int cols = cw / cell_w;
    int rows = ch / cell_h;
    if (cols < 1 || rows < 1) return;   /* область слишком мала — рисовать нечего */

    int use_shadow = (cols <= GC_SHADOW_COLS && rows <= GC_SHADOW_ROWS);

    /* Полная отрисовка — когда тень не совпадает с экраном: первый раз,
       после чужой отрисовки поверх, при смене размера, масштаба или
       фона. Во всех остальных случаях рисуются только расхождения. */
    int full = !use_shadow || !gc->shadow_valid ||
               gc->shadow_x != cx || gc->shadow_y != cy ||
               gc->shadow_cols != cols || gc->shadow_rows != rows ||
               gc->shadow_scale != scale || gc->shadow_bg != gc->bg;

    if (full) {
        hal_gfx_fill_rect(cx, cy, cols * cell_w, rows * cell_h, gc->bg);
        if (use_shadow) {
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    gc->shadow_ch[r][c] = ' ';
                    gc->shadow_fg[r][c] = gc->bg;
                }
            gc->shadow_x = cx;       gc->shadow_y = cy;
            gc->shadow_cols = cols;  gc->shadow_rows = rows;
            gc->shadow_scale = scale; gc->shadow_bg = gc->bg;
            gc->shadow_valid = 1;
        }
    }

    /* проход 1: считаем, сколько всего визуальных строк дал бы весь
       скроллбек при текущей ширине cols */
    int total_visual = 0;
    for (int i = 0; i < gc->line_count; i++)
        total_visual += wrapped_row_count(gc->line_len[i], cols);

    int skip = (total_visual > rows) ? (total_visual - rows) : 0;

    /* проход 2: "хвост" — последние rows визуальных строк, клетка за
       клеткой, вместе с пустыми хвостами строк: иначе стёртый символ
       остался бы на экране */
    int counter = 0;
    int emitted = 0;

    for (int i = 0; i < gc->line_count && emitted < rows; i++) {
        int len = gc->line_len[i];
        int chunks = wrapped_row_count(len, cols);

        for (int k = 0; k < chunks && emitted < rows; k++) {
            if (counter >= skip) {
                int start = k * cols;
                int chunk_len = len - start;
                if (chunk_len > cols) chunk_len = cols;
                if (chunk_len < 0) chunk_len = 0;

                for (int c = 0; c < cols; c++) {
                    if (c < chunk_len)
                        put_cell(gc, use_shadow, full, cx, cy, cell_w, cell_h, scale,
                                 emitted, c, gc->lines[i][start + c],
                                 gc->color[i][start + c]);
                    else
                        put_cell(gc, use_shadow, full, cx, cy, cell_w, cell_h, scale,
                                 emitted, c, ' ', gc->bg);
                }
                emitted++;
            }
            counter++;
        }
    }

    /* Строки ниже последней — пустые. */
    for (int r = emitted; r < rows; r++)
        for (int c = 0; c < cols; c++)
            put_cell(gc, use_shadow, full, cx, cy, cell_w, cell_h, scale, r, c, ' ', gc->bg);
}
