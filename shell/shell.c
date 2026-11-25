/* shell/shell.c — встроенная командная строка поверх RamFS.
 * Использует только hal.h и ramfs.h — работает одинаково на x86 и ARM64.
 *
 * ВАЖНО про cmd_edit(): полноэкранный редактор в apps/editor.c зовёт
 * hal_console_clear()/hal_console_goto(), которые пока реализованы только
 * для x86 (см. комментарий у их объявления в hal.h). Поэтому сборка
 * ARCH=arm64 text/run сейчас не слинкуется, пока в hal_console_arm64.c не
 * появятся такие же clear/goto — см. hal.h.
 */
#include "hal.h"
#include "ramfs.h"
#include "editor.h"
#include "ping.h"
#include "netcore.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "lineedit.h"
#include "shell_netcmd.h"

#define CMD_BUF_SIZE   128
#define CMD_MAX_ARGS   16

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void put_ip(const uint8_t *ip);
static void put_mac(const uint8_t *m);
static void put_dec(unsigned v);
static int  parse_ip(const char *s, uint8_t out[4]);


/* Разбивает строку на аргументы по пробелу (in-place, без malloc). */
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

/* ---------------- Текущая папка ---------------- */

/* Текущая папка оболочки. Все команды с путями разбирают аргумент через
 * path_resolve(), так что относительные имена работают от неё. Домашняя
 * папка ("~") — корень, как и в оконном терминале. */
static char g_cwd[RAMFS_MAX_PATH_LEN] = "/";

/* Превращает аргумент-путь в канонический абсолютный: относительное имя
 * приклеивается к g_cwd, "." и пустые сегменты выбрасываются, ".." снимает
 * последний сегмент (выше корня не уйти). Слишком длинный путь молча
 * обрезается — RamFS всё равно отвергнет такой. max должен быть >= 2. */
static void path_resolve(const char *arg, char *out, size_t max) {
    char full[2 * RAMFS_MAX_PATH_LEN];
    size_t n = 0;

    const char *a = arg;
    if (a[0] == '~' && (a[1] == '\0' || a[1] == '/')) {
        a++;
        full[n++] = '/';
    } else if (a[0] != '/') {
        for (size_t i = 0; g_cwd[i] && n < sizeof(full) - 2; i++) full[n++] = g_cwd[i];
        full[n++] = '/';
    }
    for (size_t i = 0; a[i] && n < sizeof(full) - 1; i++) full[n++] = a[i];
    full[n] = '\0';

    size_t len = 1;
    out[0] = '/';
    out[1] = '\0';

    size_t i = 0;
    while (full[i]) {
        while (full[i] == '/') i++;
        size_t start = i;
        while (full[i] && full[i] != '/') i++;
        size_t sl = i - start;
        if (sl == 0) break;

        if (sl == 1 && full[start] == '.') continue;
        if (sl == 2 && full[start] == '.' && full[start + 1] == '.') {
            while (len > 1 && out[len - 1] != '/') len--;   /* снять сегмент */
            if (len > 1) len--;                             /* и слэш перед ним */
            out[len] = '\0';
            continue;
        }

        size_t need = len + (len > 1 ? 1 : 0) + sl;
        if (need >= max) break;
        if (len > 1) out[len++] = '/';
        for (size_t k = 0; k < sl; k++) out[len++] = full[start + k];
        out[len] = '\0';
    }
}

/* ---------------- Команды ---------------- */

static void cmd_ls(int argc, char *argv[]) {
    char path[RAMFS_MAX_PATH_LEN];
    path_resolve(argc > 1 ? argv[1] : ".", path, sizeof(path));

    if (!ramfs_is_dir(path)) {
        const ramfs_file_t *f = ramfs_lookup(path);
        if (!f) {
            hal_console_write("ls: нет такого объекта: ");
            hal_console_write(argc > 1 ? argv[1] : path);
            hal_console_putc('\n');
            return;
        }
        hal_console_write(f->name);       /* ls <file> — просто его имя */
        hal_console_putc('\n');
        return;
    }

    const ramfs_file_t *files[RAMFS_MAX_FILES];
    size_t n = ramfs_list_dir(path, files, RAMFS_MAX_FILES);

    if (n == 0) {
        hal_console_write("(пусто)\n");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        if (files[i]->is_dir) {
            hal_console_set_color(HAL_COLOR_LIGHT_CYAN, HAL_COLOR_BLACK);
            hal_console_write(files[i]->name);
            hal_console_putc('/');
        } else {
            hal_console_set_color(HAL_COLOR_LIGHT_GREEN, HAL_COLOR_BLACK);
            hal_console_write(files[i]->name);
        }
        hal_console_reset_color();
        hal_console_putc('\n');
    }
}

static void cmd_cd(int argc, char *argv[]) {
    const char *arg = (argc > 1) ? argv[1] : "~";    /* голый cd — домой */
    char path[RAMFS_MAX_PATH_LEN];
    path_resolve(arg, path, sizeof(path));

    if (ramfs_is_dir(path)) {
        size_t i = 0;
        while (path[i] && i < sizeof(g_cwd) - 1) { g_cwd[i] = path[i]; i++; }
        g_cwd[i] = '\0';
        return;
    }

    hal_console_write("cd: ");
    hal_console_write(arg);
    hal_console_write(ramfs_lookup(path) ? ": это не директория\n" : ": нет такой директории\n");
}

static void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: cat <file>\n");
        return;
    }

    char path[RAMFS_MAX_PATH_LEN];
    path_resolve(argv[1], path, sizeof(path));

    const ramfs_file_t *f = ramfs_lookup(path);
    if (!f) {
        hal_console_write("cat: файл не найден: ");
        hal_console_write(argv[1]);
        hal_console_putc('\n');
        return;
    }

    uint8_t chunk[256], last = '\n';
    size_t off = 0;
    for (;;) {
        long n = ramfs_read(path, off, chunk, sizeof(chunk));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            last = c;
            /* управляющие байты (бинарные файлы) — точкой */
            hal_console_putc((c < 32 && c != '\n' && c != '\t') || c == 127 ? '.' : (char)c);
        }
        off += (size_t)n;
    }
    if (f->size == 0 || last != '\n')
        hal_console_putc('\n');
}

static void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: touch <файл>\n");
        return;
    }

    char path[RAMFS_MAX_PATH_LEN];
    path_resolve(argv[1], path, sizeof(path));

    if (ramfs_touch(path) != 0)
        hal_console_write("touch: не удалось создать файл\n");
}

/* neofetch: логотип слева, сведения справа. Консоль у нас построчная, без
 * позиционирования курсора на всех платформах, поэтому экран собирается
 * построчно: строка логотипа (дополненная пробелами до одной ширины) и
 * следом строка сведений. */
#define LOGO_W 20

static const char *const NF_LOGO[] = {
    "         /\\         ",
    "        /  \\        ",
    "       / /\\ \\       ",
    "      / /  \\ \\      ",
    "     / /____\\ \\     ",
    "    /_/      \\_\\    ",
};
#define NF_LOGO_LINES ((int)(sizeof NF_LOGO / sizeof NF_LOGO[0]))

/* Одна строка сведений: подпись жёлтым-голубым, значение обычным цветом. */
static void nf_info(int row) {
    switch (row) {
    case 0:
        hal_console_set_color(HAL_COLOR_YELLOW, HAL_COLOR_BLACK);
        hal_console_write("proshivkaOS NEXT ");
        hal_console_write(PROSHIVKAOS_VERSION);
        hal_console_reset_color();
        return;
    case 1:
        hal_console_write("-----------------------");
        return;
    }

    const char *label = 0, *value = 0;
    switch (row) {
    case 2: label = "kernel"; value = "microkernel, этап 2";    break;
    case 3: label = "hal   "; value = "console/mem/thread(stub)"; break;
    case 4: label = "fs    "; value = 0;                        break;
    case 5: label = "arch  "; value = hal_arch_name();          break;
    case 6: label = "cpu   "; value = hal_cpu_name();           break;
    default: return;
    }

    hal_console_set_color(HAL_COLOR_LIGHT_CYAN, HAL_COLOR_BLACK);
    hal_console_write(label);
    hal_console_reset_color();
    hal_console_write(" : ");
    if (row == 4) {
        hal_console_write("RamFS + ");
        hal_console_write(ramfs_persist_state_name());
    } else {
        hal_console_write(value);
    }
}

static void cmd_neofetch(void) {
    const int rows = 7;     /* строк сведений; логотип короче — добивается пробелами */

    for (int r = 0; r < rows || r < NF_LOGO_LINES; r++) {
        if (r < NF_LOGO_LINES) {
            hal_console_set_color(HAL_COLOR_LIGHT_CYAN, HAL_COLOR_BLACK);
            hal_console_write(NF_LOGO[r]);
            hal_console_reset_color();
        } else {
            for (int k = 0; k < LOGO_W; k++) hal_console_putc(' ');
        }
        if (r < rows) nf_info(r);
        hal_console_putc('\n');
    }
}

static void print_uint(unsigned v) {
    char buf[12]; int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char out[12]; int k = 0;
    while (n) out[k++] = buf[--n];
    out[k] = 0;
    hal_console_write(out);
}

/* Состояние постоянного хранилища: подключился ли диск и сколько операций
 * с ним не удалось. Первое, что стоит посмотреть, если файлы пропадают. */
static void print_size_human(uint32_t bytes) {
    if (bytes >= 1024u * 1024u) {
        print_uint((unsigned)(bytes / (1024u * 1024u)));
        hal_console_write("M");
    } else if (bytes >= 1024u) {
        print_uint((unsigned)(bytes / 1024u));
        hal_console_write("K");
    } else {
        print_uint((unsigned)bytes);
        hal_console_write("B");
    }
}

static void ramfs_walk_stats(const char *dir, size_t *entries, uint32_t *bytes) {
    const ramfs_file_t *list[RAMFS_MAX_FILES];
    size_t n = ramfs_list_dir(dir, list, RAMFS_MAX_FILES);
    for (size_t i = 0; i < n; i++) {
        (*entries)++;
        if (list[i]->is_dir) {
            ramfs_walk_stats(list[i]->path, entries, bytes);
        } else {
            *bytes += (uint32_t)list[i]->size;
        }
    }
}

static void cmd_df(void) {
    uint32_t free_b = 0, total_b = 0;
    ramfs_space(&free_b, &total_b);
    size_t used_entries = 1;
    uint32_t ram_bytes = 0;
    ramfs_walk_stats("/", &used_entries, &ram_bytes);

    if (total_b > 0) {
        uint32_t used_b = total_b - free_b;
        hal_console_write("disk   : total "); print_size_human(total_b);
        hal_console_write("  used "); print_size_human(used_b);
        hal_console_write("  free "); print_size_human(free_b);
        hal_console_putc('\n');
    } else {
        hal_console_write("disk   : отсутствует\n");
    }
    hal_console_write("ramfs  : "); print_size_human(ram_bytes);
    hal_console_write(" занято, "); print_uint((unsigned)used_entries);
    hal_console_write("/"); print_uint(RAMFS_MAX_ENTRIES);
    hal_console_write(" записей\n");
    hal_console_write("состояние : "); hal_console_write(ramfs_persist_state_name());
    hal_console_putc('\n');
}

static void cmd_fsinfo(void) {
    hal_console_write("fs     : RamFS + ");
    hal_console_write(ramfs_persist_state_name());
    hal_console_write("\n");
    if (ramfs_persist_errors()) {
        hal_console_write("ошибок диска: ");
        print_uint(ramfs_persist_errors());
        hal_console_write("\n");
    }
}

static void cmd_clear(void) {
    hal_console_clear();
}

static void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            hal_console_putc(' ');
        hal_console_write(argv[i]);
    }
    hal_console_putc('\n');
}

static void put_hex8(uint8_t v) {
    const char *h = "0123456789abcdef";
    hal_console_putc(h[v >> 4]);
    hal_console_putc(h[v & 15]);
}

static void put_mac(const uint8_t *m) {
    for (int i = 0; i < 6; i++) {
        if (i) hal_console_putc(':');
        put_hex8(m[i]);
    }
}

static void cmd_ifconfig(void) {
    hal_console_write("mac : "); put_mac(net_cfg.mac); hal_console_putc('\n');
    hal_console_write("ip  : "); put_ip(net_cfg.ip);
    hal_console_write(net_cfg.from_dhcp ? "  (dhcp)\n" : "  (default)\n");
    hal_console_write("mask: "); put_ip(net_cfg.mask); hal_console_putc('\n');
    hal_console_write("gw  : "); put_ip(net_cfg.gw);   hal_console_putc('\n');
    hal_console_write("dns : "); put_ip(net_cfg.dns);  hal_console_putc('\n');
}

static void cmd_dhcp(void) {
    hal_console_write("dhcp: запрос адреса...\n");
    if (dhcp_acquire() != 0) {
        hal_console_write("dhcp: нет ответа\n");
        return;
    }
    hal_console_write("dhcp: адрес получен ");
    put_ip(net_cfg.ip);
    hal_console_putc('\n');
}

/* "9999" -> 9999. 0 = успех. */
static int parse_port(const char *s, uint16_t *out) {
    unsigned v = 0;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned)(*s - '0');
        if (v > 65535) return -1;
        s++;
    }
    if (*s || v == 0) return -1;
    *out = (uint16_t)v;
    return 0;
}

/* udpsend <ip> <port> <text...> — одна датаграмма, ответ не ждём.
 * Проверка с хоста при запуске в QEMU (10.0.2.2 — это сам хост):
 *     nc -u -l 9999        и в системе:   udpsend 10.0.2.2 9999 privet */
static void cmd_udpsend(int argc, char *argv[]) {
    uint8_t ip[4];
    uint16_t port;
    if (argc < 4 || parse_ip(argv[1], ip) != 0 || parse_port(argv[2], &port) != 0) {
        hal_console_write("usage: udpsend <a.b.c.d> <порт> <текст>\n");
        return;
    }

    char text[128];
    size_t len = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3 && len < sizeof text - 1) text[len++] = ' ';
        for (const char *p = argv[i]; *p && len < sizeof text - 1; p++)
            text[len++] = *p;
    }
    text[len++] = '\n';

    int r = udp_send(ip, 40000, port, text, len);
    if (r == NET_OK)           hal_console_write("отправлено\n");
    else if (r == NET_ERR_ARP) hal_console_write("udpsend: нет MAC-адреса для next-hop (arp)\n");
    else                       hal_console_write("udpsend: ошибка отправки\n");
}

static void print_dns_error(int r) {
    switch (r) {
    case DNS_ERR_NAME:     hal_console_write("dns: неверное имя\n"); break;
    case DNS_ERR_TIMEOUT:  hal_console_write("dns: нет ответа (проверьте адрес)\n"); break;
    case DNS_ERR_NXDOMAIN: hal_console_write("dns: имя не найдено\n"); break;
    case DNS_ERR_NOADDR:   hal_console_write("dns: нет A-записи\n"); break;
    case DNS_ERR_NET:      hal_console_write("dns: ошибка отправки\n"); break;
    default:               hal_console_write("dns: сервер вернул ошибку\n"); break;
    }
}

/* nslookup <имя> */
static void cmd_nslookup(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: nslookup <name>\n");
        return;
    }
    uint8_t ip[4];
    int r = dns_resolve(argv[1], ip);
    if (r != DNS_OK) { print_dns_error(r); return; }

    hal_console_write(argv[1]);
    hal_console_write(" -> ");
    put_ip(ip);
    hal_console_putc('\n');
}

/* Дописывает строку в буфер запроса без выхода за границы. */
static void req_add(char *buf, size_t *len, size_t cap, const char *s) {
    while (*s && *len < cap - 1) buf[(*len)++] = *s++;
    buf[*len] = '\0';
}

/* http <хост> [путь] — HTTP/1.0 GET по порту 80, вывод ответа на экран.
 * HTTPS не поддерживается: пробуй сайты, которые отвечают по http
 * (example.com, neverssl.com). */
static void cmd_http(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: http <хост> [путь]\n");
        return;
    }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    uint8_t ip[4];
    if (parse_ip(host, ip) != 0) {
        int r = dns_resolve(host, ip);
        if (r != DNS_OK) { print_dns_error(r); return; }
    }

    hal_console_write("connect "); put_ip(ip); hal_console_write(":80\n");
    int r = tcp_connect(ip, 80);
    if (r != TCP_OK) {
        hal_console_write(r == TCP_ERR_RESET   ? "http: порт закрыт (RST)\n"
                        : r == TCP_ERR_TIMEOUT ? "http: timeout\n"
                                               : "http: ошибка соединения\n");
        return;
    }

    char req[256];
    size_t rl = 0;
    req[0] = '\0';
    req_add(req, &rl, sizeof req, "GET ");
    req_add(req, &rl, sizeof req, path);
    req_add(req, &rl, sizeof req, " HTTP/1.0\r\nHost: ");
    req_add(req, &rl, sizeof req, host);
    req_add(req, &rl, sizeof req, "\r\nUser-Agent: proshivkaos\r\nConnection: close\r\n\r\n");

    if (tcp_send(req, rl) < 0) {
        hal_console_write("http: ошибка отправки\n");
        tcp_close();
        return;
    }

    enum { SHOW_MAX = 2000 };
    uint8_t buf[512];
    unsigned total = 0, shown = 0;
    for (;;) {
        int n = tcp_recv(buf, sizeof buf, NET_POLL_LIMIT);
        if (n < 0) { hal_console_write("\nhttp: timeout\n"); break; }
        if (n == 0) break;                     /* сервер закрыл соединение */

        for (int i = 0; i < n; i++) {
            if (shown >= SHOW_MAX) break;
            char c = (char)buf[i];
            if (c == '\r') continue;
            hal_console_putc((c == '\n' || (c >= 32 && c < 127)) ? c : '.');
            shown++;
        }
        total += (unsigned)n;
    }
    tcp_close();

    hal_console_write("\n--- получено ");
    put_dec(total);
    hal_console_write(" bayt");
    if (total > shown) hal_console_write(" (показаны первые 2000)");
    hal_console_putc('\n');
}

/* ARP-запрос шлюзу QEMU (10.0.2.2) с нашего адреса 10.0.2.15.
 * Если пришёл ответ — передача и приём работают. */
static void cmd_arping(void) {
    static const uint8_t ip_me[4] = {10, 0, 2, 15};
    static const uint8_t ip_gw[4] = {10, 0, 2, 2};
    uint8_t mac[6], f[60] = {0};
    hal_net_get_mac(mac);

    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = mac[i]; }
    f[12] = 0x08; f[13] = 0x06;          /* ethertype ARP */
    f[14] = 0x00; f[15] = 0x01;          /* Ethernet */
    f[16] = 0x08; f[17] = 0x00;          /* IPv4 */
    f[18] = 6;    f[19] = 4;
    f[20] = 0x00; f[21] = 0x01;          /* request */
    for (int i = 0; i < 6; i++) f[22 + i] = mac[i];
    for (int i = 0; i < 4; i++) { f[28 + i] = ip_me[i]; f[38 + i] = ip_gw[i]; }

    if (hal_net_send(f, 42) != 0) {
        hal_console_write("arping: ошибка отправки\n");
        return;
    }

    uint8_t r[1536];
    for (long t = 0; t < 20000000; t++) {
        size_t n = hal_net_recv(r, sizeof r);
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 && r[21] == 0x02) {
            hal_console_write("получено от 10.0.2.2, mac ");
            put_mac(r + 22);
            hal_console_putc('\n');
            return;
        }
    }
    hal_console_write("arping: не отвечает\n");
}

static int parse_ip(const char *s, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            if (v > 255) return -1;
            s++;
        }
        out[i] = (uint8_t)v;
        if (i < 3) { if (*s != '.') return -1; s++; }
    }
    return *s == '\0' ? 0 : -1;
}

static void put_dec(unsigned v) {
    char b[11]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) hal_console_putc(b[--n]);
}

static void put_ip(const uint8_t *ip) {
    for (int i = 0; i < 4; i++) { if (i) hal_console_putc('.'); put_dec(ip[i]); }
}

static void cmd_ping(int argc, char *argv[]) {
    uint8_t ip[4];
    if (argc < 2 || parse_ip(argv[1], ip) != 0) {
        hal_console_write("usage: ping <ip>\n");
        return;
    }

    int ok = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        int r = net_ping(ip, seq);
        if (r == PING_OK) {
            ok++;
            hal_console_write("получено от "); put_ip(ip);
            hal_console_write(": seq="); put_dec(seq); hal_console_putc('\n');
        } else if (r == PING_NO_ARP) {
            hal_console_write("ping: нет MAC-адреса next-hop (arp)\n");
            return;
        } else if (r == PING_TX_ERR) {
            hal_console_write("ping: ошибка отправки\n");
            return;
        } else {
            hal_console_write("seq="); put_dec(seq);
            hal_console_write(": не отвечает\n");
        }
    }
    hal_console_write("--- "); put_ip(ip); hal_console_write(": ");
    put_dec((unsigned)ok); hal_console_write("/4 получено\n");
}

/* Полноэкранный редактор (apps/editor.c) — binды в стиле micro:
 * Ctrl+S sohranit, Ctrl+Q vyhod, Ctrl+F poisk. Файл создаётся при
 * отсутствии автоматически внутри editor_run() при первом сохранении. */
static void cmd_edit(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("использование: edit <файл>\n");
        return;
    }

    char path[RAMFS_MAX_PATH_LEN];
    path_resolve(argv[1], path, sizeof(path));

    ramfs_touch(path);
    editor_run(path);
}

static void cmd_help(void) {
    hal_console_write(
        "Доступные команды:\n"
        "  ls [директория]         - список файлов\n"
        "  cd [директория]         - сменить папку (cd .., cd ~, cd /)\n"
        "  cat <файл>         - показать содержимое файла\n"
        "  touch <файл>       - создать пустой файл\n"
        "  clear              - очистить вывод консоли\n"
        "  echo               - вывести текст\n"
        "  edit <файл>        - редактор кода\n"
        "                       (Ctrl+S сохранить, Ctrl+Q выход, Ctrl+F поиск)\n"
        "  neofetch           - информация об устройстве\n"
        "  df                 - свободное пространство на диске\n"
        "  fsinfo             - состояние диска\n"
        "  help               - эта справка\n"
        "  ifconfig           - MAC-адрес сетевой карты\n"
        "  arping             - проверка сети (ARP к 10.0.2.2)\n"
        "  ping <ip>          - ICMP echo\n"
        "  dhcp               - получить адрес по DHCP\n"
        "  udpsend <ip> <p> <текст> - отправить UDP\n"
        "  nslookup <имя>    - узнать IP-адрес по имени (DNS)\n"
        "  http <хост> [путь] - HTTP GET по порту 80\n"
        "  ssh [-p порт] user@host [флаг] - SSH\n"
    );
}

static void print_prompt(void) {
    hal_console_set_color(HAL_COLOR_LIGHT_MAGENTA, HAL_COLOR_BLACK);
    if (g_cwd[0] == '/' && g_cwd[1] == '\0') {
        hal_console_write("proshivkaos> ");
    } else {
        hal_console_write("proshivkaos ");
        hal_console_write(g_cwd);
        hal_console_write("> ");
    }
    hal_console_reset_color();
}

/* Имена команд для Tab. ДЕРЖАТЬ В СИНХРОНЕ с цепочкой else-if в shell_run():
 * добавил команду — добавь её имя сюда. */
static const char *const COMMANDS[] = {
    "ls", "cd", "cat", "touch", "edit", "neofetch", "df", "fsinfo", "help",
    "clear", "echo",
    "ifconfig", "arping", "ping", "dhcp", "udpsend", "nslookup", "http", "ssh",
};

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} shell_redir_t;

static void shell_capture_putc(char c, void *ctx) {
    shell_redir_t *rc = (shell_redir_t *)ctx;
    if (rc->len < rc->cap) rc->buf[rc->len++] = c;
}

static char g_shell_redir_buf[RAMFS_MAX_FILE_SIZE];
static char g_shell_redir_merge[RAMFS_MAX_FILE_SIZE];

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

static void exec_shell_cmd(int argc, char *argv[]) {
        if (str_eq(argv[0], "ls"))            cmd_ls(argc, argv);
        else if (str_eq(argv[0], "cd"))       cmd_cd(argc, argv);
        else if (str_eq(argv[0], "cat"))      cmd_cat(argc, argv);
        else if (str_eq(argv[0], "touch"))    cmd_touch(argc, argv);
        else if (str_eq(argv[0], "edit"))     cmd_edit(argc, argv);
        else if (str_eq(argv[0], "neofetch")) cmd_neofetch();
        else if (str_eq(argv[0], "df"))       cmd_df();
        else if (str_eq(argv[0], "fsinfo"))   cmd_fsinfo();
        else if (str_eq(argv[0], "ifconfig")) cmd_ifconfig();
        else if (str_eq(argv[0], "arping"))   cmd_arping();
        else if (str_eq(argv[0], "help"))     cmd_help();
        else if (str_eq(argv[0], "clear"))    cmd_clear();
        else if (str_eq(argv[0], "echo"))    cmd_echo(argc, argv);
        else if (str_eq(argv[0], "ping")) cmd_ping(argc, argv);
        else if (str_eq(argv[0], "dhcp"))    cmd_dhcp();
        else if (str_eq(argv[0], "udpsend")) cmd_udpsend(argc, argv);
        else if (str_eq(argv[0], "nslookup")) cmd_nslookup(argc, argv);
        else if (str_eq(argv[0], "http"))     cmd_http(argc, argv);
        else if (shell_try_netcmd(argc, argv)) {}
        else {
            hal_console_write("неизвестная команда: ");
            hal_console_write(argv[0]);
            hal_console_putc('\n');
        }
}

/* ---------------- Главный цикл ---------------- */

void shell_run(void) {
    char line[CMD_BUF_SIZE];
    char *argv[CMD_MAX_ARGS];

    cmd_help();

    if (hal_net_init() != 0)
            hal_console_write("net: карта не найдена\n");
    if (hal_net_init() == 0) net_cfg_init();
    else hal_console_write("net: карта не найдена\n");

    for (;;) {
        print_prompt();
                line_read(line, CMD_BUF_SIZE, print_prompt,
                         COMMANDS, sizeof COMMANDS / sizeof COMMANDS[0]);

        char *redir_file = 0;
        int append = 0;
        int has_redir = split_redirect(line, &redir_file, &append);
        if (has_redir < 0) {
            hal_console_write("ошибка синтаксиса: укажите файл после >\n");
            continue;
        }

        int argc = tokenize(line, argv, CMD_MAX_ARGS);
        if (argc == 0)
            continue;

        if (has_redir == 0) {
            exec_shell_cmd(argc, argv);
            continue;
        }

        char redir_path[RAMFS_MAX_PATH_LEN];
        path_resolve(redir_file, redir_path, sizeof(redir_path));

        shell_redir_t cap = { g_shell_redir_buf, 0, sizeof(g_shell_redir_buf) };
        hal_console_set_capture(shell_capture_putc, &cap);
        exec_shell_cmd(argc, argv);
        hal_console_set_capture(0, 0);

        if (ramfs_is_dir(redir_path)) {
            hal_console_write("ошибка: это директория\n");
            continue;
        }

        int wr = -1;
        if (append) {
            size_t cur_len = 0;
            if (ramfs_lookup(redir_path)) {
                long r = ramfs_read(redir_path, 0, g_shell_redir_merge, sizeof(g_shell_redir_merge));
                if (r > 0) cur_len = (size_t)r;
            } else {
                ramfs_touch(redir_path);
            }
            size_t copy_len = cap.len;
            if (cur_len + copy_len > sizeof(g_shell_redir_merge))
                copy_len = sizeof(g_shell_redir_merge) - cur_len;
            for (size_t i = 0; i < copy_len; i++)
                g_shell_redir_merge[cur_len + i] = cap.buf[i];
            wr = ramfs_write(redir_path, g_shell_redir_merge, cur_len + copy_len);
        } else {
            ramfs_touch(redir_path);
            wr = ramfs_write(redir_path, cap.buf, cap.len);
        }
        if (wr != 0)
            hal_console_write("ошибка записи\n");
    }
}
