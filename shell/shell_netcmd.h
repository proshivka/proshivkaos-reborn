/* shell/shell_netcmd.h — подключает общие сетевые команды (net/netcmd.c)
 * к текстовой консоли. Сейчас так доступна команда ssh. */
#ifndef SHELL_NETCMD_H
#define SHELL_NETCMD_H

/* Выполняет argv как сетевую команду, если это одна из команд netcmd.
 * Возвращает 1, если команда была обработана, иначе 0 (ничего не выведено). */
int shell_try_netcmd(int argc, char *argv[]);

#endif
