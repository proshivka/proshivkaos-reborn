/* gui/terminal_app.c */
#include "terminal_app.h"
#include "hal_time.h"
#include "utf8.h"

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static void str_copy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Внешние команды (см. terminal_app.h). */
static terminal_ext_cmd_fn ext_cmd;
static const char *ext_help;

void terminal_app_set_ext_command(terminal_ext_cmd_fn fn) { ext_cmd = fn; }
void terminal_app_set_ext_help(const char *text)         { ext_help = text; }
int  terminal_app_cancel_requested(const terminal_app_t *app) { return app->cancel; }

void terminal_app_set_raw(terminal_app_t *app, int on) {
    app->raw = on;
    app->rq_head = app->rq_tail = 0;                 /* то, что набрано «до», не должно попасть в сеанс */
}

int terminal_app_raw_getc(terminal_app_t *app) {
    if (app->rq_head == app->rq_tail) return -1;
    uint8_t c = app->rq[app->rq_head % sizeof app->rq];
    app->rq_head++;
    return c;
}

static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

/* Резолвит аргумент-путь относительно текущей директории терминала:
 * "/abs/path" -> как есть; ".." -> родитель cwd; "." или "" -> cwd;
 * "~" -> домашняя директория; иначе -> cwd + "/" + arg. */
static void resolve_path(terminal_app_t *app, const char *arg, char *out, size_t max) {
    if (!arg || arg[0] == '\0') { str_copy(out, app->cwd, max); return; }

    if (arg[0] == '/') { str_copy(out, arg, max); return; }
    if (str_eq(arg, "~")) { str_copy(out, TERM_HOME_DIR, max); return; }
    if (str_eq(arg, ".")) { str_copy(out, app->cwd, max); return; }

    if (str_eq(arg, "..")) {
        size_t len = str_len(app->cwd);
        size_t last_slash = 0;
        int found = 0;
        for (size_t i = 0; i < len; i++)
            if (app->cwd[i] == '/') { last_slash = i; found = 1; }

        if (!found || last_slash == 0) str_copy(out, "/", max);
        else {
            size_t n = last_slash;
            if (n >= max) n = max - 1;
            for (size_t i = 0; i < n; i++) out[i] = app->cwd[i];
            out[n] = '\0';
        }
        return;
    }

    /* обычное имя — присоединяем к cwd */
    size_t cwd_len = str_len(app->cwd);
    if (cwd_len == 1 && app->cwd[0] == '/') {
        out[0] = '/';
        str_copy(out + 1, arg, max - 1);
    } else {
        str_copy(out, app->cwd, max);
        size_t n = str_len(out);
        if (n < max - 1) out[n++] = '/';
        out[n] = '\0';
        str_copy(out + n, arg, max - n);
    }
}

/* Показывает cwd в промпте с подстановкой "~" вместо домашней директории,
 * как в настоящем bash. */
static void write_prompt_path(terminal_app_t *app) {
    size_t home_len = str_len(TERM_HOME_DIR);

    if (str_eq(app->cwd, TERM_HOME_DIR)) {
        gconsole_write(&app->console, "~");
        return;
    }

    /* если cwd лежит внутри домашней директории — подставляем "~" */
    int under_home = 1;
    for (size_t i = 0; i < home_len; i++)
        if (app->cwd[i] != TERM_HOME_DIR[i]) { under_home = 0; break; }
    if (under_home && app->cwd[home_len] != '/') under_home = 0;

    if (under_home) {
        gconsole_write(&app->console, "~");
        gconsole_write(&app->console, app->cwd + home_len);
    } else {
        gconsole_write(&app->console, app->cwd);
    }
}

static void print_prompt(terminal_app_t *app) {
    gconsole_putc(&app->console, '\n');
    gconsole_set_ink(&app->console, GFX_LIGHT_RED);   /* root-промпт красным — конвенция */
    gconsole_write(&app->console, "root@proshivkaOS:");
    write_prompt_path(app);
    gconsole_write(&app->console, "# ");
    gconsole_set_ink(&app->console, app->console.fg);  /* дальше ввод обычным цветом */
}

static void print_error(terminal_app_t *app, const char *msg) {
    gconsole_set_ink(&app->console, GFX_LIGHT_RED);
    gconsole_putc(&app->console, '\n');
    gconsole_write(&app->console, msg);
    gconsole_set_ink(&app->console, app->console.fg);
}

static void write_padded(gconsole_t *gc, unsigned int v, int width) {
    char buf[12];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n < width) buf[n++] = '0';
    for (int i = n - 1; i >= 0; i--) gconsole_putc(gc, buf[i]);
}

static void write_uint(gconsole_t *gc, uint32_t v) {
    write_padded(gc, (unsigned int)v, 1);
}

static void write_size_human(gconsole_t *gc, uint32_t bytes) {
    if (bytes >= 1024u * 1024u) {
        write_uint(gc, bytes / (1024u * 1024u));
        gconsole_write(gc, "M");
    } else if (bytes >= 1024u) {
        write_uint(gc, bytes / 1024u);
        gconsole_write(gc, "K");
    } else {
        write_uint(gc, bytes);
        gconsole_write(gc, "B");
    }
}

static void ramfs_walk_stats(const char *dir, size_t *entries, uint32_t *bytes) {
    const ramfs_file_t *list[RAMFS_MAX_ENTRIES];
    size_t n = ramfs_list_dir(dir, list, RAMFS_MAX_ENTRIES);
    for (size_t i = 0; i < n; i++) {
        (*entries)++;
        if (list[i]->is_dir) {
            ramfs_walk_stats(list[i]->path, entries, bytes);
        } else {
            *bytes += (uint32_t)list[i]->size;
        }
    }
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} redir_cap_t;

static void redir_capture_putc(char c, void *ctx) {
    redir_cap_t *rc = (redir_cap_t *)ctx;
    if (rc->len < rc->cap) rc->buf[rc->len++] = c;
}

static char g_redir_buf[RAMFS_MAX_FILE_SIZE];
static char g_redir_merge[RAMFS_MAX_FILE_SIZE];

/* Ищет '>' или '>>' в строке команды. Возвращает:
 *   0 — перенаправления нет
 *   1 — найдено ('*' out_file, '*' append), строка обрезана на месте '>'
 *  -1 — синтаксическая ошибка (после '>' не указан файл) */
static int split_redirect(char *line, char **out_file, int *append) {
    for (char *p = line; *p; p++) {
        if (*p == '>') {
            int is_app = (p[1] == '>');
            *p = '\0';
            char *f = p + (is_app ? 2 : 1);
            while (*f == ' ') f++;
            if (!*f) return -1;
            *out_file = f;
            char *end = f;
            while (*end && *end != ' ') end++;
            *end = '\0';
            *append = is_app;
            return 1;
        }
    }
    return 0;
}

static void exec_command(terminal_app_t *app, char *line) {
    char *argv[TERM_MAX_ARGS];
    int argc = tokenize(line, argv, TERM_MAX_ARGS);
    if (argc == 0) return;

    char path[RAMFS_MAX_PATH_LEN];

    if (str_eq(argv[0], "ls")) {
        resolve_path(app, argc >= 2 ? argv[1] : ".", path, sizeof(path));
        if (!ramfs_is_dir(path)) {
            const ramfs_file_t *f = ramfs_lookup(path);
            if (!f) {
                print_error(app, "нет такого файла или каталога");
            } else {
                gconsole_putc(&app->console, '\n');
                gconsole_write(&app->console, f->name);
            }
        } else {
            const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
            size_t n = ramfs_list_dir(path, files, RAMFS_MAX_ENTRIES);
            if (n == 0) {
                gconsole_write(&app->console, "\n(пусто)");
            } else {
                for (size_t i = 0; i < n; i++) {
                    gconsole_putc(&app->console, '\n');
                    if (files[i]->is_dir) gconsole_set_ink(&app->console, GFX_LIGHT_BLUE);
                    gconsole_write(&app->console, files[i]->name);
                    if (files[i]->is_dir) gconsole_write(&app->console, "/");
                    gconsole_set_ink(&app->console, app->console.fg);
                }
            }
        }

    } else if (str_eq(argv[0], "pwd")) {
        gconsole_putc(&app->console, '\n');
        gconsole_write(&app->console, app->cwd);

    } else if (str_eq(argv[0], "cd")) {
        resolve_path(app, argc >= 2 ? argv[1] : "~", path, sizeof(path));
        if (!ramfs_is_dir(path)) {
            print_error(app, "нет такого каталога");
        } else {
            str_copy(app->cwd, path, sizeof(app->cwd));
        }

    } else if (str_eq(argv[0], "mkdir")) {
        if (argc < 2) { print_error(app, "использование: mkdir <каталог>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        ramfs_mkdir(path);

    } else if (str_eq(argv[0], "cat")) {
        if (argc < 2) { print_error(app, "использование: cat <файл>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        const ramfs_file_t *f = ramfs_lookup(path);
        if (!f || f->is_dir) { print_error(app, "не найдено"); return; }
        gconsole_putc(&app->console, '\n');
        uint8_t chunk[256];
        size_t off = 0;
        for (;;) {
            long n = ramfs_read(path, off, chunk, sizeof(chunk));
            if (n <= 0) break;
            for (long i = 0; i < n; i++) {
                uint8_t c = chunk[i];
                /* управляющие байты (бинарные файлы) — точкой, не в консоль */
                gconsole_putc(&app->console,
                              (c < 32 && c != '\n' && c != '\t') || c == 127 ? '.' : (char)c);
            }
            off += (size_t)n;
        }

    } else if (str_eq(argv[0], "touch")) {
        if (argc < 2) { print_error(app, "использование: touch <файл>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        ramfs_touch(path);
        gconsole_write(&app->console, "\nготово");

    } else if (str_eq(argv[0], "edit")) {
        if (argc < 2) { print_error(app, "использование: edit <файл>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        ramfs_touch(path);
        str_copy(app->edit_filename, path, sizeof(app->edit_filename));
        app->edit_total = 0;
        app->in_edit_mode = 1;
        gconsole_write(&app->console, "\n-- режим правки, строка '.' = сохранить --");

    } else if (str_eq(argv[0], "clear")) {
        gconsole_clear(&app->console);
        return;   /* без \n перед следующим промптом — экран и так пуст */

    } else if (str_eq(argv[0], "date")) {
        int y, mo, d, h, mi, s;
        hal_time_rtc(&y, &mo, &d, &h, &mi, &s);
        gconsole_putc(&app->console, '\n');
        write_padded(&app->console, (unsigned)y, 4);
        gconsole_putc(&app->console, '-');
        write_padded(&app->console, (unsigned)mo, 2);
        gconsole_putc(&app->console, '-');
        write_padded(&app->console, (unsigned)d, 2);
        gconsole_putc(&app->console, ' ');
        write_padded(&app->console, (unsigned)h, 2);
        gconsole_putc(&app->console, ':');
        write_padded(&app->console, (unsigned)mi, 2);
        gconsole_putc(&app->console, ':');
        write_padded(&app->console, (unsigned)s, 2);

    } else if (str_eq(argv[0], "uptime")) {
        unsigned long total = (unsigned long)(hal_time_ms() / 1000);
        unsigned int hh = (unsigned int)(total / 3600);
        unsigned int mm = (unsigned int)((total % 3600) / 60);
        unsigned int ss = (unsigned int)(total % 60);
        gconsole_putc(&app->console, '\n');
        write_padded(&app->console, hh, 2);
        gconsole_write(&app->console, "ч ");
        write_padded(&app->console, mm, 2);
        gconsole_write(&app->console, "м ");
        write_padded(&app->console, ss, 2);
        gconsole_write(&app->console, "с");

    } else if (str_eq(argv[0], "neofetch")) {
        gconsole_write(&app->console, "\nproshivkaOS NEXT ");
        gconsole_write(&app->console, PROSHIVKAOS_VERSION);
        gconsole_write(&app->console, "\nАРХ: ");
        gconsole_write(&app->console, hal_arch_name());
        gconsole_write(&app->console, "\nЦПУ: ");
        gconsole_write(&app->console, hal_cpu_name());
        gconsole_write(&app->console, "\nФС : RAMFS + ");
        gconsole_write(&app->console, ramfs_persist_state_name());

    } else if (str_eq(argv[0], "echo")) {
        gconsole_putc(&app->console, '\n');
        for (int i = 1; i < argc; i++) {
            if (i > 1) gconsole_putc(&app->console, ' ');
            gconsole_write(&app->console, argv[i]);
        }

    } else if (str_eq(argv[0], "df")) {
        uint32_t free_b = 0, total_b = 0;
        ramfs_space(&free_b, &total_b);
        size_t used_entries = 1; /* корень "/" */
        uint32_t ram_bytes = 0;
        ramfs_walk_stats("/", &used_entries, &ram_bytes);

        if (total_b > 0) {
            uint32_t used_b = total_b - free_b;
            gconsole_write(&app->console, "\nДИСК  : ВСЕГО ");
            write_size_human(&app->console, total_b);
            gconsole_write(&app->console, "  ЗАНЯТО ");
            write_size_human(&app->console, used_b);
            gconsole_write(&app->console, "  СВОБОДНО ");
            write_size_human(&app->console, free_b);
        } else {
            gconsole_write(&app->console, "\nДИСК  : НЕТ (ТОЛЬКО RAMFS)");
        }
        gconsole_write(&app->console, "\nRAMFS : ");
        write_size_human(&app->console, ram_bytes);
        gconsole_write(&app->console, " ЗАНЯТО, ");
        write_uint(&app->console, (uint32_t)used_entries);
        gconsole_write(&app->console, "/");
        write_uint(&app->console, RAMFS_MAX_ENTRIES);
        gconsole_write(&app->console, " ЗАПИСЕЙ");
        gconsole_write(&app->console, "\nСТАТУС: ");
        gconsole_write(&app->console, ramfs_persist_state_name());

    } else if (str_eq(argv[0], "fsinfo")) {
        gconsole_write(&app->console, "\nRAMFS + ");
        gconsole_write(&app->console, ramfs_persist_state_name());
        if (ramfs_persist_errors()) {
            gconsole_write(&app->console, "\nошибки: ");
            write_padded(&app->console, ramfs_persist_errors(), 1);
        }

    } else if (str_eq(argv[0], "help")) {
        gconsole_write(&app->console, "\nls cd pwd mkdir cat touch edit echo");
        gconsole_write(&app->console, "\nclear date uptime neofetch df fsinfo help");
        if (ext_help) {
            gconsole_putc(&app->console, '\n');
            gconsole_write(&app->console, ext_help);
        }

    } else if (ext_cmd && ext_cmd(app, argc, argv)) {
        /* выполнена внешней командой (сеть) */

    } else {
        print_error(app, "неизвестная команда");
    }
}

static void run_command(terminal_app_t *app, char *line) {
    char *redir_file = 0;
    int append = 0;
    int has_redir = split_redirect(line, &redir_file, &append);
    if (has_redir < 0) {
        print_error(app, "ошибка синтаксиса: ожидается файл после >");
        return;
    }
    if (has_redir == 0) {
        exec_command(app, line);
        return;
    }

    char redir_path[RAMFS_MAX_PATH_LEN];
    resolve_path(app, redir_file, redir_path, sizeof(redir_path));

    redir_cap_t cap = { g_redir_buf, 0, sizeof(g_redir_buf) };
    gconsole_set_capture(&app->console, redir_capture_putc, &cap);
    exec_command(app, line);
    gconsole_set_capture(&app->console, 0, 0);

    size_t start = 0;
    if (cap.len > 0 && cap.buf[0] == '\n') start = 1;
    size_t out_len = cap.len - start;
    if (out_len > 0 && cap.buf[start + out_len - 1] != '\n' && cap.len < cap.cap) {
        cap.buf[start + out_len] = '\n';
        out_len++;
    }

    if (ramfs_is_dir(redir_path)) {
        print_error(app, "это каталог");
        return;
    }

    int wr = -1;
    if (append) {
        size_t cur_len = 0;
        const ramfs_file_t *ef = ramfs_lookup(redir_path);
        if (ef) {
            long r = ramfs_read(redir_path, 0, g_redir_merge, sizeof(g_redir_merge));
            if (r > 0) cur_len = (size_t)r;
        } else {
            ramfs_touch(redir_path);
        }
        size_t copy_len = out_len;
        if (cur_len + copy_len > sizeof(g_redir_merge))
            copy_len = sizeof(g_redir_merge) - cur_len;
        for (size_t i = 0; i < copy_len; i++)
            g_redir_merge[cur_len + i] = cap.buf[start + i];
        wr = ramfs_write(redir_path, g_redir_merge, cur_len + copy_len);
    } else {
        ramfs_touch(redir_path);
        wr = ramfs_write(redir_path, cap.buf + start, out_len);
    }

    if (wr != 0) print_error(app, "ошибка записи");
}

void terminal_app_init(terminal_app_t *app, const gui_window_t *win) {
    terminal_app_init_colors(app, win, GFX_BLACK, GFX_WHITE);
}

void terminal_app_init_colors(terminal_app_t *app, const gui_window_t *win,
                               uint8_t fg, uint8_t bg) {
    gconsole_init(&app->console, win, fg, bg);
    app->line_len = 0;
    app->in_edit_mode = 0;
    app->edit_total = 0;
    app->busy = 0;
    app->cancel = 0;
    app->raw = 0;
    app->rq_head = app->rq_tail = 0;

    ramfs_mkdir(TERM_HOME_DIR);
    str_copy(app->cwd, TERM_HOME_DIR, sizeof(app->cwd));

    gconsole_write(&app->console, "Терминал proshivkaOS");
    print_prompt(app);
}

/* ---------------- Автодополнение по Tab ---------------- */

static const char *const TERM_CMDS[] = {
    "ls", "cd", "pwd", "mkdir", "cat", "touch", "edit", "clear",
    "date", "uptime", "neofetch", "df", "fsinfo", "echo", "help",
    "ifconfig", "arping", "ping", "dhcp", "udpsend", "nslookup", "http", "ssh",
};
#define TERM_LOCAL_CMD_COUNT 15
#define TERM_ALL_CMD_COUNT   (sizeof(TERM_CMDS) / sizeof(TERM_CMDS[0]))

static const char *const TERM_FILE_CMDS[] = { "ls", "cd", "cat", "edit", "touch", "mkdir" };

static int starts_with(const char *s, const char *prefix, size_t plen) {
    for (size_t i = 0; i < plen; i++)
        if (s[i] != prefix[i] || !s[i]) return 0;
    return 1;
}

static int word_eq(const char *w, size_t wlen, const char *b) {
    return str_len(b) == wlen && starts_with(b, w, wlen);
}

static int is_file_cmd(const char *w, size_t wlen) {
    for (size_t i = 0; i < sizeof(TERM_FILE_CMDS) / sizeof(TERM_FILE_CMDS[0]); i++)
        if (word_eq(w, wlen, TERM_FILE_CMDS[i])) return 1;
    return 0;
}

static void append_byte_to_line(terminal_app_t *app, char ch) {
    if (app->line_len < TERM_LINE_BUF - 1) {
        app->line[app->line_len++] = ch;
        gconsole_putc(&app->console, ch);
    }
}

static void autocomplete(terminal_app_t *app) {
    size_t len = app->line_len;
    size_t ws = len;
    while (ws > 0 && app->line[ws - 1] != ' ') ws--;
    size_t plen = len - ws;

    int first = 1;
    size_t a = 0;
    while (a < ws && app->line[a] == ' ') a++;
    if (a < ws) first = 0;

    if (!first) {
        size_t b = a;
        while (b < len && app->line[b] != ' ') b++;
        if (!is_file_cmd(app->line + a, b - a)) return;
    }

    const char *m[64];
    size_t n = 0;
    if (first) {
        size_t ncmds = ext_cmd ? TERM_ALL_CMD_COUNT : TERM_LOCAL_CMD_COUNT;
        for (size_t i = 0; i < ncmds && n < 64; i++)
            if (starts_with(TERM_CMDS[i], app->line + ws, plen)) m[n++] = TERM_CMDS[i];
    } else {
        const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
        size_t fn = ramfs_list_dir(app->cwd, files, RAMFS_MAX_ENTRIES);
        for (size_t i = 0; i < fn && n < 64; i++)
            if (starts_with(files[i]->name, app->line + ws, plen)) m[n++] = files[i]->name;
    }
    if (!n) return;

    size_t common = str_len(m[0]);
    for (size_t k = 1; k < n; k++) {
        size_t l = 0;
        while (m[0][l] && m[k][l] == m[0][l]) l++;
        if (l < common) common = l;
    }

    if (common > plen) {
        for (size_t i = plen; i < common; i++) append_byte_to_line(app, m[0][i]);
        if (n == 1) append_byte_to_line(app, ' ');
        return;
    }
    if (n == 1) {
        if (app->line_len == 0 || app->line[app->line_len - 1] != ' ')
            append_byte_to_line(app, ' ');
        return;
    }

    gconsole_putc(&app->console, '\n');
    for (size_t k = 0; k < n; k++) {
        if (k) gconsole_write(&app->console, "  ");
        gconsole_write(&app->console, m[k]);
    }
    app->line[app->line_len] = '\0';
    print_prompt(app);
    gconsole_write(&app->console, app->line);
}

void terminal_app_handle_char(terminal_app_t *app, char c) {
    terminal_app_handle_key(app, (unsigned char)c);
}

void terminal_app_handle_key(terminal_app_t *app, int key) {
    /* Коды HAL_KEY_* лежат в 0x100..0x10F и символами не являются. */
    if (key < 0 || (key >= 0x100 && key < 0x110)) return;
    uint32_t cp = (uint32_t)key;

    /* Идёт команда: ввод не принимаем, только Ctrl+C (код 3) её прерывает. */
    if (app->busy) {
        if (app->raw) {
            /* Команде (ssh) нужны байты, а не символы. */
            char enc[4];
            int n = utf8_encode(cp, enc);
            for (int k = 0; k < n; k++)
                if (app->rq_tail - app->rq_head < sizeof app->rq)
                    app->rq[app->rq_tail++ % sizeof app->rq] = (uint8_t)enc[k];
        } else if (cp == 3) {
            app->cancel = 1;
        }
        return;
    }

    if (cp == '\t') {
        if (!app->in_edit_mode) autocomplete(app);
        return;
    }

    if (cp == '\r') cp = '\n';

    if (cp == '\n') {
        app->line[app->line_len] = '\0';

        if (app->in_edit_mode) {
            if (str_eq(app->line, ".")) {
                ramfs_write(app->edit_filename, app->edit_buf, app->edit_total);
                app->in_edit_mode = 0;
                gconsole_write(&app->console, "\nсохранено");
            } else {
                size_t len = str_len(app->line);
                if (app->edit_total + len + 1 < RAMFS_MAX_FILE_SIZE) {
                    for (size_t i = 0; i < len; i++)
                        app->edit_buf[app->edit_total++] = app->line[i];
                    app->edit_buf[app->edit_total++] = '\n';
                }
                gconsole_putc(&app->console, '\n');
            }
        } else {
            run_command(app, app->line);
        }

        app->line_len = 0;
        if (!app->in_edit_mode) print_prompt(app);
        else gconsole_write(&app->console, "\n. ");

    } else if (cp == '\b' || cp == 127) {
        /* Стираем символ целиком: в UTF-8 буква — несколько байт, а на
           экране это одна клетка. */
        if (app->line_len > 0) {
            app->line_len = utf8_prev(app->line, app->line_len, app->line_len);
            gconsole_putcp(&app->console, '\b');
        }

    } else if (cp >= 32) {
        char enc[4];
        int n = utf8_encode(cp, enc);
        /* Влезает целиком или не берём вовсе — половина буквы в строке
           дала бы битый UTF-8. */
        if (n > 0 && app->line_len + n < TERM_LINE_BUF) {
            for (int k = 0; k < n; k++) app->line[app->line_len++] = enc[k];
            gconsole_putcp(&app->console, cp);
        }
    }
}
