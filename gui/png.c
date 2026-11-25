/* gui/png.c — freestanding-декодер PNG и DEFLATE (RFC 1951 / RFC 2083).
 *
 * Работает поверх одного статического рабочего буфера (без malloc/free и без
 * больших массивов на стеке), поэтому безопасен в bare-metal ядре с
 * ограниченным стеком. Декодирование идёт потоково: DEFLATE распаковывает
 * байты прямо в построчный буфер фильтрации и пишет готовые пиксели
 * 0xAARRGGBB в выходной буфер.
 */
#include "png.h"

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

/* ---------------- Вспомогательные функции ---------------- */

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t tag4(char a, char b, char c, char d) {
    return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) |
           ((uint32_t)(uint8_t)c << 8)  |  (uint32_t)(uint8_t)d;
}

int png_is_png(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (!data || len < 8) return 0;
    for (int i = 0; i < 8; i++)
        if (data[i] != sig[i]) return 0;
    return 1;
}

static int valid_ihdr(uint32_t w, uint32_t h, uint8_t depth, uint8_t ctype,
                      uint8_t comp, uint8_t filt, uint8_t interlace) {
    if (w == 0 || h == 0) return 0;
    if (comp != 0 || filt != 0 || interlace > 1) return 0;
    switch (ctype) {
    case PNG_COLOR_GRAY:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PNG_COLOR_RGB:
    case PNG_COLOR_GRAY_ALPHA:
    case PNG_COLOR_RGBA:
        return depth == 8 || depth == 16;
    case PNG_COLOR_INDEXED:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    default:
        return 0;
    }
}

static int channels_of(uint8_t ctype) {
    switch (ctype) {
    case PNG_COLOR_GRAY:       return 1;
    case PNG_COLOR_RGB:        return 3;
    case PNG_COLOR_INDEXED:    return 1;
    case PNG_COLOR_GRAY_ALPHA: return 2;
    case PNG_COLOR_RGBA:       return 4;
    default:                   return 0;
    }
}

int png_probe(const uint8_t *data, size_t len, png_info_t *info) {
    if (!png_is_png(data, len)) return PNG_ERR_BAD_SIG;
    if (len < 8 + 12 + 13) return PNG_ERR_TRUNCATED;

    uint32_t chunk_len  = read_be32(data + 8);
    uint32_t chunk_type = read_be32(data + 12);
    if (chunk_type != tag4('I', 'H', 'D', 'R') || chunk_len != 13)
        return PNG_ERR_BAD_HDR;

    const uint8_t *ih = data + 16;
    uint32_t w = read_be32(ih + 0);
    uint32_t h = read_be32(ih + 4);
    uint8_t depth     = ih[8];
    uint8_t ctype     = ih[9];
    uint8_t comp      = ih[10];
    uint8_t filt      = ih[11];
    uint8_t interlace = ih[12];

    if (!valid_ihdr(w, h, depth, ctype, comp, filt, interlace))
        return PNG_ERR_BAD_HDR;

    if (info) {
        info->width      = w;
        info->height     = h;
        info->bit_depth  = depth;
        info->color_type = ctype;
        info->interlace  = interlace;
        info->has_alpha  = (ctype == PNG_COLOR_GRAY_ALPHA || ctype == PNG_COLOR_RGBA) ? 1 : 0;
    }
    return PNG_OK;
}

/* ---------------- Потоковый читатель IDAT + битов ---------------- */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         chunk_pos;   /* начало текущего чанка (где длина) */
    size_t         pos;         /* текущая позиция внутри IDAT данных */
    size_t         chunk_end;   /* конец данных текущего чанка IDAT */
    uint32_t       bitbuf;
    int            bitcnt;
    int            eof;
} idat_stream_t;

static int idat_advance_chunk(idat_stream_t *s) {
    /* Текущий чанк закончился: переходим через 4 байта CRC к следующему. */
    size_t next = s->chunk_end + 4;
    while (next + 12 <= s->len) {
        uint32_t clen  = read_be32(s->data + next);
        uint32_t ctype = read_be32(s->data + next + 4);
        if (next + 12 + (size_t)clen > s->len) {
            s->eof = 1;
            return -1;
        }
        if (ctype == tag4('I', 'D', 'A', 'T')) {
            s->chunk_pos = next;
            s->pos       = next + 8;
            s->chunk_end = next + 8 + clen;
            if (clen == 0) {
                next = s->chunk_end + 4;
                continue;
            }
            return 0;
        }
        /* Если после IDAT встретился другой чанк, поток IDAT завершён. */
        s->eof = 1;
        return -1;
    }
    s->eof = 1;
    return -1;
}

static int idat_next_byte(idat_stream_t *s) {
    if (s->eof) return -1;
    if (s->pos >= s->chunk_end) {
        if (idat_advance_chunk(s) < 0) return -1;
    }
    return (int)s->data[s->pos++];
}

static int bs_fill(idat_stream_t *s, int nbits) {
    while (s->bitcnt < nbits) {
        int b = idat_next_byte(s);
        if (b < 0) return -1;
        s->bitbuf |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    return 0;
}

static int bs_read_bits(idat_stream_t *s, int nbits) {
    if (nbits == 0) return 0;
    if (bs_fill(s, nbits) < 0) return -1;
    int v = (int)(s->bitbuf & ((1u << nbits) - 1u));
    s->bitbuf >>= nbits;
    s->bitcnt -= nbits;
    return v;
}

static void bs_align_byte(idat_stream_t *s) {
    int drop = s->bitcnt & 7;
    s->bitbuf >>= drop;
    s->bitcnt -= drop;
}

/* ---------------- Каноническое дерево Хаффмана ---------------- */

#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMS 288

typedef struct {
    uint16_t count[HUFF_MAX_BITS + 1];
    uint16_t first_code[HUFF_MAX_BITS + 1];
    uint16_t first_sym[HUFF_MAX_BITS + 1];
    uint16_t symbols[HUFF_MAX_SYMS];
} huff_t;

static int huff_build(huff_t *h, const uint8_t *lengths, int num_syms) {
    if (num_syms > HUFF_MAX_SYMS) return -1;
    for (int i = 0; i <= HUFF_MAX_BITS; i++) h->count[i] = 0;

    for (int i = 0; i < num_syms; i++) {
        if (lengths[i] > HUFF_MAX_BITS) return -1;
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;

    /* Проверка на переполнение дерева (Kraft inequality). */
    int left = 1;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }

    int code = 0;
    int idx = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        code <<= 1;
        h->first_code[len] = (uint16_t)code;
        h->first_sym[len]  = (uint16_t)idx;
        idx += h->count[len];
        code += h->count[len];
    }

    uint16_t offs[HUFF_MAX_BITS + 1];
    for (int len = 0; len <= HUFF_MAX_BITS; len++) offs[len] = h->first_sym[len];

    for (int i = 0; i < num_syms; i++) {
        uint8_t l = lengths[i];
        if (l != 0) h->symbols[offs[l]++] = (uint16_t)i;
    }
    return 0;
}

static int huff_decode(idat_stream_t *s, const huff_t *h) {
    int code = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        int bit = bs_read_bits(s, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        int off = code - (int)h->first_code[len];
        if (off >= 0 && off < (int)h->count[len])
            return (int)h->symbols[h->first_sym[len] + off];
    }
    return -1;
}

/* ---------------- Рабочий контекст декодера ---------------- */

#define DEFLATE_WIN_SIZE 32768u
#define PNG_MAX_ROW_BYTES (PNG_MAX_WIDTH * 8u + 1u)

typedef struct {
    png_info_t info;

    uint8_t  plte_r[256];
    uint8_t  plte_g[256];
    uint8_t  plte_b[256];
    uint8_t  plte_a[256];
    int      plte_count;

    int      has_trns_key;
    uint16_t trns_r, trns_g, trns_b;

    uint32_t *out;
    uint32_t  max_w, max_h;

    /* Фильтрация строк */
    uint8_t  row_a[PNG_MAX_ROW_BYTES];
    uint8_t  row_b[PNG_MAX_ROW_BYTES];
    uint8_t *prev_row;
    uint8_t *cur_row;
    uint32_t row_pos;
    uint32_t row_need;      /* 1 (filter) + row_bytes */
    uint32_t bpp_bytes;     /* шаг фильтра (1..8) */

    /* Текущий проход (для non-interlace 1 проход, для Adam7 — 7 проходов) */
    int      pass;
    uint32_t pass_x0, pass_y0, pass_dx, pass_dy;
    uint32_t pass_w, pass_h, pass_y;
    int      done_pixels;

    /* Окно DEFLATE */
    uint8_t  window[DEFLATE_WIN_SIZE];
    uint32_t win_pos;
    uint32_t total_out;

    /* Деревья Хаффмана */
    huff_t   litlen_huff;
    huff_t   dist_huff;
    huff_t   codelen_huff;
} png_dec_t;

static png_dec_t g_png_dec;

/* ---------------- Преобразование строки PNG в 0xAARRGGBB ---------------- */

static uint8_t paeth_pred(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int setup_pass(png_dec_t *d, int pass) {
    static const uint8_t x0_tbl[7] = { 0, 4, 0, 2, 0, 1, 0 };
    static const uint8_t y0_tbl[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const uint8_t dx_tbl[7] = { 8, 8, 4, 4, 2, 2, 1 };
    static const uint8_t dy_tbl[7] = { 8, 8, 8, 4, 4, 2, 2 };

    while (1) {
        if (d->info.interlace == 0) {
            if (pass > 0) {
                d->done_pixels = 1;
                return 0;
            }
            d->pass_x0 = 0; d->pass_y0 = 0;
            d->pass_dx = 1; d->pass_dy = 1;
            d->pass_w  = d->info.width;
            d->pass_h  = d->info.height;
        } else {
            if (pass >= 7) {
                d->done_pixels = 1;
                return 0;
            }
            d->pass_x0 = x0_tbl[pass];
            d->pass_y0 = y0_tbl[pass];
            d->pass_dx = dx_tbl[pass];
            d->pass_dy = dy_tbl[pass];
            d->pass_w  = (d->info.width  > d->pass_x0)
                       ? (d->info.width  - d->pass_x0 + d->pass_dx - 1) / d->pass_dx : 0;
            d->pass_h  = (d->info.height > d->pass_y0)
                       ? (d->info.height - d->pass_y0 + d->pass_dy - 1) / d->pass_dy : 0;
            if (d->pass_w == 0 || d->pass_h == 0) {
                pass++;
                continue;
            }
        }

        d->pass   = pass;
        d->pass_y = 0;
        uint32_t bits_per_px = (uint32_t)channels_of(d->info.color_type) * d->info.bit_depth;
        uint32_t row_bytes   = (d->pass_w * bits_per_px + 7u) / 8u;
        d->bpp_bytes = (bits_per_px + 7u) / 8u;
        if (d->bpp_bytes < 1) d->bpp_bytes = 1;
        d->row_need = 1u + row_bytes;
        d->row_pos  = 0;
        memset(d->prev_row, 0, row_bytes);
        return 1;
    }
}

static uint8_t scale_sample(uint32_t v, uint8_t depth) {
    switch (depth) {
    case 1:  return v ? 255 : 0;
    case 2:  return (uint8_t)(v * 85u);
    case 4:  return (uint8_t)(v * 17u);
    case 8:  return (uint8_t)v;
    default: return (uint8_t)(v >> 8);
    }
}

static int flush_scanline(png_dec_t *d) {
    if (d->done_pixels) return 0;

    uint8_t  filt = d->cur_row[0];
    uint8_t *raw  = d->cur_row + 1;
    uint8_t *prev = d->prev_row;
    uint32_t row_bytes = d->row_need - 1;
    uint32_t bpp = d->bpp_bytes;

    if (filt > 4) return PNG_ERR_FORMAT;

    for (uint32_t i = 0; i < row_bytes; i++) {
        uint8_t a = (i >= bpp) ? raw[i - bpp] : 0;
        uint8_t b = prev[i];
        uint8_t c = (i >= bpp) ? prev[i - bpp] : 0;
        switch (filt) {
        case 0: break;
        case 1: raw[i] = (uint8_t)(raw[i] + a); break;
        case 2: raw[i] = (uint8_t)(raw[i] + b); break;
        case 3: raw[i] = (uint8_t)(raw[i] + (((uint32_t)a + (uint32_t)b) >> 1)); break;
        case 4: raw[i] = (uint8_t)(raw[i] + paeth_pred(a, b, c)); break;
        }
    }

    uint32_t y = d->pass_y0 + d->pass_y * d->pass_dy;
    uint32_t stride = d->info.width;
    uint8_t  depth  = d->info.bit_depth;
    uint8_t  ctype  = d->info.color_type;

    for (uint32_t px = 0; px < d->pass_w; px++) {
        uint32_t x = d->pass_x0 + px * d->pass_dx;
        uint8_t r = 0, g = 0, b = 0, a = 255;

        if (ctype == PNG_COLOR_GRAY) {
            uint32_t val;
            if (depth == 16) {
                val = read_be16(raw + px * 2);
                r = g = b = (uint8_t)(val >> 8);
            } else if (depth == 8) {
                val = raw[px];
                r = g = b = (uint8_t)val;
            } else {
                uint32_t ppb = 8u / depth;
                uint32_t byte_i = px / ppb;
                uint32_t shift = (ppb - 1u - (px % ppb)) * depth;
                uint32_t mask = (1u << depth) - 1u;
                val = (raw[byte_i] >> shift) & mask;
                r = g = b = scale_sample(val, depth);
            }
            if (d->has_trns_key && val == d->trns_r) {
                a = 0;
                d->info.has_alpha = 1;
            }
        } else if (ctype == PNG_COLOR_INDEXED) {
            uint32_t idx;
            if (depth == 8) {
                idx = raw[px];
            } else {
                uint32_t ppb = 8u / depth;
                uint32_t byte_i = px / ppb;
                uint32_t shift = (ppb - 1u - (px % ppb)) * depth;
                uint32_t mask = (1u << depth) - 1u;
                idx = (raw[byte_i] >> shift) & mask;
            }
            if ((int)idx >= d->plte_count) return PNG_ERR_FORMAT;
            r = d->plte_r[idx];
            g = d->plte_g[idx];
            b = d->plte_b[idx];
            a = d->plte_a[idx];
        } else if (ctype == PNG_COLOR_RGB) {
            if (depth == 8) {
                const uint8_t *p = raw + px * 3;
                r = p[0]; g = p[1]; b = p[2];
                if (d->has_trns_key && r == d->trns_r && g == d->trns_g && b == d->trns_b) {
                    a = 0;
                    d->info.has_alpha = 1;
                }
            } else {
                const uint8_t *p = raw + px * 6;
                uint16_t r16 = read_be16(p), g16 = read_be16(p + 2), b16 = read_be16(p + 4);
                r = (uint8_t)(r16 >> 8); g = (uint8_t)(g16 >> 8); b = (uint8_t)(b16 >> 8);
                if (d->has_trns_key && r16 == d->trns_r && g16 == d->trns_g && b16 == d->trns_b) {
                    a = 0;
                    d->info.has_alpha = 1;
                }
            }
        } else if (ctype == PNG_COLOR_GRAY_ALPHA) {
            if (depth == 8) {
                r = g = b = raw[px * 2];
                a = raw[px * 2 + 1];
            } else {
                r = g = b = raw[px * 4];
                a = raw[px * 4 + 2];
            }
        } else if (ctype == PNG_COLOR_RGBA) {
            if (depth == 8) {
                const uint8_t *p = raw + px * 4;
                r = p[0]; g = p[1]; b = p[2]; a = p[3];
            } else {
                const uint8_t *p = raw + px * 8;
                r = p[0]; g = p[2]; b = p[4]; a = p[6];
            }
        }

        d->out[(size_t)y * stride + x] =
            ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    /* Меняем prev_row и cur_row местами. */
    if (raw == d->row_a + 1) {
        d->prev_row = d->row_a + 1;
        d->cur_row  = d->row_b;
    } else {
        d->prev_row = d->row_b + 1;
        d->cur_row  = d->row_a;
    }

    d->row_pos = 0;
    d->pass_y++;
    if (d->pass_y >= d->pass_h) {
        setup_pass(d, d->pass + 1);
    }
    return 0;
}

static int push_byte(png_dec_t *d, uint8_t byte) {
    d->window[d->win_pos] = byte;
    d->win_pos = (d->win_pos + 1u) & (DEFLATE_WIN_SIZE - 1u);
    d->total_out++;

    if (d->done_pixels) return 0;

    d->cur_row[d->row_pos++] = byte;
    if (d->row_pos == d->row_need)
        return flush_scanline(d);
    return 0;
}

/* ---------------- Распаковка DEFLATE (RFC 1951) ---------------- */

static void build_fixed_huff(huff_t *litlen, huff_t *dist) {
    uint8_t ll[288];
    for (int i = 0;   i <= 143; i++) ll[i] = 8;
    for (int i = 144; i <= 255; i++) ll[i] = 9;
    for (int i = 256; i <= 279; i++) ll[i] = 7;
    for (int i = 280; i <= 287; i++) ll[i] = 8;
    huff_build(litlen, ll, 288);

    uint8_t dl[32];
    for (int i = 0; i < 32; i++) dl[i] = 5;
    huff_build(dist, dl, 32);
}

static int read_dynamic_huff(png_dec_t *d, idat_stream_t *s) {
    static const uint8_t cl_order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    int hlit  = bs_read_bits(s, 5);
    int hdist = bs_read_bits(s, 5);
    int hclen = bs_read_bits(s, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return PNG_ERR_DEFLATE;

    int n_lit  = hlit + 257;
    int n_dist = hdist + 1;
    int n_cl   = hclen + 4;
    if (n_lit > 286 || n_dist > 30) return PNG_ERR_DEFLATE;

    uint8_t cl_lens[19];
    for (int i = 0; i < 19; i++) cl_lens[i] = 0;
    for (int i = 0; i < n_cl; i++) {
        int v = bs_read_bits(s, 3);
        if (v < 0) return PNG_ERR_DEFLATE;
        cl_lens[cl_order[i]] = (uint8_t)v;
    }
    if (huff_build(&d->codelen_huff, cl_lens, 19) < 0) return PNG_ERR_DEFLATE;

    uint8_t lens[286 + 30];
    int total = n_lit + n_dist;
    int i = 0;
    while (i < total) {
        int sym = huff_decode(s, &d->codelen_huff);
        if (sym < 0) return PNG_ERR_DEFLATE;
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
        } else {
            int rep = 0;
            uint8_t val = 0;
            if (sym == 16) {
                if (i == 0) return PNG_ERR_DEFLATE;
                val = lens[i - 1];
                int extra = bs_read_bits(s, 2);
                if (extra < 0) return PNG_ERR_DEFLATE;
                rep = 3 + extra;
            } else if (sym == 17) {
                int extra = bs_read_bits(s, 3);
                if (extra < 0) return PNG_ERR_DEFLATE;
                rep = 3 + extra;
            } else {
                int extra = bs_read_bits(s, 7);
                if (extra < 0) return PNG_ERR_DEFLATE;
                rep = 11 + extra;
            }
            if (i + rep > total) return PNG_ERR_DEFLATE;
            for (int r = 0; r < rep; r++) lens[i++] = val;
        }
    }

    if (huff_build(&d->litlen_huff, lens, n_lit) < 0) return PNG_ERR_DEFLATE;
    if (huff_build(&d->dist_huff, lens + n_lit, n_dist) < 0) return PNG_ERR_DEFLATE;
    return 0;
}

static int inflate_zlib(png_dec_t *d, idat_stream_t *s) {
    static const uint16_t len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const uint8_t dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    int cmf = idat_next_byte(s);
    int flg = idat_next_byte(s);
    if (cmf < 0 || flg < 0) return PNG_ERR_TRUNCATED;
    if ((cmf & 0x0F) != 8) return PNG_ERR_DEFLATE;
    if (((cmf << 8) | flg) % 31 != 0) return PNG_ERR_DEFLATE;
    if (flg & 0x20) return PNG_ERR_DEFLATE; /* FDICT не поддерживается в PNG */

    int bfinal = 0;
    while (!bfinal) {
        bfinal = bs_read_bits(s, 1);
        int btype = bs_read_bits(s, 2);
        if (bfinal < 0 || btype < 0) return PNG_ERR_DEFLATE;

        if (btype == 0) {
            bs_align_byte(s);
            int l0 = idat_next_byte(s), l1 = idat_next_byte(s);
            int n0 = idat_next_byte(s), n1 = idat_next_byte(s);
            if (l0 < 0 || l1 < 0 || n0 < 0 || n1 < 0) return PNG_ERR_DEFLATE;
            uint16_t len  = (uint16_t)(l0 | (l1 << 8));
            uint16_t nlen = (uint16_t)(n0 | (n1 << 8));
            if ((uint16_t)(len ^ 0xFFFFu) != nlen) return PNG_ERR_DEFLATE;
            for (uint16_t i = 0; i < len; i++) {
                int b = idat_next_byte(s);
                if (b < 0) return PNG_ERR_DEFLATE;
                int err = push_byte(d, (uint8_t)b);
                if (err < 0) return err;
            }
        } else if (btype == 1 || btype == 2) {
            if (btype == 1) {
                build_fixed_huff(&d->litlen_huff, &d->dist_huff);
            } else {
                int err = read_dynamic_huff(d, s);
                if (err < 0) return err;
            }

            while (1) {
                int sym = huff_decode(s, &d->litlen_huff);
                if (sym < 0) return PNG_ERR_DEFLATE;
                if (sym < 256) {
                    int err = push_byte(d, (uint8_t)sym);
                    if (err < 0) return err;
                } else if (sym == 256) {
                    break;
                } else {
                    if (sym > 285) return PNG_ERR_DEFLATE;
                    int lidx = sym - 257;
                    int extra = bs_read_bits(s, len_extra[lidx]);
                    if (extra < 0) return PNG_ERR_DEFLATE;
                    uint32_t match_len = (uint32_t)len_base[lidx] + (uint32_t)extra;

                    int dsym = huff_decode(s, &d->dist_huff);
                    if (dsym < 0 || dsym >= 30) return PNG_ERR_DEFLATE;
                    int dextra = bs_read_bits(s, dist_extra[dsym]);
                    if (dextra < 0) return PNG_ERR_DEFLATE;
                    uint32_t dist = (uint32_t)dist_base[dsym] + (uint32_t)dextra;

                    if (dist == 0 || dist > DEFLATE_WIN_SIZE || dist > d->total_out)
                        return PNG_ERR_DEFLATE;

                    for (uint32_t k = 0; k < match_len; k++) {
                        uint8_t b = d->window[(d->win_pos - dist) & (DEFLATE_WIN_SIZE - 1u)];
                        int err = push_byte(d, b);
                        if (err < 0) return err;
                    }
                }
            }
        } else {
            return PNG_ERR_DEFLATE;
        }
    }

    return d->done_pixels ? PNG_OK : PNG_ERR_TRUNCATED;
}

/* ---------------- Главная точка входа ---------------- */

int png_decode(const uint8_t *data, size_t len,
               uint32_t *out_argb, uint32_t max_w, uint32_t max_h,
               png_info_t *info) {
    png_dec_t *d = &g_png_dec;
    memset(d, 0, sizeof(*d));

    int err = png_probe(data, len, &d->info);
    if (err < 0) return err;

    if (d->info.width > max_w || d->info.height > max_h ||
        d->info.width > PNG_MAX_WIDTH || d->info.height > PNG_MAX_HEIGHT)
        return PNG_ERR_TOO_LARGE;

    d->out   = out_argb;
    d->max_w = max_w;
    d->max_h = max_h;
    for (int i = 0; i < 256; i++) d->plte_a[i] = 255;

    size_t pos = 8;
    int seen_idat = 0;
    int seen_iend = 0;
    size_t first_idat_pos = 0;

    while (pos + 12 <= len) {
        uint32_t clen  = read_be32(data + pos);
        uint32_t ctype = read_be32(data + pos + 4);
        if (pos + 12 + (size_t)clen > len) return PNG_ERR_TRUNCATED;
        const uint8_t *cdata = data + pos + 8;

        if (ctype == tag4('P', 'L', 'T', 'E')) {
            if (clen == 0 || (clen % 3) != 0 || clen > 256 * 3)
                return PNG_ERR_FORMAT;
            d->plte_count = (int)(clen / 3);
            for (int i = 0; i < d->plte_count; i++) {
                d->plte_r[i] = cdata[i * 3 + 0];
                d->plte_g[i] = cdata[i * 3 + 1];
                d->plte_b[i] = cdata[i * 3 + 2];
            }
        } else if (ctype == tag4('t', 'R', 'N', 'S')) {
            if (d->info.color_type == PNG_COLOR_INDEXED) {
                if (clen > 256) return PNG_ERR_FORMAT;
                for (uint32_t i = 0; i < clen; i++) {
                    d->plte_a[i] = cdata[i];
                    if (cdata[i] < 255) d->info.has_alpha = 1;
                }
            } else if (d->info.color_type == PNG_COLOR_GRAY && clen >= 2) {
                d->has_trns_key = 1;
                d->trns_r = read_be16(cdata);
            } else if (d->info.color_type == PNG_COLOR_RGB && clen >= 6) {
                d->has_trns_key = 1;
                d->trns_r = read_be16(cdata + 0);
                d->trns_g = read_be16(cdata + 2);
                d->trns_b = read_be16(cdata + 4);
            }
        } else if (ctype == tag4('I', 'D', 'A', 'T')) {
            if (!seen_idat) {
                seen_idat = 1;
                first_idat_pos = pos;
            }
        } else if (ctype == tag4('I', 'E', 'N', 'D')) {
            seen_iend = 1;
            break;
        }

        pos += 12 + (size_t)clen;
    }

    if (!seen_idat || !seen_iend) return PNG_ERR_FORMAT;
    if (d->info.color_type == PNG_COLOR_INDEXED && d->plte_count == 0)
        return PNG_ERR_FORMAT;

    d->cur_row  = d->row_a;
    d->prev_row = d->row_b + 1;
    setup_pass(d, 0);

    uint32_t first_len = read_be32(data + first_idat_pos);
    idat_stream_t stream;
    stream.data      = data;
    stream.len       = len;
    stream.chunk_pos = first_idat_pos;
    stream.pos       = first_idat_pos + 8;
    stream.chunk_end = first_idat_pos + 8 + first_len;
    stream.bitbuf    = 0;
    stream.bitcnt    = 0;
    stream.eof       = 0;

    err = inflate_zlib(d, &stream);
    if (err < 0) return err;

    if (info) *info = d->info;
    return PNG_OK;
}

const char *png_strerror(int err) {
    switch (err) {
    case PNG_OK:            return "OK";
    case PNG_ERR_BAD_SIG:   return "NOT A PNG FILE";
    case PNG_ERR_BAD_HDR:   return "BAD PNG HEADER";
    case PNG_ERR_TOO_LARGE: return "IMAGE TOO LARGE";
    case PNG_ERR_TRUNCATED: return "TRUNCATED PNG";
    case PNG_ERR_DEFLATE:   return "DEFLATE ERROR";
    case PNG_ERR_FORMAT:    return "INVALID PNG DATA";
    default:                return "PNG ERROR";
    }
}

const char *png_color_name(uint8_t color_type) {
    switch (color_type) {
    case PNG_COLOR_GRAY:       return "GRAY";
    case PNG_COLOR_RGB:        return "RGB";
    case PNG_COLOR_INDEXED:    return "INDEXED";
    case PNG_COLOR_GRAY_ALPHA: return "GRAY+A";
    case PNG_COLOR_RGBA:       return "RGBA";
    default:                   return "?";
    }
}
