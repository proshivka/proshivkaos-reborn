/* gui/png.h — декодер PNG без зависимостей от libc и динамической памяти.
 *
 * Поддерживает стандартные PNG (RFC 2083):
 *   - цветовые типы 0 (Gray), 2 (RGB), 3 (Indexed/PLTE), 4 (Gray+Alpha), 6 (RGBA);
 *   - глубины 1, 2, 4, 8, 16 бит на канал;
 *   - прозрачность tRNS и альфа-канал;
 *   - сжатие zlib/DEFLATE (stored, fixed Huffman, dynamic Huffman);
 *   - все 5 PNG-фильтров (None, Sub, Up, Average, Paeth);
 *   - чересстрочность Adam7 (interlace = 1).
 *
 * Выходной формат пикселей — 0xAARRGGBB (alpha в старшем байте).
 */
#ifndef GUI_PNG_H
#define GUI_PNG_H

#include <stddef.h>
#include <stdint.h>

#define PNG_MAX_WIDTH   1024u
#define PNG_MAX_HEIGHT  1024u

enum {
    PNG_OK            =  0,
    PNG_ERR_BAD_SIG   = -1,
    PNG_ERR_BAD_HDR   = -2,
    PNG_ERR_TOO_LARGE = -3,
    PNG_ERR_TRUNCATED = -4,
    PNG_ERR_DEFLATE   = -5,
    PNG_ERR_FORMAT    = -6
};

enum {
    PNG_COLOR_GRAY       = 0,
    PNG_COLOR_RGB        = 2,
    PNG_COLOR_INDEXED    = 3,
    PNG_COLOR_GRAY_ALPHA = 4,
    PNG_COLOR_RGBA       = 6
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  bit_depth;
    uint8_t  color_type;
    uint8_t  interlace;
    uint8_t  has_alpha;
} png_info_t;

/* Быстрая проверка 8-байтовой сигнатуры PNG. */
int png_is_png(const uint8_t *data, size_t len);

/* Прочитать заголовок IHDR без декодирования пикселей. */
int png_probe(const uint8_t *data, size_t len, png_info_t *info);

/* Декодировать PNG в буфер out_argb размером не менее width * height uint32_t.
 * Если width > max_w или height > max_h, возвращает PNG_ERR_TOO_LARGE. */
int png_decode(const uint8_t *data, size_t len,
               uint32_t *out_argb, uint32_t max_w, uint32_t max_h,
               png_info_t *info);

const char *png_strerror(int err);
const char *png_color_name(uint8_t color_type);

#endif
