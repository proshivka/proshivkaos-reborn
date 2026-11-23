/* gui/terminal_app.h — приложение "Терминал": root@proshivkaOS-промпт,
 * реальные директории (cd/mkdir/pwd/ls), команды clear/touch/cat/edit/
 * date/uptime/neofetch/help, цветной вывод (директории синим, ошибки
 * красным, промпт красным как у root).
 */
#ifndef GUI_TERMINAL_APP_H
#define GUI_TERMINAL_APP_H

#include "gconsole.h"
#include "ramfs.h"

#define TERM_LINE_BUF   100   /* было 48: длинная команда молча обрезалась */
#define TERM_HOME_DIR   "/"

typedef struct terminal_app {
    gconsole_t console;
    char line[TERM_LINE_BUF];   /* UTF-8; line_len — в БАЙТАХ */
    int line_len;

    char cwd[RAMFS_MAX_PATH_LEN];

    int in_edit_mode;
    char edit_filename[RAMFS_MAX_PATH_LEN];
    char edit_buf[RAMFS_MAX_FILE_SIZE];
    size_t edit_total;

    /* Команда выполняется (сетевые команды ждут ответа). Пока busy,
     * клавиши не попадают в строку ввода, кроме Ctrl+C — он ставит cancel. */
    int busy;
    int cancel;

    /* «Сырой» режим: пока busy, клавиши (в т.ч. Ctrl+C) складываются в очередь
     * и читаются командой через terminal_app_raw_getc(). Нужен для ssh. */
    int raw;
    uint8_t rq[64];
    unsigned rq_head, rq_tail;
} terminal_app_t;

void terminal_app_init(terminal_app_t *app, const gui_window_t *win);

/* То же самое, но с явными цветами. Нужно тач-сборке: там тема тёмная, а
 * приветственная строка и первый промпт печатаются ВНУТРИ инициализации —
 * если менять цвета после неё, эти символы остаются чёрными на чёрном
 * (цвет запоминается у каждого символа отдельно, см. gconsole_t.color). */
void terminal_app_init_colors(terminal_app_t *app, const gui_window_t *win,
                               uint8_t fg, uint8_t bg);
/* Ввод. Строка хранится в UTF-8: терминал принимает и кириллицу.
 *   handle_key  — символ по коду Юникода (экранная клавиатура тач-сборки).
 *                 Коды HAL_KEY_* (0x100..0x10F: стрелки и т.п.) игнорируются.
 *   handle_char — старый вход для байтового ASCII (десктопная клавиатура). */
void terminal_app_handle_key(terminal_app_t *app, int key);
void terminal_app_handle_char(terminal_app_t *app, char c);

/* Внешние команды. Терминал сам про них ничего не знает: если команда не
 * встроенная, он спрашивает обработчик. Нужен, чтобы сетевой стек
 * подключался только там, где он есть (в тач-сборке обработчика нет). */
#define TERM_MAX_ARGS 12
typedef int (*terminal_ext_cmd_fn)(terminal_app_t *app, int argc, char *argv[]);
void terminal_app_set_ext_command(terminal_ext_cmd_fn fn);
void terminal_app_set_ext_help(const char *text);   /* строка для команды help */

/* Пользователь нажал Ctrl+C, пока выполнялась команда. */
int  terminal_app_cancel_requested(const terminal_app_t *app);

/* Сырой ввод: включить/выключить и забрать клавишу (-1 — очередь пуста). */
void terminal_app_set_raw(terminal_app_t *app, int on);
int  terminal_app_raw_getc(terminal_app_t *app);

#endif
