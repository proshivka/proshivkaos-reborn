/* gui/utf8.h — разбор и сборка UTF-8 для приложений тач-оболочки.
 *
 * Текст в RamFS — байты, и файл с кириллицей на диске — обычный UTF-8.
 * Редактору и просмотрщику нужно ходить по нему СИМВОЛАМИ, а не байтами:
 * каретка не должна вставать посреди буквы, а Backspace — стирать
 * половину. В отличие от font_next_cp() (gui/font.c) здесь учитывается
 * длина буфера: файл в RamFS не оканчивается нулём.
 */
#ifndef GUI_TOUCH_UTF8_H
#define GUI_TOUCH_UTF8_H

#include <stdint.h>

#define UTF8_BAD 0xFFFDu   /* символ-заменитель: рисуется как '?' */

/* Разобрать символ, начинающийся с байта i. Возвращает его длину в байтах
 * (всегда >= 1, даже на битых данных — тогда cp = UTF8_BAD и длина 1). */
static inline int utf8_decode(const char *b, int len, int i, uint32_t *cp) {
    unsigned char c = (unsigned char)b[i];
    int n;
    uint32_t v;

    if (c < 0x80)                { *cp = c; return 1; }
    else if (c >= 0xC2 && c <= 0xDF) { n = 1; v = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; v = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; v = c & 0x07; }
    else                         { *cp = UTF8_BAD; return 1; }

    if (i + n > len - 1) { *cp = UTF8_BAD; return 1; }   /* обрезан буфером */
    for (int k = 1; k <= n; k++) {
        unsigned char cc = (unsigned char)b[i + k];
        if ((cc & 0xC0) != 0x80) { *cp = UTF8_BAD; return 1; }
        v = (v << 6) | (cc & 0x3F);
    }
    *cp = v;
    return n + 1;
}

/* Записать cp в out (до 3 байт; символы за пределами BMP не поддержаны).
 * Возвращает число байт, 0 — если символ не записать. */
static inline int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    return 0;
}

/* Начало символа, стоящего перед байтом i (i > 0). */
static inline int utf8_prev(const char *b, int len, int i) {
    int j = i - 1, k = 0;
    while (j > 0 && k < 3 && ((unsigned char)b[j] & 0xC0) == 0x80) { j--; k++; }
    uint32_t cp;
    if (utf8_decode(b, len, j, &cp) != i - j) return i - 1;   /* битые данные */
    return j;
}

#endif
