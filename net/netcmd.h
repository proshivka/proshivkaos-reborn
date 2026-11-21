/* net/netcmd.h — сетевые команды (ifconfig, arping, ping, dhcp, udpsend,
 * nslookup, http), написанные один раз и не знающие, куда выводят текст:
 * вывод идёт через net_out_t. Графический терминал подставляет в него
 * gconsole, текстовый шелл может подставить hal_console_putc. */
#ifndef NET_NETCMD_H
#define NET_NETCMD_H

#include <stddef.h>

#define NET_STYLE_NORMAL 0
#define NET_STYLE_ERROR  1

typedef struct net_out {
    void (*putc)(void *ctx, char c);
    void (*style)(void *ctx, int style);   /* NET_STYLE_*, можно NULL */
    void (*flush)(void *ctx);              /* после каждой строки, можно NULL */
    void *ctx;
    int   lead_nl;   /* 1: перевод строки ПЕРЕД каждой строкой (как в GUI-терминале);
                        0: ПОСЛЕ каждой строки (как в текстовом шелле) */

    /* Ввод — нужен только команде ssh (пароль, подтверждение ключа, сеанс).
     * Без getkey команда ssh отвечает, что клавиатура недоступна. */
    int  (*getkey)(void *ctx);                 /* код клавиши или -1, если нажатия нет */
    void (*raw_mode)(void *ctx, int on);       /* on=1: клавиши (в т.ч. Ctrl+C) идут команде; 0 — обратно */
} net_out_t;

/* Имена команд (для справки и автодополнения). */
size_t      netcmd_count(void);
const char *netcmd_name(size_t i);
int         netcmd_is(const char *name);

/* Выполняет команду, если argv[0] — одна из сетевых. Возвращает 1, если
 * команда была сетевой (и выполнена), иначе 0 и ничего не выводит. */
int netcmd_exec(const net_out_t *out, int argc, char *argv[]);

#endif
