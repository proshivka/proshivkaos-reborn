/* gui/wallpaper.c — обои рабочего стола (XP запечённые + Windows 11 Bloom)
 * и синхронизация адаптивной палитры при переключении семейств тем.
 */
#include "hal_gfx.h"
#include "gfxfb.h"
#include "palette.h"
#include "window.h"
#include "wallpaper.h"
#include "wallpaper_data.h"

/* ---------------- запасной процедурный пейзаж (XP без картинки) ---------------- */

typedef struct { int x_pct, h_pct; } hill_point_t;

static const hill_point_t hill_back[]  = {
    {0, 35}, {25, 45}, {50, 30}, {75, 50}, {100, 35}
};
static const hill_point_t hill_front[] = {
    {0, 20}, {20, 30}, {45, 15}, {70, 35}, {100, 18}
};

static int interpolate_height(const hill_point_t *pts, int n, int x_pct) {
    for (int i = 0; i < n - 1; i++) {
        if (x_pct >= pts[i].x_pct && x_pct <= pts[i + 1].x_pct) {
            int x0 = pts[i].x_pct,     h0 = pts[i].h_pct;
            int x1 = pts[i + 1].x_pct, h1 = pts[i + 1].h_pct;
            if (x1 == x0) return h0;
            return h0 + (h1 - h0) * (x_pct - x0) / (x1 - x0);
        }
    }
    return pts[n - 1].h_pct;
}

static void draw_hill_layer(int screen_w, int screen_h,
                            const hill_point_t *pts, int n, uint8_t color) {
    for (int x = 0; x < screen_w; x++) {
        int x_pct = x * 100 / screen_w;
        int h_pct = interpolate_height(pts, n, x_pct);
        int y_top = screen_h - (h_pct * screen_h / 100);
        for (int y = y_top; y < screen_h; y++)
            hal_gfx_put_pixel(x, y, color);
    }
}

static void draw_filled_circle(int cx, int cy, int r, uint8_t color) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                hal_gfx_put_pixel(cx + x, cy + y, color);
}

static void draw_cloud(int cx, int cy, int r) {
    draw_filled_circle(cx - r, cy,       r,               GFX_CLOUD_WHITE);
    draw_filled_circle(cx,     cy - r/2, r + r / 3,        GFX_CLOUD_WHITE);
    draw_filled_circle(cx + r, cy,       r,               GFX_CLOUD_WHITE);
}

static void draw_procedural_fallback(int screen_w, int screen_h) {
    int horizon_y = screen_h * 62 / 100;

    int band1 = horizon_y * 40 / 100;
    int band2 = horizon_y * 75 / 100;
    for (int y = 0; y < horizon_y; y++) {
        uint8_t color = (y < band1) ? GFX_SKY_DEEP
                       : (y < band2) ? GFX_SKY_MID
                                     : GFX_SKY_HORIZON;
        for (int x = 0; x < screen_w; x++)
            hal_gfx_put_pixel(x, y, color);
    }

    for (int y = horizon_y; y < screen_h; y++)
        for (int x = 0; x < screen_w; x++)
            hal_gfx_put_pixel(x, y, GFX_HILL_MID);

    int sun_r = (screen_w < screen_h ? screen_w : screen_h) * 5 / 100;
    if (sun_r < 4) sun_r = 4;
    int sun_x = screen_w * 82 / 100;
    int sun_y = screen_h * 14 / 100;
    draw_filled_circle(sun_x, sun_y, sun_r + sun_r / 2, GFX_SUN_GLOW);
    draw_filled_circle(sun_x, sun_y, sun_r, GFX_SUN_YELLOW);

    draw_cloud(screen_w * 20 / 100, screen_h * 12 / 100, sun_r / 2 + 2);
    draw_cloud(screen_w * 45 / 100, screen_h * 8  / 100, sun_r / 2 + 1);

    draw_hill_layer(screen_w, screen_h, hill_back,
                     sizeof(hill_back) / sizeof(hill_back[0]), GFX_HILL_DARK);
    draw_hill_layer(screen_w, screen_h, hill_front,
                     sizeof(hill_front) / sizeof(hill_front[0]), GFX_HILL_LIGHT);
}

static void draw_watermark(int x, int y, const char *text) {
    hal_gfx_draw_string_transparent(x + 1, y + 1, text, GFX_BLACK);
    hal_gfx_draw_string_transparent(x, y, text, GFX_CLOUD_WHITE);
}

/* ---------------- Windows 11 палитра и обои Bloom ---------------- */

static int last_xp_w = -1, last_xp_h = -1;
static int palette_mode_win11 = 0;

static void build_win11_palette(uint8_t *rgb /* 224 * 3 */) {
    static const uint8_t ui_colors[16 * 3] = {
        243, 245, 249,   /* 32 GFX_W11_LIGHT_BG   */
        252, 253, 255,   /* 33 GFX_W11_LIGHT_TERM */
         24,  27,  34,   /* 34 GFX_W11_LIGHT_TEXT */
        107, 114, 128,   /* 35 GFX_W11_LIGHT_DIM  */
         32,  34,  40,   /* 36 GFX_W11_DARK_BG    */
         18,  20,  26,   /* 37 GFX_W11_DARK_TERM  */
        240, 244, 250,   /* 38 GFX_W11_DARK_TEXT  */
        142, 150, 164,   /* 39 GFX_W11_DARK_DIM   */
          0, 103, 192,   /* 40 GFX_W11_ACCENT     */
         96, 205, 255,   /* 41 GFX_W11_ACCENT_LT  */
        233, 239, 247,   /* 42 GFX_W11_TASKBAR_LT */
         28,  32,  41,   /* 43 GFX_W11_TASKBAR_DK */
        198, 206, 219,   /* 44 GFX_W11_BORDER_LT  */
         59,  64,  76,   /* 45 GFX_W11_BORDER_DK  */
        196,  43,  28,   /* 46 GFX_W11_CLOSE_RED  */
          8,   9,  12    /* 47 GFX_UI_SHADOW      */
    };

    int idx = 0;
    for (int i = 0; i < 16 * 3; i++)
        rgb[idx++] = ui_colors[i];

    /* 48..63: 16 оттенков нейтрально-сланцевой шкалы (тени и Mica) */
    for (int i = 0; i < 16; i++) {
        int r = 10 + i * 240 / 15;
        int g = 12 + i * 240 / 15;
        int b = 16 + i * 239 / 15;
        rgb[idx++] = (uint8_t)r;
        rgb[idx++] = (uint8_t)g;
        rgb[idx++] = (uint8_t)b;
    }

    /* 64..79: 16 оттенков ночного синего индиго (тёмный Bloom) */
    for (int i = 0; i < 16; i++) {
        rgb[idx++] = (uint8_t)(6 + i * 32 / 15);
        rgb[idx++] = (uint8_t)(12 + i * 96 / 15);
        rgb[idx++] = (uint8_t)(28 + i * 212 / 15);
    }

    /* 80..95: 16 оттенков светлого неба и лазури (светлый Bloom) */
    for (int i = 0; i < 16; i++) {
        rgb[idx++] = (uint8_t)(210 - i * 188 / 15);
        rgb[idx++] = (uint8_t)(232 - i * 144 / 15);
        rgb[idx++] = (uint8_t)(252 - i * 44 / 15);
    }

    /* 96..111: 16 оттенков яркого голубого/циана (блики лепестков) */
    for (int i = 0; i < 16; i++) {
        rgb[idx++] = (uint8_t)(16 + i * 146 / 15);
        rgb[idx++] = (uint8_t)(72 + i * 156 / 15);
        rgb[idx++] = (uint8_t)(180 + i * 75 / 15);
    }

    /* 112..255: 144 цвета равномерного 6x6x4 RGB-куба */
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 4; b++) {
                rgb[idx++] = (uint8_t)(r * 51);
                rgb[idx++] = (uint8_t)(g * 51);
                rgb[idx++] = (uint8_t)(b * 85);
            }
        }
    }
}

void gui_theme_sync_palette(int theme) {
    int want_win11 = (theme == GUI_THEME_WIN11_LIGHT || theme == GUI_THEME_WIN11_DARK);
    if (want_win11) {
        if (!palette_mode_win11) {
            uint8_t pal[WALLPAPER_PALETTE_COLORS * 3];
            build_win11_palette(pal);
            hal_gfx_set_image_palette(pal, WALLPAPER_PALETTE_COLORS, WALLPAPER_PALETTE_START);
            palette_mode_win11 = 1;
            last_xp_w = -1;
            last_xp_h = -1;
        }
    } else {
        if (palette_mode_win11) {
            palette_init();
            hal_gfx_set_image_palette((const uint8_t *)&g_palette[WALLPAPER_PALETTE_START],
                                      WALLPAPER_PALETTE_COLORS, WALLPAPER_PALETTE_START);
            palette_mode_win11 = 0;
            last_xp_w = -1;
            last_xp_h = -1;
        }
    }
}

#define WIN11_CACHE_MAX_W 800
#define WIN11_CACHE_MAX_H 600
static uint32_t win11_wp_cache[WIN11_CACHE_MAX_W * WIN11_CACHE_MAX_H];
static int win11_cache_w = -1, win11_cache_h = -1, win11_cache_dark = -1;

static uint32_t rgb_lerp(uint32_t a, uint32_t b, int t, int n) {
    if (t <= 0) return a;
    if (t >= n) return b;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return GFX_RGB(ar + (br - ar) * t / n,
                   ag + (bg - ag) * t / n,
                   ab + (bb - ab) * t / n);
}

static uint32_t rgb_blend(uint32_t dst, uint32_t src, int alpha255) {
    if (alpha255 <= 0) return dst;
    if (alpha255 >= 255) return src;
    int na = 255 - alpha255;
    int r = (((src >> 16) & 255) * alpha255 + ((dst >> 16) & 255) * na) / 255;
    int g = (((src >> 8)  & 255) * alpha255 + ((dst >> 8)  & 255) * na) / 255;
    int b = ((src         & 255) * alpha255 + (dst         & 255) * na) / 255;
    return GFX_RGB(r, g, b);
}

typedef struct {
    int ox, oy;
    int cos_a, sin_a;
    int rx, ry;
    int bend;
    uint32_t col_in;
    uint32_t col_rim;
    int alpha_max;
} bloom_petal_t;

static uint32_t eval_win11_pixel(int x, int y, int w, int h, int dark) {
    /* Фоновый градиент: диагонально-радиальный */
    int tx = w > 1 ? x * 256 / (w - 1) : 0;
    int ty = h > 1 ? y * 256 / (h - 1) : 0;
    int diag = (tx + ty) >> 1;

    uint32_t bg_top = dark ? GFX_RGB(5, 11, 26)   : GFX_RGB(214, 233, 252);
    uint32_t bg_bot = dark ? GFX_RGB(10, 22, 52)  : GFX_RGB(196, 222, 248);
    uint32_t col = rgb_lerp(bg_top, bg_bot, diag, 256);

    int cx = w / 2;
    int cy = h * 47 / 100;
    int base = (w < (h * 4 / 3)) ? w : (h * 4 / 3);
    int r_ref = base * 42 / 100;
    if (r_ref < 24) r_ref = 24;

    int dx = (x - cx) * 256 / r_ref;
    int dy = (y - cy) * 256 / r_ref;
    int dist2 = dx * dx + dy * dy;

    /* Мягкое ореольное свечение вокруг сердцевины цветка */
    if (dist2 < 340 * 340) {
        int glow = 255 - dist2 * 255 / (340 * 340);
        uint32_t glow_col = dark ? GFX_RGB(20, 62, 155) : GFX_RGB(162, 212, 255);
        col = rgb_blend(col, glow_col, glow * (dark ? 110 : 95) / 255);
    }

    if (dist2 > 460 * 460)
        return col;

    static const bloom_petal_t petals_light[5] = {
        { -78,  34,  110, -231, 185, 112,  36, GFX_RGB(11,  68, 188), GFX_RGB(112, 206, 255), 215 },
        { -36, -54,  218, -134, 195, 108,  32, GFX_RGB(22,  98, 222), GFX_RGB(145, 222, 255), 220 },
        {  34, -64,  248,   64, 200, 110, -28, GFX_RGB(16,  86, 214), GFX_RGB(128, 216, 255), 225 },
        {  82,  14,  160,  200, 188, 106, -34, GFX_RGB(30, 126, 240), GFX_RGB(168, 232, 255), 220 },
        {  14,  68,  -30,  254, 178, 116,  24, GFX_RGB(14,  58, 168), GFX_RGB( 96, 194, 255), 230 }
    };
    static const bloom_petal_t petals_dark[5] = {
        { -78,  34,  110, -231, 185, 112,  36, GFX_RGB(10,  40, 122), GFX_RGB( 64, 174, 255), 225 },
        { -36, -54,  218, -134, 195, 108,  32, GFX_RGB(16,  66, 176), GFX_RGB( 90, 198, 255), 230 },
        {  34, -64,  248,   64, 200, 110, -28, GFX_RGB(14,  58, 164), GFX_RGB( 78, 192, 255), 235 },
        {  82,  14,  160,  200, 188, 106, -34, GFX_RGB(24,  92, 214), GFX_RGB(116, 218, 255), 230 },
        {  14,  68,  -30,  254, 178, 116,  24, GFX_RGB( 8,  36, 110), GFX_RGB( 56, 162, 248), 235 }
    };

    const bloom_petal_t *petals = dark ? petals_dark : petals_light;

    for (int i = 0; i < 5; i++) {
        const bloom_petal_t *p = &petals[i];
        int px = dx - p->ox;
        int py = dy - p->oy;
        int u = (px * p->cos_a + py * p->sin_a) >> 8;
        int v = (-px * p->sin_a + py * p->cos_a) >> 8;
        v -= (p->bend * u * u) >> 16;

        int d2 = (u * u * 1024) / (p->rx * p->rx) + (v * v * 1024) / (p->ry * p->ry);
        if (d2 >= 1024) continue;

        /* Шёлковый градиент внутри лепестка: светлее к внешнему краю */
        int rim = d2 * 256 / 1024;
        if (v > 0) rim += v * 100 / p->ry;
        if (rim > 256) rim = 256;
        if (rim < 0) rim = 0;
        uint32_t pcol = rgb_lerp(p->col_in, p->col_rim, rim, 256);

        int a = p->alpha_max;
        if (d2 > 760)
            a = a * (1024 - d2) / (1024 - 760);
        if (u < -(p->rx / 2)) {
            int base_fade = (u + p->rx) * 255 / (p->rx / 2);
            if (base_fade < 0) base_fade = 0;
            if (base_fade < 255) a = a * base_fade / 255;
        }
        col = rgb_blend(col, pcol, a);
    }

    /* Тёмно-синий акцент в самой сердцевине лепестков для глубины */
    if (dist2 < 96 * 96) {
        int core = 255 - dist2 * 255 / (96 * 96);
        uint32_t core_col = dark ? GFX_RGB(6, 20, 64) : GFX_RGB(14, 62, 168);
        col = rgb_blend(col, core_col, core * 135 / 255);
    }

    return col;
}

static void draw_win11_bloom(int screen_w, int screen_h, int dark) {
    if (screen_w <= WIN11_CACHE_MAX_W && screen_h <= WIN11_CACHE_MAX_H) {
        if (win11_cache_w != screen_w || win11_cache_h != screen_h || win11_cache_dark != dark) {
            for (int y = 0; y < screen_h; y++) {
                uint32_t *row = &win11_wp_cache[(long)y * screen_w];
                for (int x = 0; x < screen_w; x++)
                    row[x] = eval_win11_pixel(x, y, screen_w, screen_h, dark);
            }
            win11_cache_w = screen_w;
            win11_cache_h = screen_h;
            win11_cache_dark = dark;
        }
        gfxfb_blit_rgb(0, 0, screen_w, screen_h, win11_wp_cache, screen_w);
    } else {
        for (int y = 0; y < screen_h; y++)
            for (int x = 0; x < screen_w; x++)
                gfxfb_put_rgb(x, y, eval_win11_pixel(x, y, screen_w, screen_h, dark));
    }
}

/* ---------------- основная точка входа ---------------- */

void gui_draw_wallpaper_themed(int screen_w, int screen_h, int theme) {
    gui_theme_sync_palette(theme);

    if (theme == GUI_THEME_WIN11_LIGHT || theme == GUI_THEME_WIN11_DARK) {
        draw_win11_bloom(screen_w, screen_h, theme == GUI_THEME_WIN11_DARK);
        return;
    }

    const uint8_t *pixels = 0;
    const uint8_t *palette = 0;

    if (screen_w == 320 && screen_h == 200) {
        pixels = wallpaper_320x200_pixels;
        palette = wallpaper_320x200_palette;
    } else if (screen_w == 640 && screen_h == 480) {
        pixels = wallpaper_640x480_pixels;
        palette = wallpaper_640x480_palette;
    } else if (screen_w == 800 && screen_h == 600) {
        pixels = wallpaper_800x600_pixels;
        palette = wallpaper_800x600_palette;
    }

    if (pixels) {
        if (screen_w != last_xp_w || screen_h != last_xp_h) {
            hal_gfx_set_image_palette(palette, WALLPAPER_PALETTE_COLORS, WALLPAPER_PALETTE_START);
            last_xp_w = screen_w;
            last_xp_h = screen_h;
        }
        hal_gfx_blit(0, 0, screen_w, screen_h, pixels);
    } else {
        draw_procedural_fallback(screen_w, screen_h);
        last_xp_w = -1;
        last_xp_h = -1;
    }

    draw_watermark(4, 4, "SHHH, IT'S PROTOTYPE, DO NOT LEAK...");
}

void gui_draw_wallpaper(int screen_w, int screen_h) {
    gui_draw_wallpaper_themed(screen_w, screen_h, GUI_THEME_XP_LIGHT);
}
