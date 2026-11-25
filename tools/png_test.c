#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "png.h"

static uint32_t g_out[PNG_MAX_WIDTH * PNG_MAX_HEIGHT];
static uint8_t  g_in[2 * 1024 * 1024];

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.png> <expected_raw_argb.bin>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen png"); return 2; }
    size_t len = fread(g_in, 1, sizeof(g_in), f);
    fclose(f);

    png_info_t info;
    int err = png_decode(g_in, len, g_out, PNG_MAX_WIDTH, PNG_MAX_HEIGHT, &info);
    if (err != PNG_OK) {
        fprintf(stderr, "FAIL %s: png_decode=%d (%s)\n", argv[1], err, png_strerror(err));
        return 1;
    }

    FILE *fe = fopen(argv[2], "rb");
    if (!fe) { perror("fopen exp"); return 2; }
    size_t n_px = (size_t)info.width * (size_t)info.height;
    uint32_t *exp = (uint32_t *)malloc(n_px * 4);
    size_t got = fread(exp, 4, n_px, fe);
    fclose(fe);
    if (got != n_px) {
        fprintf(stderr, "FAIL %s: size mismatch got=%zu exp=%zu\n", argv[1], got, n_px);
        free(exp);
        return 1;
    }

    for (size_t i = 0; i < n_px; i++) {
        if (g_out[i] != exp[i]) {
            fprintf(stderr, "FAIL %s: pixel %zu got=0x%08X exp=0x%08X\n",
                    argv[1], i, g_out[i], exp[i]);
            free(exp);
            return 1;
        }
    }

    free(exp);
    printf("PASS %s (%ux%u, %s, %u-bit, interlace=%u)\n",
           argv[1], info.width, info.height,
           png_color_name(info.color_type), info.bit_depth, info.interlace);
    return 0;
}
