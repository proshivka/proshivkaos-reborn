/* arch/x86/vga.c — вывод текста в буфер 0xB8000 (текстовый режим VGA 80x25) */
#include <stdint.h>
#include <stddef.h>
#include "ports.h"
#include "font8x16.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  ((volatile uint16_t *)0xB8000)
#define VGA_DEFAULT_COLOR   0x0F   /* белый на чёрном */

/* Порты CRT-контроллера — сдвигают аппаратный текстовый курсор (мигающий
 * подчёркивание/блок), а не просто меняют то, что мы сами рисуем в буфер.
 * Нужно полноэкранным приложениям (apps/editor.c), где курсор — это
 * реальная позиция редактирования, а не просто "куда пишет putc дальше". */
#define VGA_CRTC_INDEX_PORT 0x3D4
#define VGA_CRTC_DATA_PORT  0x3D5
#define VGA_CRTC_CURSOR_HIGH 0x0E
#define VGA_CRTC_CURSOR_LOW  0x0F

static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_current_color = VGA_DEFAULT_COLOR;

/* hal_console_putc передаёт UTF-8 по одному байту за вызов. */
static uint32_t utf8_codepoint;
static uint32_t utf8_min_codepoint;
static uint8_t utf8_remaining;

static void vga_update_hw_cursor(void);
static void vga_scroll(void);

static inline uint16_t vga_entry(uint8_t c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

#define VGA_SEQ_INDEX 0x3C4
#define VGA_SEQ_DATA  0x3C5
#define VGA_GC_INDEX  0x3CE
#define VGA_GC_DATA   0x3CF
#define VGA_FONT_MEM  ((volatile uint8_t *)0xA0000)

static uint8_t vga_read_seq(uint8_t index) {
    outb(VGA_SEQ_INDEX, index);
    return inb(VGA_SEQ_DATA);
}

static void vga_write_seq(uint8_t index, uint8_t value) {
    outb(VGA_SEQ_INDEX, index);
    outb(VGA_SEQ_DATA, value);
}

static uint8_t vga_read_gc(uint8_t index) {
    outb(VGA_GC_INDEX, index);
    return inb(VGA_GC_DATA);
}

static void vga_write_gc(uint8_t index, uint8_t value) {
    outb(VGA_GC_INDEX, index);
    outb(VGA_GC_DATA, value);
}

/* Загрузка 256 глифов в plane 2 VGA. Текстовый режим выделяет 32 байта
 * на знак; шрифт 8x16 занимает первые 16 байт каждого слота. */
static void vga_load_font(void) {
    uint8_t old_seq0 = vga_read_seq(0);
    uint8_t old_seq2 = vga_read_seq(2);
    uint8_t old_seq4 = vga_read_seq(4);
    uint8_t old_gc4 = vga_read_gc(4);
    uint8_t old_gc5 = vga_read_gc(5);
    uint8_t old_gc6 = vga_read_gc(6);

    vga_write_seq(0, 0x01); /* reset */
    vga_write_seq(2, 0x04); /* только plane 2 */
    vga_write_seq(4, 0x07); /* sequential, chain-4 off */
    vga_write_gc(4, 0x02);  /* read plane 2 */
    vga_write_gc(5, 0x00);  /* write mode 0 */
    vga_write_gc(6, 0x04);  /* A0000..AFFFF */

    for (unsigned glyph = 0; glyph < 256; glyph++) {
        volatile uint8_t *dst = VGA_FONT_MEM + glyph * 32u;
        for (unsigned row = 0; row < 16; row++) dst[row] = g_vga_font[glyph][row];
        for (unsigned row = 16; row < 32; row++) dst[row] = 0;
    }

    vga_write_gc(6, old_gc6);
    vga_write_gc(5, old_gc5);
    vga_write_gc(4, old_gc4);
    vga_write_seq(4, old_seq4);
    vga_write_seq(2, old_seq2);
    /* Обе таблицы символов должны ссылаться на загруженный банк 0. */
    vga_write_seq(3, 0x00);
    vga_write_seq(0, old_seq0);
}

static uint8_t vga_glyph_for_codepoint(uint32_t cp) {
    unsigned lo = 0, hi = g_vga_glyph_map_len;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (g_vga_glyph_map[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_vga_glyph_map_len && g_vga_glyph_map[lo].cp == cp)
        return g_vga_glyph_map[lo].slot;
    return (uint8_t)'?';
}

static void vga_put_glyph(uint32_t cp) {
    if (cp == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (cp == '\r') {
        vga_col = 0;
    } else if (cp == '\b') {
        if (vga_col == 0 && vga_row > 0) {
            vga_row--;
            vga_col = VGA_WIDTH;
        }
        if (vga_col > 0) {
            vga_col--;
            VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_current_color);
        }
    } else if (cp == '\t') {
        unsigned spaces = 8u - (unsigned)(vga_col & 7u);
        while (spaces--) {
            VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_current_color);
            vga_col++;
            if (vga_col >= VGA_WIDTH) { vga_col = 0; vga_row++; }
            if (vga_row >= VGA_HEIGHT) vga_scroll();
        }
    } else if (cp >= 0x20 && cp != 0x7F) {
        uint8_t slot = vga_glyph_for_codepoint(cp);
        VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(slot, vga_current_color);
        vga_col++;
        if (vga_col >= VGA_WIDTH) { vga_col = 0; vga_row++; }
    }

    if (vga_row >= VGA_HEIGHT) vga_scroll();
    vga_update_hw_cursor();
}

static void vga_put_utf8_byte(uint8_t byte) {
again:
    if (utf8_remaining) {
        if ((byte & 0xC0u) == 0x80u) {
            utf8_codepoint = (utf8_codepoint << 6) | (byte & 0x3Fu);
            if (--utf8_remaining == 0) {
                uint32_t cp = utf8_codepoint;
                if (cp < utf8_min_codepoint || cp > 0x10FFFFu ||
                    (cp >= 0xD800u && cp <= 0xDFFFu)) cp = '?';
                vga_put_glyph(cp);
            }
            return;
        }
        utf8_remaining = 0;
        utf8_codepoint = 0;
        utf8_min_codepoint = 0;
        vga_put_glyph('?');
        goto again;
    }

    if (byte < 0x80u) {
        vga_put_glyph(byte);
    } else if (byte >= 0xC2u && byte <= 0xDFu) {
        utf8_codepoint = byte & 0x1Fu;
        utf8_min_codepoint = 0x80u;
        utf8_remaining = 1;
    } else if (byte >= 0xE0u && byte <= 0xEFu) {
        utf8_codepoint = byte & 0x0Fu;
        utf8_min_codepoint = 0x800u;
        utf8_remaining = 2;
    } else if (byte >= 0xF0u && byte <= 0xF4u) {
        utf8_codepoint = byte & 0x07u;
        utf8_min_codepoint = 0x10000u;
        utf8_remaining = 3;
    } else {
        vga_put_glyph('?');
    }
}

static void vga_update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    outb(VGA_CRTC_INDEX_PORT, VGA_CRTC_CURSOR_LOW);
    outb(VGA_CRTC_DATA_PORT, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX_PORT, VGA_CRTC_CURSOR_HIGH);
    outb(VGA_CRTC_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_current_color = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_reset_color(void) {
    vga_current_color = VGA_DEFAULT_COLOR;
}

static void vga_scroll(void) {
    for (size_t r = 1; r < VGA_HEIGHT; r++)
        for (size_t c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[(r - 1) * VGA_WIDTH + c] = VGA_MEMORY[r * VGA_WIDTH + c];

    for (size_t c = 0; c < VGA_WIDTH; c++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = vga_entry(' ', vga_current_color);

    vga_row = VGA_HEIGHT - 1;
}

void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_current_color = VGA_DEFAULT_COLOR;
    utf8_codepoint = utf8_min_codepoint = 0;
    utf8_remaining = 0;
    vga_load_font();
    for (size_t r = 0; r < VGA_HEIGHT; r++)
        for (size_t c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[r * VGA_WIDTH + c] = vga_entry(' ', vga_current_color);
    vga_update_hw_cursor();
}

/* Полная очистка экрана + курсор в левый верхний угол. В отличие от
 * vga_init() ничего не трогает в состоянии цвета — только содержимое
 * экрана и позицию, как и положено "clear", а не "reinit". */
void vga_clear(void) {
    utf8_codepoint = utf8_min_codepoint = 0;
    utf8_remaining = 0;
    for (size_t r = 0; r < VGA_HEIGHT; r++)
        for (size_t c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[r * VGA_WIDTH + c] = vga_entry(' ', vga_current_color);
    vga_row = 0;
    vga_col = 0;
    vga_update_hw_cursor();
}

/* Произвольное позиционирование курсора — то, чего обычной последовательной
 * печати (vga_putc/vga_write) не хватает полноэкранным приложениям.
 * Значения вне экрана обрезаются, а не игнорируются: вызывающему (editor.c)
 * так проще — не нужно самому клампить каждый раз перед вызовом. */
void vga_set_cursor(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

    vga_row = (size_t)row;
    vga_col = (size_t)col;
    vga_update_hw_cursor();
}

void vga_putc(char c) {
    vga_put_utf8_byte((uint8_t)c);
}

void vga_write(const char *s) {
    while (*s)
        vga_putc(*s++);
}
