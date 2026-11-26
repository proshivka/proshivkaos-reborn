/* gui/gif.c — GIF87a/GIF89a, палитры, прозрачность, interlace, LZW,
 * disposal 0..3 и Netscape loop extension. Без libc и malloc.
 */
#include "gif.h"

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

typedef struct {
    gif_decoder_t *decoder;
    unsigned block_remaining;
    int terminated;
} gif_subreader_t;

static uint32_t gif_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static int read_u8(gif_decoder_t *d, uint8_t *out) {
    if (d->pos >= d->len) return -1;
    *out = d->data[d->pos++];
    return 0;
}

static int read_le16(gif_decoder_t *d, uint16_t *out) {
    uint8_t lo, hi;
    if (read_u8(d, &lo) < 0 || read_u8(d, &hi) < 0) return -1;
    *out = (uint16_t)(lo | ((uint16_t)hi << 8));
    return 0;
}

int gif_is_gif(const uint8_t *data, size_t len) {
    if (!data || len < 6) return 0;
    return (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
            data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a');
}

static int read_color_table(gif_decoder_t *d, uint8_t table[256][3], unsigned count) {
    if (count < 2 || count > 256 || d->len - d->pos < (size_t)count * 3u)
        return -1;
    for (unsigned i = 0; i < count; i++) {
        table[i][0] = d->data[d->pos++];
        table[i][1] = d->data[d->pos++];
        table[i][2] = d->data[d->pos++];
    }
    return 0;
}

static int skip_subblocks(gif_decoder_t *d) {
    for (;;) {
        uint8_t n;
        if (read_u8(d, &n) < 0) return -1;
        if (n == 0) return 0;
        if (d->len - d->pos < n) return -1;
        d->pos += n;
    }
}

static int read_application_extension(gif_decoder_t *d) {
    uint8_t id_len;
    char app_id[12] = {0};
    if (read_u8(d, &id_len) < 0 || id_len > 11 || d->len - d->pos < id_len)
        return -1;
    for (unsigned i = 0; i < id_len; i++) app_id[i] = (char)d->data[d->pos++];

    int is_netscape = 0;
    for (unsigned i = 0; i < 11; i++) {
        static const char netscape[] = "NETSCAPE2.0";
        static const char animexts[] = "ANIMEXTS1.0";
        if (app_id[i] != netscape[i] && app_id[i] != animexts[i]) break;
        if (i == 10) is_netscape = 1;
    }

    int first_block = 1;
    for (;;) {
        uint8_t n;
        if (read_u8(d, &n) < 0) return -1;
        if (n == 0) return 0;
        if (d->len - d->pos < n) return -1;
        if (is_netscape && first_block && n >= 3 && d->data[d->pos] == 1) {
            d->has_loop_extension = 1;
            d->loop_count = (uint16_t)(d->data[d->pos + 1] |
                                      ((uint16_t)d->data[d->pos + 2] << 8));
            d->infinite_loop = (d->loop_count == 0);
        }
        d->pos += n;
        first_block = 0;
    }
}

static int read_graphic_control(gif_decoder_t *d) {
    uint8_t block_size;
    if (read_u8(d, &block_size) < 0) return -1;
    if (block_size != 4 || d->len - d->pos < 5) return -1;

    uint8_t packed = d->data[d->pos++];
    uint16_t delay = (uint16_t)(d->data[d->pos] | ((uint16_t)d->data[d->pos + 1] << 8));
    d->pos += 2;
    uint8_t transparent_index = d->data[d->pos++];
    uint8_t terminator = d->data[d->pos++];
    if (terminator != 0) return -1;

    d->gce_disposal = (uint8_t)((packed >> 2) & 7u);
    d->gce_transparent = (uint8_t)(packed & 1u);
    d->gce_trans_index = transparent_index;
    d->gce_delay_cs = delay;
    return 0;
}

int gif_decoder_open(gif_decoder_t *d,
                     const uint8_t *data, size_t len,
                     uint32_t *out_argb, uint32_t max_w, uint32_t max_h,
                     uint32_t *restore_buffer, size_t restore_capacity,
                     gif_info_t *info) {
    if (!d || !data || !out_argb) return GIF_ERR_FORMAT;
    if (!gif_is_gif(data, len)) return GIF_ERR_BAD_SIG;
    memset(d, 0, sizeof(*d));
    d->data = data;
    d->len = len;
    d->canvas = out_argb;
    d->restore = restore_buffer;
    d->restore_capacity = restore_capacity;
    d->max_w = max_w;
    d->max_h = max_h;
    d->loop_count = 0;

    d->pos = 6;
    uint16_t width, height;
    if (read_le16(d, &width) < 0 || read_le16(d, &height) < 0 ||
        width == 0 || height == 0)
        return GIF_ERR_TRUNCATED;
    if (width > max_w || height > max_h || width > GIF_MAX_WIDTH || height > GIF_MAX_HEIGHT)
        return GIF_ERR_TOO_LARGE;
    d->width = width;
    d->height = height;

    if (d->len - d->pos < 3) return GIF_ERR_TRUNCATED;
    uint8_t packed = d->data[d->pos++];
    d->background_index = d->data[d->pos++];
    (void)d->data[d->pos++]; /* pixel aspect ratio */

    if (packed & 0x80u) {
        unsigned colors = 1u << ((packed & 7u) + 1u);
        if (read_color_table(d, d->global_palette, colors) < 0)
            return GIF_ERR_TRUNCATED;
        d->global_colors = (uint16_t)colors;
        d->global_table_present = 1;
        if (d->background_index < d->global_colors) {
            d->background_pixel = gif_rgb(d->global_palette[d->background_index][0],
                                          d->global_palette[d->background_index][1],
                                          d->global_palette[d->background_index][2]);
        }
    }

    d->first_frame_pos = d->pos;
    d->gce_disposal = 0;
    d->gce_transparent = 0;
    d->gce_trans_index = 0;
    d->gce_delay_cs = 0;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->width = width;
        info->height = height;
    }
    return GIF_DONE;
}

static void fill_rect(gif_decoder_t *d, unsigned x, unsigned y,
                      unsigned w, unsigned h, uint32_t color) {
    if (x >= d->width || y >= d->height) return;
    if (w > d->width - x) w = d->width - x;
    if (h > d->height - y) h = d->height - y;
    for (unsigned row = 0; row < h; row++) {
        uint32_t *dst = d->canvas + (size_t)(y + row) * d->width + x;
        for (unsigned col = 0; col < w; col++) dst[col] = color;
    }
}

static void apply_previous_disposal(gif_decoder_t *d) {
    if (!d->canvas_initialized) return;
    if (d->previous_disposal == 2) {
        uint32_t bg = d->background_pixel;
        if (d->previous_transparent && d->previous_trans_index == d->background_index)
            bg = 0;
        fill_rect(d, d->previous_x, d->previous_y,
                  d->previous_w, d->previous_h, bg);
    } else if (d->previous_disposal == 3 && d->restore_valid && d->restore) {
        for (unsigned row = 0; row < d->restore_h; row++) {
            uint32_t *dst = d->canvas + (size_t)(d->restore_y + row) * d->width + d->restore_x;
            const uint32_t *src = d->restore + (size_t)row * d->restore_w;
            memcpy(dst, src, (size_t)d->restore_w * sizeof(uint32_t));
        }
    }
    d->restore_valid = 0;
}


static int prepare_interlace_rows(gif_decoder_t *d, unsigned height, int interlaced) {
    unsigned n = 0;
    if (!interlaced) {
        for (unsigned y = 0; y < height; y++) d->row_order[n++] = (uint16_t)y;
    } else {
        static const unsigned starts[4] = {0, 4, 2, 1};
        static const unsigned steps[4] = {8, 8, 4, 2};
        for (unsigned pass = 0; pass < 4; pass++)
            for (unsigned y = starts[pass]; y < height; y += steps[pass])
                d->row_order[n++] = (uint16_t)y;
    }
    return n == height ? 0 : -1;
}

static int subreader_byte(gif_subreader_t *r) {
    if (r->terminated) return -1;
    if (r->block_remaining == 0) {
        uint8_t n;
        if (read_u8(r->decoder, &n) < 0) return -1;
        if (n == 0) {
            r->terminated = 1;
            return -1;
        }
        r->block_remaining = n;
    }
    uint8_t b;
    if (read_u8(r->decoder, &b) < 0) return -1;
    r->block_remaining--;
    return b;
}

typedef struct {
    gif_subreader_t *reader;
    uint32_t bits;
    unsigned bit_count;
} gif_bitreader_t;

static int lzw_read_code(gif_bitreader_t *br, unsigned code_size) {
    while (br->bit_count < code_size) {
        int b = subreader_byte(br->reader);
        if (b < 0) return -1;
        br->bits |= (uint32_t)(uint8_t)b << br->bit_count;
        br->bit_count += 8;
    }
    int code = (int)(br->bits & ((1u << code_size) - 1u));
    br->bits >>= code_size;
    br->bit_count -= code_size;
    return code;
}

static int output_index(gif_decoder_t *d, uint32_t *out_pos, uint16_t left,
                        uint16_t top, uint16_t frame_w, uint16_t frame_h,
                        uint8_t index, uint8_t transparent, uint8_t transparent_index) {
    if (*out_pos >= (uint32_t)frame_w * frame_h) return -1;
    if (index >= d->active_colors) return GIF_ERR_PALETTE;

    unsigned row_sequence = *out_pos / frame_w;
    unsigned x = *out_pos % frame_w;
    unsigned y = d->row_order[row_sequence];
    if (!(transparent && index == transparent_index)) {
        uint32_t color = gif_rgb(d->active_palette[index][0],
                                 d->active_palette[index][1],
                                 d->active_palette[index][2]);
        d->canvas[(size_t)(top + y) * d->width + left + x] = color;
    }
    (*out_pos)++;
    return 0;
}

static int decode_lzw(gif_decoder_t *d, uint8_t minimum_code_size,
                      uint16_t left, uint16_t top, uint16_t frame_w, uint16_t frame_h,
                      uint8_t transparent, uint8_t transparent_index) {
    if (minimum_code_size < 2 || minimum_code_size > 8) return GIF_ERR_LZW;

    gif_subreader_t sub = { d, 0, 0 };
    gif_bitreader_t bits = { &sub, 0, 0 };
    unsigned clear_code = 1u << minimum_code_size;
    unsigned end_code = clear_code + 1u;
    unsigned next_code = end_code + 1u;
    unsigned code_size = minimum_code_size + 1u;
    int old_code = -1;
    uint8_t first_char = 0;
    uint32_t out_pos = 0;
    uint32_t expected = (uint32_t)frame_w * frame_h;
    int got_end = 0;

    while (out_pos < expected) {
        int code = lzw_read_code(&bits, code_size);
        if (code < 0) return GIF_ERR_TRUNCATED;
        if ((unsigned)code == clear_code) {
            next_code = end_code + 1u;
            code_size = minimum_code_size + 1u;
            old_code = -1;
            continue;
        }
        if ((unsigned)code == end_code) {
            got_end = 1;
            break;
        }
        if ((unsigned)code >= next_code && (unsigned)code != next_code)
            return GIF_ERR_LZW;

        if (old_code < 0) {
            if ((unsigned)code >= clear_code) return GIF_ERR_LZW;
            first_char = (uint8_t)code;
            int rc = output_index(d, &out_pos, left, top, frame_w, frame_h,
                                  first_char, transparent, transparent_index);
            if (rc < 0) return rc;
            old_code = code;
            continue;
        }

        int in_code = code;
        unsigned sp = 0;
        if ((unsigned)code == next_code) {
            if (sp >= 4096) return GIF_ERR_LZW;
            d->stack[sp++] = first_char;
            code = old_code;
        }

        while ((unsigned)code >= clear_code) {
            if ((unsigned)code >= next_code || sp >= 4095) return GIF_ERR_LZW;
            d->stack[sp++] = d->suffix[code];
            code = d->prefix[code];
        }
        if (code < 0 || (unsigned)code >= clear_code || sp >= 4096) return GIF_ERR_LZW;
        first_char = (uint8_t)code;
        d->stack[sp++] = first_char;

        while (sp) {
            int rc = output_index(d, &out_pos, left, top, frame_w, frame_h,
                                  d->stack[--sp], transparent, transparent_index);
            if (rc < 0) return rc;
            if (out_pos == expected) break;
        }

        if (next_code < 4096) {
            d->prefix[next_code] = (uint16_t)old_code;
            d->suffix[next_code] = first_char;
            next_code++;
            if (next_code == (1u << code_size) && code_size < 12)
                code_size++;
        }
        old_code = in_code;
    }

    /* Проверяем конец потока, затем съедаем остаток последнего sub-block:
       следующая структура GIF начинается строго после нулевого блока. */
    if (out_pos != expected && !got_end) return GIF_ERR_TRUNCATED;
    while (!sub.terminated) {
        int b = subreader_byte(&sub);
        if (b < 0 && !sub.terminated) return GIF_ERR_TRUNCATED;
    }
    return out_pos == expected ? 0 : GIF_ERR_TRUNCATED;
}

static int restart_animation(gif_decoder_t *d) {
    if (!d->has_loop_extension) return 0;
    if (!d->infinite_loop && d->loops_done >= d->loop_count) return 0;
    if (!d->infinite_loop) d->loops_done++;
    d->pos = d->first_frame_pos;
    d->frame_count = 0;
    d->previous_disposal = 0;
    d->restore_valid = 0;
    d->canvas_initialized = 0;
    return 1;
}

int gif_decoder_next(gif_decoder_t *d, gif_info_t *info) {
    if (!d || !d->data || !d->canvas) return GIF_ERR_FORMAT;

    for (;;) {
        uint8_t marker;
        if (read_u8(d, &marker) < 0) return GIF_ERR_TRUNCATED;

        if (marker == 0x3B) { /* trailer */
            if (restart_animation(d)) {
                /* Canvas будет очищен с учётом GCE прозрачности первого кадра
                   сразу после разбора его управляющего блока. */
                d->gce_disposal = 0;
                d->gce_transparent = 0;
                d->gce_trans_index = 0;
                d->gce_delay_cs = 0;
                continue;
            }
            return GIF_DONE;
        }

        if (marker == 0x21) { /* extension */
            uint8_t label;
            if (read_u8(d, &label) < 0) return GIF_ERR_TRUNCATED;
            int rc = 0;
            if (label == 0xF9) rc = read_graphic_control(d);
            else if (label == 0xFF) rc = read_application_extension(d);
            else rc = skip_subblocks(d);
            if (rc < 0) return GIF_ERR_TRUNCATED;
            continue;
        }

        if (marker != 0x2C) return GIF_ERR_FORMAT;

        uint16_t left, top, frame_w, frame_h;
        if (read_le16(d, &left) < 0 || read_le16(d, &top) < 0 ||
            read_le16(d, &frame_w) < 0 || read_le16(d, &frame_h) < 0 ||
            d->pos >= d->len)
            return GIF_ERR_TRUNCATED;
        uint8_t packed = d->data[d->pos++];
        if (frame_w == 0 || frame_h == 0 || left > d->width || top > d->height ||
            frame_w > d->width - left || frame_h > d->height - top)
            return GIF_ERR_FORMAT;

        d->active_palette = d->global_palette;
        d->active_colors = d->global_colors;
        if (packed & 0x80u) {
            unsigned colors = 1u << ((packed & 7u) + 1u);
            if (read_color_table(d, d->local_palette, colors) < 0)
                return GIF_ERR_TRUNCATED;
            d->active_palette = (const uint8_t (*)[3])d->local_palette;
            d->active_colors = (uint16_t)colors;
        }
        if (!d->active_palette || d->active_colors < 2) return GIF_ERR_PALETTE;
        if (d->pos >= d->len) return GIF_ERR_TRUNCATED;
        uint8_t min_code_size = d->data[d->pos++];

        /* Disposal применяется лишь когда действительно найден следующий
         * кадр. При достижении trailer последний кадр должен оставаться на
         * экране, а не исчезать в момент завершения анимации. */
        if (d->canvas_initialized)
            apply_previous_disposal(d);

        if (!d->canvas_initialized) {
            uint32_t bg = d->global_table_present ? d->background_pixel : 0;
            if (d->gce_transparent && d->gce_trans_index == d->background_index)
                bg = 0;
            uint32_t count = (uint32_t)d->width * d->height;
            for (uint32_t i = 0; i < count; i++) d->canvas[i] = bg;
            d->canvas_initialized = 1;
        }

        if (prepare_interlace_rows(d, frame_h, (packed & 0x40u) != 0) < 0)
            return GIF_ERR_FORMAT;

        if (d->gce_disposal == 3) {
            size_t need = (size_t)frame_w * frame_h;
            if (!d->restore || need > d->restore_capacity) return GIF_ERR_MEMORY;
            for (unsigned row = 0; row < frame_h; row++) {
                const uint32_t *src = d->canvas + (size_t)(top + row) * d->width + left;
                uint32_t *dst = d->restore + (size_t)row * frame_w;
                memcpy(dst, src, (size_t)frame_w * sizeof(uint32_t));
            }
            d->restore_x = left;
            d->restore_y = top;
            d->restore_w = frame_w;
            d->restore_h = frame_h;
            d->restore_valid = 1;
        }

        int rc = decode_lzw(d, min_code_size, left, top, frame_w, frame_h,
                            d->gce_transparent, d->gce_trans_index);
        if (rc < 0) return rc;

        d->previous_x = left;
        d->previous_y = top;
        d->previous_w = frame_w;
        d->previous_h = frame_h;
        d->previous_disposal = d->gce_disposal;
        d->previous_transparent = d->gce_transparent;
        d->previous_trans_index = d->gce_trans_index;
        d->frame_count++;

        if (info) {
            info->width = d->width;
            info->height = d->height;
            info->frame_index = d->frame_count;
            info->delay_ms = d->gce_delay_cs ? (uint32_t)d->gce_delay_cs * 10u : 100u;
            if (info->delay_ms < 20u) info->delay_ms = 20u;
            info->has_transparency = d->gce_transparent;
            info->disposal = d->gce_disposal;
        }

        /* GCE относится только к следующему изображению. */
        d->gce_disposal = 0;
        d->gce_transparent = 0;
        d->gce_trans_index = 0;
        d->gce_delay_cs = 0;
        return GIF_FRAME;
    }
}

const char *gif_strerror(int err) {
    switch (err) {
    case GIF_ERR_BAD_SIG:  return "NOT A GIF FILE";
    case GIF_ERR_TRUNCATED:return "TRUNCATED GIF";
    case GIF_ERR_TOO_LARGE:return "IMAGE TOO LARGE";
    case GIF_ERR_FORMAT:   return "INVALID GIF FORMAT";
    case GIF_ERR_LZW:      return "GIF LZW ERROR";
    case GIF_ERR_PALETTE:  return "GIF PALETTE ERROR";
    case GIF_ERR_MEMORY:   return "GIF FRAME NEEDS MORE MEMORY";
    default:               return "GIF ERROR";
    }
}
