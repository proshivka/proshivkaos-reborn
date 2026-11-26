/* gui/gif.h — freestanding-декодер GIF87a/GIF89a с покадровым выводом.
 * Выходной буфер — композитное изображение 0xAARRGGBB.
 */
#ifndef GUI_GIF_H
#define GUI_GIF_H

#include <stddef.h>
#include <stdint.h>

#define GIF_MAX_WIDTH  1024u
#define GIF_MAX_HEIGHT 1024u
#define GIF_MAX_PIXELS (GIF_MAX_WIDTH * GIF_MAX_HEIGHT)

enum {
    GIF_FRAME             =  1,
    GIF_DONE              =  0,
    GIF_ERR_BAD_SIG       = -1,
    GIF_ERR_TRUNCATED     = -2,
    GIF_ERR_TOO_LARGE     = -3,
    GIF_ERR_FORMAT        = -4,
    GIF_ERR_LZW           = -5,
    GIF_ERR_PALETTE       = -6,
    GIF_ERR_MEMORY        = -7
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_index;       /* Номер показанного кадра, начиная с 1. */
    uint32_t delay_ms;          /* Задержка до следующего кадра. */
    uint8_t  has_transparency;
    uint8_t  disposal;
} gif_info_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t first_frame_pos;

    uint32_t *canvas;
    uint32_t *restore;
    size_t restore_capacity;
    uint32_t max_w;
    uint32_t max_h;

    uint16_t width, height;
    uint8_t background_index;
    uint8_t global_table_present;
    uint16_t global_colors;
    uint8_t global_palette[256][3];
    uint8_t local_palette[256][3];
    const uint8_t (*active_palette)[3];
    uint16_t active_colors;
    uint32_t background_pixel;
    uint8_t canvas_initialized;

    /* Текущий Graphic Control Extension. */
    uint8_t gce_disposal;
    uint8_t gce_transparent;
    uint8_t gce_trans_index;
    uint16_t gce_delay_cs;

    /* Кадр, который сейчас лежит в canvas, — для disposal 2 и 3. */
    uint8_t previous_disposal;
    uint8_t previous_transparent;
    uint8_t previous_trans_index;
    uint16_t previous_x, previous_y, previous_w, previous_h;
    uint16_t restore_x, restore_y, restore_w, restore_h;
    uint8_t restore_valid;

    /* Циклическое воспроизведение Netscape loop extension. */
    uint8_t has_loop_extension;
    uint8_t infinite_loop;
    uint16_t loop_count;
    uint16_t loops_done;
    uint32_t frame_count;

    /* Рабочие таблицы LZW; не занимают стек ядра. */
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint8_t stack[4096];
    uint16_t row_order[GIF_MAX_HEIGHT];
} gif_decoder_t;

int gif_is_gif(const uint8_t *data, size_t len);
int gif_decoder_open(gif_decoder_t *decoder,
                     const uint8_t *data, size_t len,
                     uint32_t *out_argb, uint32_t max_w, uint32_t max_h,
                     uint32_t *restore_buffer, size_t restore_capacity,
                     gif_info_t *info);
/* Возвращает GIF_FRAME, GIF_DONE или отрицательный код ошибки. */
int gif_decoder_next(gif_decoder_t *decoder, gif_info_t *info);
const char *gif_strerror(int err);

#endif
