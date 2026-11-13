/* arch/switcher/hal_switcher.c — аппаратный уровень для proshivkaOS Switcher
 * на Android / Linux (Galaxy A15, Mi 10 Ultra, Generic AArch64).
 *
 * Работает напрямую с фреймбуфером ядра (/dev/graphics/fb0 или /dev/fb0)
 * и подсистемой ввода evdev (/dev/input/event*), пока SurfaceFlinger
 * временно остановлен скриптом launch_proshivka.sh.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "../../hal/hal.h"
#include "../../hal/hal_time.h"
#include "../../hal/hal_input.h"
#include "../../gui/gfxfb.h"
#include "../../gui/hal_gfx.h"
#include "../../gui/palette.h"

/* ---------------- Состояние выхода ---------------- */
static volatile sig_atomic_t g_should_exit = 0;

void hal_request_exit(void) {
    g_should_exit = 1;
}

int hal_should_exit(void) {
    /* Также проверяем файл запроса выхода от Android Switcher */
    if (access("/exit_proshivka", F_OK) == 0 ||
        access("/data/local/tmp/exit_proshivka", F_OK) == 0 ||
        access("/exit_lowyk", F_OK) == 0 ||
        access("/data/local/tmp/exit_lowyk", F_OK) == 0) {
        g_should_exit = 1;
    }
    return g_should_exit;
}

static void sig_handler(int sig) {
    (void)sig;
    g_should_exit = 1;
}

/* ---------------- Консоль и отладка ---------------- */

void hal_console_init(void) {
    /* Не буферизировать вывод для мгновенной диагностики через adb / log */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

void hal_console_putc(char c) {
    putchar(c);
}

void hal_console_write(const char *s) {
    if (s) fputs(s, stdout);
}

int hal_console_getc(void) {
    return -1;
}

char hal_console_getc_blocking(void) {
    return (char)getchar();
}

void hal_console_set_color(uint8_t fg, uint8_t bg) {
    (void)fg; (void)bg;
}

void hal_console_reset_color(void) {}

void hal_console_clear(void) {
    fputs("\033[2J\033[H", stdout);
}

void hal_console_goto(int row, int col) {
    printf("\033[%d;%dH", row + 1, col + 1);
}

void hal_debug_mark(int id, uint8_t r, uint8_t g, uint8_t b) {
    (void)id; (void)r; (void)g; (void)b;
}

void hal_debug_text(const char *s) {
    if (s) fputs(s, stdout);
}

void hal_debug_progress(void) {}
void hal_debug_boot_done(void) {}
void hal_debug_activity(int id) { (void)id; }

/* ---------------- Сведения о платформе ---------------- */

static char g_device_model[64] = "Samsung Galaxy A15";
static char g_cpu_name[64]     = "Helio G99 Octa-Core";
static char g_board_name[64]   = "Galaxy A15 (MT6789)";

static void detect_hardware(void) {
    /* Пытаемся прочитать модель устройства из /proc/device-tree/model */
    FILE *f = fopen("/proc/device-tree/model", "r");
    if (f) {
        if (fgets(g_device_model, sizeof(g_device_model), f)) {
            size_t len = strlen(g_device_model);
            if (len > 0 && g_device_model[len - 1] == '\n') g_device_model[len - 1] = '\0';
        }
        fclose(f);
    } else {
        /* Проверяем /system/build.prop или getprop */
        f = fopen("/system/build.prop", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "ro.product.model=", 17) == 0) {
                    strncpy(g_device_model, line + 17, sizeof(g_device_model) - 1);
                    size_t len = strlen(g_device_model);
                    if (len > 0 && g_device_model[len - 1] == '\n') g_device_model[len - 1] = '\0';
                    break;
                }
            }
            fclose(f);
        }
    }

    /* Читаем /proc/cpuinfo */
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Hardware\t:", 10) == 0 || strncmp(line, "Hardware:", 9) == 0) {
                char *p = strchr(line, ':');
                if (p) {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;
                    strncpy(g_cpu_name, p, sizeof(g_cpu_name) - 1);
                    size_t len = strlen(g_cpu_name);
                    if (len > 0 && g_cpu_name[len - 1] == '\n') g_cpu_name[len - 1] = '\0';
                }
                break;
            }
        }
        fclose(f);
    }
}

uint64_t arch_cycles_per_second(void) {
    uint64_t frq = 0;
#if defined(__aarch64__)
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
#endif
    return frq > 0 ? frq : 2200000000ULL;
}

const char *hal_arch_name(void) {
    return "ARM64";
}

const char *hal_cpu_name(void) {
    return g_cpu_name;
}

const char *hal_board_name(void) {
    return g_device_model;
}

const char *hal_platform_device_model(void) {
    return g_device_model;
}

void hal_arch_init(void *boot_info) {
    (void)boot_info;
    detect_hardware();
}

void hal_panic(const char *msg) {
    fprintf(stderr, "\n[FATAL PANIC] proshivkaOS: %s\n", msg ? msg : "unknown");
    exit(1);
}

void hal_cpu_halt(void) {
    exit(0);
}

void hal_reboot(void) {
    exit(0);
}

void hal_shutdown(void) {
    exit(0);
}

void hal_parallel(void (*fn)(void *arg, int part, int parts), void *arg) {
    if (fn) fn(arg, 0, 1);
}

/* ---------------- Память ---------------- */

void hal_mem_init(void) {}

void *hal_mem_alloc(size_t size) {
    return malloc(size);
}

void hal_mem_free(void *ptr) {
    free(ptr);
}

size_t hal_mem_used(void) {
    /* Читаем из /proc/meminfo */
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 256 * 1024 * 1024;
    char line[128];
    long total_kb = 0, free_kb = 0, avail_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line + 9, "%ld", &total_kb);
        else if (strncmp(line, "MemFree:", 8) == 0) sscanf(line + 8, "%ld", &free_kb);
        else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line + 13, "%ld", &avail_kb);
    }
    fclose(f);
    if (avail_kb > 0 && total_kb > 0) return (size_t)(total_kb - avail_kb) * 1024;
    if (total_kb > 0 && free_kb > 0) return (size_t)(total_kb - free_kb) * 1024;
    return 512 * 1024 * 1024;
}

size_t hal_mem_total(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 4096UL * 1024 * 1024;
    char line[128];
    long total_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%ld", &total_kb);
            break;
        }
    }
    fclose(f);
    if (total_kb > 0) return (size_t)total_kb * 1024;
    return 4096UL * 1024 * 1024;
}

/* ---------------- Время и таймеры ---------------- */

static struct timespec g_t0;

void hal_time_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
}

uint64_t hal_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t sec = (uint64_t)(ts.tv_sec - g_t0.tv_sec);
    int64_t nsec = ts.tv_nsec - g_t0.tv_nsec;
    return sec * 1000 + (uint64_t)(nsec / 1000000);
}

uint64_t hal_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t sec = (uint64_t)(ts.tv_sec - g_t0.tv_sec);
    int64_t nsec = ts.tv_nsec - g_t0.tv_nsec;
    return sec * 1000000 + (uint64_t)(nsec / 1000);
}

void hal_time_set_wall(uint32_t unix_utc, int tz_minutes) {
    (void)unix_utc; (void)tz_minutes;
}

void hal_time_rtc(int *year, int *month, int *day, int *hour, int *min, int *sec) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    if (year)  *year  = tm_buf.tm_year + 1900;
    if (month) *month = tm_buf.tm_mon + 1;
    if (day)   *day   = tm_buf.tm_mday;
    if (hour)  *hour  = tm_buf.tm_hour;
    if (min)   *min   = tm_buf.tm_min;
    if (sec)   *sec   = tm_buf.tm_sec;
}

void hal_time_delay_ms(uint32_t ms) {
    usleep(ms * 1000);
}

void hal_time_delay_us(uint32_t us) {
    usleep(us);
}

void hal_background_poll(void) {}
void hal_usb_pump(void) {}
void hal_watchdog_pet(void) {}

/* ---------------- Дисплей и Фреймбуфер ---------------- */

static int g_fb_fd = -1;
static uint8_t *g_fb_mmap = NULL;
static size_t g_fb_mmap_size = 0;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;

static void set_backlight(void) {
    static const char *const bl_paths[] = {
        "/sys/class/leds/lcd-backlight/brightness",
        "/sys/devices/platform/soc/soc:mtk-leds/leds/lcd-backlight/brightness",
        "/sys/class/backlight/panel0-backlight/brightness",
        "/sys/class/leds/panel0-backlight/brightness"
    };
    for (size_t i = 0; i < sizeof(bl_paths)/sizeof(bl_paths[0]); i++) {
        int fd = open(bl_paths[i], O_WRONLY);
        if (fd >= 0) {
            /* Выставляем яркий комфортный уровень */
            write(fd, "255\n", 4);
            close(fd);
            break;
        }
    }
}

void hal_gfx_init(void) {
    const char *fb_devs[] = { "/dev/graphics/fb0", "/dev/fb0" };
    for (size_t i = 0; i < sizeof(fb_devs)/sizeof(fb_devs[0]); i++) {
        g_fb_fd = open(fb_devs[i], O_RDWR);
        if (g_fb_fd >= 0) {
            printf("[HAL] Opened framebuffer: %s\n", fb_devs[i]);
            break;
        }
    }

    if (g_fb_fd < 0) {
        fprintf(stderr, "[HAL] WARNING: Cannot open fb0, running dummy headless\n");
        /* Создаём буфер в памяти 1080x2340 для тестов */
        int w = 1080, h = 2340;
        g_fb_mmap_size = w * h * 4;
        g_fb_mmap = (uint8_t *)calloc(1, g_fb_mmap_size);
        palette_init();
        gfxfb_bind(g_fb_mmap, w, h, w * 4, GFXFB_FMT_XRGB32);
        return;
    }

    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) < 0) {
        perror("[HAL] ioctl FBIOGET_VSCREENINFO failed");
    }
    if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) < 0) {
        perror("[HAL] ioctl FBIOGET_FSCREENINFO failed");
    }

    /* Unblank экран если был выключен */
    ioctl(g_fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
    set_backlight();

    int w = g_vinfo.xres;
    int h = g_vinfo.yres;
    int bpp = g_vinfo.bits_per_pixel;
    int pitch = g_finfo.line_length > 0 ? g_finfo.line_length : w * (bpp / 8);

    g_fb_mmap_size = g_finfo.smem_len > 0 ? g_finfo.smem_len : (size_t)h * pitch;
    g_fb_mmap = (uint8_t *)mmap(NULL, g_fb_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);

    if (g_fb_mmap == MAP_FAILED) {
        perror("[HAL] mmap failed");
        g_fb_mmap = NULL;
        return;
    }

    int fmt = GFXFB_FMT_XRGB32;
    if (bpp == 24) fmt = GFXFB_FMT_RGB24;
    else if (bpp == 8) fmt = GFXFB_FMT_PAL8;

    printf("[HAL] Display bound: %dx%d @ %dbpp (pitch=%d, fmt=%d)\n", w, h, bpp, pitch, fmt);

    palette_init();
    gfxfb_bind(g_fb_mmap, w, h, pitch, fmt);
}

int hal_gfx_set_resolution(int width, int height) {
    (void)width; (void)height;
    return 0;
}

/* ---------------- Ввод: Touchscreen & Hardware Keys ---------------- */

#define MAX_INPUT_DEVS 16

typedef struct {
    int fd;
    int is_touch;
    int min_x, max_x;
    int min_y, max_y;
    int cur_x, cur_y;
    int is_down;
} input_dev_t;

static input_dev_t g_inputs[MAX_INPUT_DEVS];
static int g_num_inputs = 0;
static int g_screen_w = 1080;
static int g_screen_h = 2340;

/* Очередь событий ввода для плавного потребления */
#define EV_QUEUE_SIZE 64
static hal_input_event_t g_ev_queue[EV_QUEUE_SIZE];
static int g_ev_head = 0, g_ev_tail = 0;

static void queue_push(const hal_input_event_t *ev) {
    int next = (g_ev_head + 1) % EV_QUEUE_SIZE;
    if (next != g_ev_tail) {
        g_ev_queue[g_ev_head] = *ev;
        g_ev_head = next;
    }
}

static int queue_pop(hal_input_event_t *ev) {
    if (g_ev_head == g_ev_tail) return 0;
    *ev = g_ev_queue[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % EV_QUEUE_SIZE;
    return 1;
}

void hal_input_init(void) {
    g_num_inputs = 0;
    g_ev_head = g_ev_tail = 0;

    for (int i = 0; i < 32 && g_num_inputs < MAX_INPUT_DEVS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long evbit = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);

        input_dev_t *dev = &g_inputs[g_num_inputs];
        dev->fd = fd;
        dev->is_touch = 0;
        dev->min_x = 0; dev->max_x = 1080;
        dev->min_y = 0; dev->max_y = 2340;
        dev->cur_x = 0; dev->cur_y = 0;
        dev->is_down = 0;

        if (evbit & (1 << EV_ABS)) {
            struct input_absinfo abs;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) >= 0 && abs.maximum > 0) {
                dev->is_touch = 1;
                dev->min_x = abs.minimum;
                dev->max_x = abs.maximum;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) >= 0 && abs.maximum > 0) {
                    dev->min_y = abs.minimum;
                    dev->max_y = abs.maximum;
                }
            } else if (ioctl(fd, EVIOCGABS(ABS_X), &abs) >= 0 && abs.maximum > 0) {
                dev->is_touch = 1;
                dev->min_x = abs.minimum;
                dev->max_x = abs.maximum;
                if (ioctl(fd, EVIOCGABS(ABS_Y), &abs) >= 0 && abs.maximum > 0) {
                    dev->min_y = abs.minimum;
                    dev->max_y = abs.maximum;
                }
            }
        }

        char name[128] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        printf("[HAL] Input %s: %s (touch=%d, X=[%d..%d], Y=[%d..%d])\n",
               path, name, dev->is_touch, dev->min_x, dev->max_x, dev->min_y, dev->max_y);

        g_num_inputs++;
    }
}

void hal_input_set_screen(int w, int h) {
    if (w > 0) g_screen_w = w;
    if (h > 0) g_screen_h = h;
}

int hal_input_has_cursor(void) {
    return 0; /* На телефоне курсор не нужен — чистый тачскрин */
}

void hal_input_pointer_pos(int *x, int *y) {
    if (x) *x = 0;
    if (y) *y = 0;
}

int hal_input_available(void) {
    return g_num_inputs > 0;
}

/* Опрос evdev */
static void poll_evdev_devices(void) {
    struct input_event evs[32];
    static int s_vol_up = 0, s_vol_down = 0;

    for (int d = 0; d < g_num_inputs; d++) {
        input_dev_t *dev = &g_inputs[d];
        int rd = read(dev->fd, evs, sizeof(evs));
        if (rd <= 0) continue;

        int count = rd / sizeof(struct input_event);
        int syn = 0;

        for (int i = 0; i < count; i++) {
            struct input_event *e = &evs[i];

            if (e->type == EV_KEY) {
                if (e->code == BTN_TOUCH) {
                    if (e->value == 1 && !dev->is_down) {
                        dev->is_down = 1;
                        hal_input_event_t out = {
                            .type = HAL_EV_POINTER_DOWN,
                            .x = dev->cur_x,
                            .y = dev->cur_y,
                            .pressed = 1
                        };
                        queue_push(&out);
                    } else if (e->value == 0 && dev->is_down) {
                        dev->is_down = 0;
                        hal_input_event_t out = {
                            .type = HAL_EV_POINTER_UP,
                            .x = dev->cur_x,
                            .y = dev->cur_y,
                            .pressed = 0
                        };
                        queue_push(&out);
                    }
                } else if (e->code == KEY_VOLUMEUP) {
                    s_vol_up = e->value;
                    if (e->value == 1) {
                        /* ESC для навигации назад */
                        hal_input_event_t out = { .type = HAL_EV_KEY, .key = 27 };
                        queue_push(&out);
                    }
                } else if (e->code == KEY_VOLUMEDOWN) {
                    s_vol_down = e->value;
                } else if (e->code == KEY_POWER && e->value == 1) {
                    /* Кнопка питания — переход Домой */
                    hal_input_event_t out = { .type = HAL_EV_KEY, .key = 27 };
                    queue_push(&out);
                }

                /* Экстренный выход: одновременное зажатие громкость вверх + вниз */
                if (s_vol_up && s_vol_down) {
                    printf("[HAL] Emergency exit combination pressed!\n");
                    hal_request_exit();
                }
            } else if (e->type == EV_ABS) {
                int span_x = dev->max_x - dev->min_x;
                int span_y = dev->max_y - dev->min_y;
                if (span_x <= 0) span_x = 1;
                if (span_y <= 0) span_y = 1;

                if (e->code == ABS_MT_POSITION_X || e->code == ABS_X) {
                    int raw = e->value - dev->min_x;
                    dev->cur_x = (int)(((long)raw * g_screen_w) / span_x);
                    if (dev->cur_x < 0) dev->cur_x = 0;
                    if (dev->cur_x >= g_screen_w) dev->cur_x = g_screen_w - 1;
                } else if (e->code == ABS_MT_POSITION_Y || e->code == ABS_Y) {
                    int raw = e->value - dev->min_y;
                    dev->cur_y = (int)(((long)raw * g_screen_h) / span_y);
                    if (dev->cur_y < 0) dev->cur_y = 0;
                    if (dev->cur_y >= g_screen_h) dev->cur_y = g_screen_h - 1;
                } else if (e->code == ABS_MT_TRACKING_ID) {
                    if (e->value >= 0 && !dev->is_down) {
                        dev->is_down = 1;
                        hal_input_event_t out = {
                            .type = HAL_EV_POINTER_DOWN,
                            .x = dev->cur_x,
                            .y = dev->cur_y,
                            .pressed = 1
                        };
                        queue_push(&out);
                    } else if (e->value < 0 && dev->is_down) {
                        dev->is_down = 0;
                        hal_input_event_t out = {
                            .type = HAL_EV_POINTER_UP,
                            .x = dev->cur_x,
                            .y = dev->cur_y,
                            .pressed = 0
                        };
                        queue_push(&out);
                    }
                }
            } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                syn = 1;
            }
        }

        if (syn && dev->is_down) {
            hal_input_event_t out = {
                .type = HAL_EV_POINTER_MOVE,
                .x = dev->cur_x,
                .y = dev->cur_y,
                .pressed = 1
            };
            queue_push(&out);
        }
    }
}

int hal_input_poll(hal_input_event_t *ev) {
    /* Если в очереди есть готовые события, отдаём их */
    if (queue_pop(ev)) return 1;

    /* Иначе опрашиваем устройства ядра */
    poll_evdev_devices();
    return queue_pop(ev);
}

/* ---------------- Главная точка входа Switcher /init ---------------- */

extern void touch_main(void);

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, sig_handler);

    printf("===================================================\n");
    printf("   proshivkaOS Reborn — Android Switcher Edition   \n");
    printf("   Target: Samsung Galaxy A15 / Helio G99 / AArch64\n");
    printf("===================================================\n");

    hal_console_init();
    hal_time_init();
    hal_arch_init(NULL);

    /* Запускаем тач-интерфейс proshivkaOS */
    touch_main();

    /* При выходе: очищаем экран */
    if (g_fb_mmap) {
        memset(g_fb_mmap, 0, g_fb_mmap_size);
        munmap(g_fb_mmap, g_fb_mmap_size);
    }
    if (g_fb_fd >= 0) close(g_fb_fd);

    printf("[HAL] proshivkaOS Switcher gracefully exited. Restoring Android...\n");
    return 0;
}
