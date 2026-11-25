#!/usr/bin/env python3
"""tools/fontvga.py — шрифт 8x16 для ТЕКСТОВОЙ консоли x86 (VGA).

Зачем отдельный генератор, если есть tools/fontgen.py. Тот делает сглаженные
маски для ГРАФИЧЕСКОГО интерфейса: пиксель там — байт прозрачности, а цвет
выбирается при отрисовке. Здесь наоборот: текстовый режим VGA хранит по
одному БИТУ на пиксель, а знак — это 16 байт по 8 бит. Сглаживать нечего,
зато знак обязан уложиться в клетку 8x16 ровно.

Штатный шрифт BIOS кириллицы не содержит вовсе: им набиралась латиница, а
русский текст в системе до сих пор писали транслитом. Здесь растеризуется
Roboto Mono в клетку 8x16 (ширина знака 8 px, базовая линия — 13-я строка
клетки сверху), и получается полный набор: ASCII, кириллица, типографские
знаки. arch/x86/vga.c загружает его в плоскость 2 видеопамяти.

Растеризация идёт с 4-кратным запасом и усреднением: у знака шириной ровно
8 px без этого «и» и «ш» слипаются в пятно, а с ним перемычки остаются
различимы.

Использование: python3 tools/fontvga.py > arch/x86/font8x16_data.c
Шрифт: third_party/fonts/RobotoMono.ttf (SIL Open Font License 1.1).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT, "third_party", "fonts", "RobotoMono.ttf")

PX = 13           # кегль: ширина знака ровно 8 px
CELL_W = 8
CELL_H = 16
BASE_ROW = 13     # сколько строк клетки над базовой линией
SS = 4            # суперсемплинг
THRESHOLD = 0.42  # доля закраски, при которой пиксель считается чёрным

# Набор знаков. Коды 0x00..0x7F занимают сами себя — любой байт ASCII
# рисуется как раньше, — а остальное раскладывается по свободным слотам
# начиная с 0x80 (см. g_vga_glyph_map в сгенерированном файле).
CHARS = (
    list(range(0x20, 0x7F)) +          # 95: печатный ASCII
    [0x401, 0x451] +                   # Ё ё
    list(range(0x410, 0x450)) +        # А..я
    [0xB0, 0x2022, 0x2116,             # ° • №
     0xAB, 0xBB,                       # « »
     0x2013, 0x2014,                   # – —
     0x201C, 0x201D, 0x2018, 0x2019]   # “ ” ‘ ’
)


def raster(font, ch, shift):
    """16 байт по 8 бит — знак, вписанный в клетку. Старший бит слева."""
    pad = 8
    w = h = (CELL_W + 2 * pad) * SS
    im = Image.new("L", (w, h), 0)
    ImageDraw.Draw(im).text((pad * SS, pad * SS + shift), ch, font=font, fill=255)

    out = []
    for row in range(CELL_H):
        bits = 0
        y0 = (pad + row) * SS
        for col in range(CELL_W):
            x0 = (pad + col) * SS
            s = 0
            for y in range(y0, y0 + SS):
                for x in range(x0, x0 + SS):
                    s += im.getpixel((x, y))
            if s >= THRESHOLD * SS * SS * 255:
                bits |= 0x80 >> col
        out.append(bits)
    return out


def main():
    font = ImageFont.truetype(FONT_PATH, PX * SS)
    ascent, _ = font.getmetrics()          # в масштабе SS
    shift = (BASE_ROW * SS) - ascent       # базовая линия -> строка BASE_ROW

    slots = {}
    nxt = 0x80
    for cp in CHARS:
        if cp < 0x80:
            slots[cp] = cp
        else:
            slots[cp] = nxt
            nxt += 1
    if nxt > 256:
        sys.exit("fontvga: набор не помещается в 256 знаков шрифта")

    glyphs = {slots[cp]: raster(font, chr(cp), shift) for cp in CHARS}

    out = sys.stdout
    out.write("/* arch/x86/font8x16_data.c — СГЕНЕРИРОВАНО tools/fontvga.py,\n")
    out.write(" * руками не править. Roboto Mono, SIL Open Font License 1.1\n")
    out.write(" * (third_party/fonts). Шрифт текстового режима VGA 8x16:\n")
    out.write(" * знак — 16 байт по 8 бит, старший бит слева. */\n")
    out.write("#include <stdint.h>\n")
    out.write('#include "font8x16.h"\n\n')

    out.write("const uint8_t g_vga_font[256][16] = {\n")
    for slot in range(256):
        g = glyphs.get(slot, [0] * 16)
        out.write("    {" + ",".join("0x%02X" % b for b in g) + "},\n")
    out.write("};\n\n")

    out.write("const vga_glyph_map_t g_vga_glyph_map[] = {\n")
    for cp in sorted(CHARS):
        out.write("    {0x%04X, 0x%02X},\n" % (cp, slots[cp]))
    out.write("};\n")
    out.write("const unsigned g_vga_glyph_map_len = %d;\n" % len(CHARS))


if __name__ == "__main__":
    main()
