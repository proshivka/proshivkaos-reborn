/* arch/x86/font8x16.h — шрифт текстовой консоли VGA (8x16) с кириллицей.
 *
 * Текстовый режим VGA хранит знак как 16 байт по 8 бит. Таблица g_vga_font
 * содержит 256 знаков; какие из них какой букве соответствуют, описывает
 * g_vga_glyph_map — пара «код символа Unicode → слот шрифта». Штатный
 * шрифт BIOS кириллицы не имеет, поэтому vga.c загружает этот свой.
 *
 * Таблица СГЕНЕРИРОВАНА tools/fontvga.py — править нужно там.
 */
#ifndef ARCH_X86_FONT8X16_H
#define ARCH_X86_FONT8X16_H

#include <stdint.h>

typedef struct {
    uint16_t cp;    /* код символа Unicode */
    uint8_t  slot;  /* слот в g_vga_font */
} vga_glyph_map_t;

extern const uint8_t        g_vga_font[256][16];
extern const vga_glyph_map_t g_vga_glyph_map[];
extern const unsigned        g_vga_glyph_map_len;

#endif
