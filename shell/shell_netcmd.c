/* shell/shell_netcmd.c — мост между текстовой консолью и net/netcmd.c.
 *
 * netcmd пишет через net_out_t и читает клавиши через getkey; здесь они
 * привязываются к hal_console_putc / hal_console_getc. Без прерываний и
 * «насоса» событий (он нужен только окну GUI), поэтому ожидания в сети здесь
 * просто крутятся в цикле, как в остальных текстовых командах.
 */
#include <stddef.h>
#include "hal.h"
#include "netcmd.h"
#include "shell_netcmd.h"

static void out_putc(void *ctx, char c) {
    (void)ctx;
    hal_console_putc(c);
}

/* hal_console_getc() отдаёт код клавиши или значение <= 0, если нажатия нет;
 * стрелки и прочие HAL_KEY_* (> 255) для SSH не нужны — отбрасываем. */
static int out_getkey(void *ctx) {
    (void)ctx;
    int c = hal_console_getc();
    if (c <= 0 || c > 255) return -1;
    return c;
}

int shell_try_netcmd(int argc, char *argv[]) {
    if (argc < 1 || !netcmd_is(argv[0])) return 0;

    /* lead_nl = 0: строки заканчиваются переводом (стиль текстового шелла).
     * style = NULL: ошибки выводятся обычным цветом. raw_mode не нужен —
     * клавиши и так принимаются самой командой, а не шеллом. */
    net_out_t out = { out_putc, 0, 0, 0, 0, out_getkey, 0 };
    return netcmd_exec(&out, argc, argv);
}
