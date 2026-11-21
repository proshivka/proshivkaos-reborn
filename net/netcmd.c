/* net/netcmd.c — реализация сетевых команд поверх netcore/udp/dhcp/dns/tcp.
 * Вывод — только через net_out_t, поэтому один и тот же код работает и в
 * текстовой консоли, и в окне терминала. Все ожидания внутри можно
 * прервать (см. net_set_pump в netcore.h): тогда команда печатает
 * «interrupted» и возвращается. */
#include "netcore.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "ping.h"
#include "ssh.h"
#include "entropy.h"
#include "netcmd.h"

/* ---------------- строка-сборщик ---------------- */

typedef struct { char b[200]; size_t n; } sb_t;

static void sb_init(sb_t *s) { s->n = 0; s->b[0] = '\0'; }
static void sb_str(sb_t *s, const char *t) {
    while (*t && s->n < sizeof s->b - 1) s->b[s->n++] = *t++;
    s->b[s->n] = '\0';
}
static void sb_dec(sb_t *s, unsigned v) {
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && s->n < sizeof s->b - 1) s->b[s->n++] = t[--n];
    s->b[s->n] = '\0';
}
static void sb_ip(sb_t *s, const uint8_t *ip) {
    for (int i = 0; i < 4; i++) { if (i) sb_str(s, "."); sb_dec(s, ip[i]); }
}
static void sb_mac(sb_t *s, const uint8_t *m) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char t[3] = { hx[m[i] >> 4], hx[m[i] & 15], 0 };
        if (i) sb_str(s, ":");
        sb_str(s, t);
    }
}

/* ---------------- вывод ---------------- */

static void emit(const net_out_t *o, const char *s) {
    while (*s) o->putc(o->ctx, *s++);
}

static void line(const net_out_t *o, int style, const char *s) {
    if (o->lead_nl) o->putc(o->ctx, '\n');
    if (style && o->style) o->style(o->ctx, style);
    emit(o, s);
    if (style && o->style) o->style(o->ctx, NET_STYLE_NORMAL);
    if (!o->lead_nl) o->putc(o->ctx, '\n');
    if (o->flush) o->flush(o->ctx);
}

static void info(const net_out_t *o, const char *s)  { line(o, NET_STYLE_NORMAL, s); }
static void fail(const net_out_t *o, const char *s)  { line(o, NET_STYLE_ERROR, s); }
static void say(const net_out_t *o, const sb_t *s)   { line(o, NET_STYLE_NORMAL, s->b); }

/* ---------------- разбор аргументов ---------------- */

static size_t str_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
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

/* Печатает «interrupted», если пользователь прервал операцию. */
static int was_interrupted(const net_out_t *o) {
    if (!net_aborted()) return 0;
    fail(o, "^C interrupted");
    return 1;
}

static void dns_error(const net_out_t *o, int r) {
    switch (r) {
    case DNS_ERR_NAME:     fail(o, "dns: bad name"); break;
    case DNS_ERR_TIMEOUT:  fail(o, "dns: no answer (run dhcp?)"); break;
    case DNS_ERR_NXDOMAIN: fail(o, "dns: no such name"); break;
    case DNS_ERR_NOADDR:   fail(o, "dns: no A record"); break;
    case DNS_ERR_NET:      fail(o, "dns: send error"); break;
    default:               fail(o, "dns: server error"); break;
    }
}

/* Имя или IP-литерал -> адрес. 0 — успех (иначе сообщение уже выведено). */
static int resolve_host(const net_out_t *o, const char *host, uint8_t ip[4]) {
    if (parse_ip(host, ip) == 0) return 0;
    int r = dns_resolve(host, ip);
    if (r == DNS_OK) return 0;
    if (!was_interrupted(o)) dns_error(o, r);
    return -1;
}

/* ---------------- команды ---------------- */

static void cmd_ifconfig(const net_out_t *o) {
    sb_t s;
    sb_init(&s); sb_str(&s, "mac : "); sb_mac(&s, net_cfg.mac); say(o, &s);
    sb_init(&s); sb_str(&s, "ip  : "); sb_ip(&s, net_cfg.ip);
    sb_str(&s, net_cfg.from_dhcp ? " (dhcp)" : " (default)"); say(o, &s);
    sb_init(&s); sb_str(&s, "mask: "); sb_ip(&s, net_cfg.mask); say(o, &s);
    sb_init(&s); sb_str(&s, "gw  : "); sb_ip(&s, net_cfg.gw);   say(o, &s);
    sb_init(&s); sb_str(&s, "dns : "); sb_ip(&s, net_cfg.dns);  say(o, &s);
}

static void cmd_arping(const net_out_t *o, int argc, char *argv[]) {
    uint8_t ip[4], mac[6];
    if (argc >= 2) {
        if (parse_ip(argv[1], ip) != 0) { fail(o, "usage: arping [a.b.c.d]"); return; }
    } else {
        net_copy(ip, net_cfg.gw, 4);
    }

    net_arp_flush();
    int r = net_arp_resolve(ip, mac);
    if (r == NET_OK) {
        sb_t s; sb_init(&s);
        sb_str(&s, "reply from "); sb_ip(&s, ip); sb_str(&s, ": "); sb_mac(&s, mac);
        say(o, &s);
    } else if (!was_interrupted(o)) {
        fail(o, r == NET_ERR_TX ? "arping: send error" : "arping: no reply");
    }
}

static void cmd_ping(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 2) { fail(o, "usage: ping <ip|name>"); return; }

    uint8_t ip[4];
    if (resolve_host(o, argv[1], ip) != 0) return;

    int ok = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        int r = net_ping(ip, seq);
        sb_t s; sb_init(&s);

        if (r == PING_OK) {
            ok++;
            sb_str(&s, "reply from "); sb_ip(&s, ip);
            sb_str(&s, ": seq="); sb_dec(&s, seq);
            say(o, &s);
        } else if (was_interrupted(o)) {
            return;
        } else if (r == PING_NO_ARP) {
            fail(o, "ping: no route (ARP failed)");
            return;
        } else if (r == PING_TX_ERR) {
            fail(o, "ping: send error");
            return;
        } else {
            sb_str(&s, "timeout: seq="); sb_dec(&s, seq);
            line(o, NET_STYLE_ERROR, s.b);
        }
    }

    sb_t s; sb_init(&s);
    sb_str(&s, "--- "); sb_ip(&s, ip); sb_str(&s, ": ");
    sb_dec(&s, (unsigned)ok); sb_str(&s, "/4 received");
    say(o, &s);
}

static void cmd_dhcp(const net_out_t *o) {
    info(o, "dhcp: requesting address...");
    if (dhcp_acquire() != 0) {
        if (!was_interrupted(o)) fail(o, "dhcp: no server answered");
        return;
    }
    sb_t s; sb_init(&s);
    sb_str(&s, "dhcp: got "); sb_ip(&s, net_cfg.ip);
    sb_str(&s, ", gw "); sb_ip(&s, net_cfg.gw);
    sb_str(&s, ", dns "); sb_ip(&s, net_cfg.dns);
    say(o, &s);
}

static void cmd_udpsend(const net_out_t *o, int argc, char *argv[]) {
    uint8_t ip[4];
    uint16_t port;
    if (argc < 4 || parse_ip(argv[1], ip) != 0 || parse_port(argv[2], &port) != 0) {
        fail(o, "usage: udpsend <ip> <port> <text>");
        return;
    }

    char text[128];
    size_t len = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3 && len < sizeof text - 2) text[len++] = ' ';
        for (const char *p = argv[i]; *p && len < sizeof text - 2; p++) text[len++] = *p;
    }
    text[len++] = '\n';

    int r = udp_send(ip, 40000, port, text, len);
    if (r == NET_OK)           info(o, "sent");
    else if (was_interrupted(o)) { }
    else if (r == NET_ERR_ARP) fail(o, "udpsend: no route (ARP failed)");
    else                       fail(o, "udpsend: send error");
}

static void cmd_nslookup(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 2) { fail(o, "usage: nslookup <name>"); return; }

    uint8_t ip[4];
    int r = dns_resolve(argv[1], ip);
    if (r != DNS_OK) {
        if (!was_interrupted(o)) dns_error(o, r);
        return;
    }
    sb_t s; sb_init(&s);
    sb_str(&s, argv[1]); sb_str(&s, " -> "); sb_ip(&s, ip);
    say(o, &s);
}

static void req_add(char *buf, size_t *len, size_t cap, const char *s) {
    while (*s && *len < cap - 1) buf[(*len)++] = *s++;
    buf[*len] = '\0';
}

#define HTTP_SHOW_MAX 2000

static void cmd_http(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 2) { fail(o, "usage: http <host> [path]"); return; }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    uint8_t ip[4];
    if (resolve_host(o, host, ip) != 0) return;

    sb_t s; sb_init(&s);
    sb_str(&s, "connecting to "); sb_ip(&s, ip); sb_str(&s, ":80");
    say(o, &s);

    int r = tcp_connect(ip, 80);
    if (r != TCP_OK) {
        if (!was_interrupted(o))
            fail(o, r == TCP_ERR_RESET   ? "http: port closed (RST)"
                  : r == TCP_ERR_TIMEOUT ? "http: connect timeout"
                                         : "http: connect error");
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
        if (!was_interrupted(o)) fail(o, "http: send error");
        tcp_close();
        return;
    }

    /* Тело ответа идёт «сплошным потоком» — перевод строки перед ним один. */
    if (o->lead_nl) o->putc(o->ctx, '\n');

    uint8_t buf[512];
    unsigned total = 0, shown = 0;
    int aborted = 0, err = 0;
    for (;;) {
        int n = tcp_recv(buf, sizeof buf, NET_POLL_LIMIT);
        if (n < 0) { if (net_aborted()) aborted = 1; else err = 1; break; }
        if (n == 0) break;                         /* сервер закрыл соединение */

        for (int i = 0; i < n && shown < HTTP_SHOW_MAX; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            o->putc(o->ctx, (c == '\n' || (c >= 32 && c < 127)) ? c : '.');
            shown++;
        }
        total += (unsigned)n;
        if (o->flush) o->flush(o->ctx);
    }
    tcp_close();

    if (!o->lead_nl) o->putc(o->ctx, '\n');
    if (aborted) { was_interrupted(o); return; }
    if (err) fail(o, "http: receive timeout/error");

    sb_init(&s);
    sb_str(&s, "--- received "); sb_dec(&s, total); sb_str(&s, " bytes");
    if (total > shown) sb_str(&s, " (first 2000 shown)");
    say(o, &s);
}


/* ---------------- ssh ---------------- */

/* Известные хосты: «хост:порт» → SHA-256 ключа. Живут только до перезагрузки. */
#define KNOWN_MAX 8
static struct { char host[48]; uint8_t hash[32]; int used; } known[KNOWN_MAX];
static int known_next;

typedef struct {
    const net_out_t *o;
    char host[48];
    int  esc;               /* состояние разбора escape-последовательностей */
} ssh_ctx_t;

static void raw(const net_out_t *o, int on) { if (o->raw_mode) o->raw_mode(o->ctx, on); }

/* Читает строку с клавиатуры. echo=0 — пароль. Возвращает длину, -1 — отмена (Ctrl+C). */
static int read_keys_line(const net_out_t *o, char *buf, size_t max, int echo) {
    size_t len = 0;
    raw(o, 1);
    for (;;) {
        net_yield();                                   /* интерфейс живёт, пока ждём нажатия */
        if (net_aborted()) { raw(o, 0); return -1; }
        int k = o->getkey(o->ctx);
        if (k < 0) continue;
        if (k == '\n' || k == '\r') break;
        if (k == 3) { raw(o, 0); return -1; }
        if (k == '\b' || k == 127) {
            if (len) { len--; if (echo) { o->putc(o->ctx, '\b'); if (o->flush) o->flush(o->ctx); } }
            continue;
        }
        if (k >= 32 && k < 127 && len < max - 1) {
            buf[len++] = (char)k;
            if (echo) { o->putc(o->ctx, (char)k); if (o->flush) o->flush(o->ctx); }
        }
    }
    raw(o, 0);
    buf[len] = '\0';
    if (!o->lead_nl) o->putc(o->ctx, '\n');            /* Enter не эхо-ится: закрываем строку сами */
    return (int)len;
}

static void prompt(const net_out_t *o, const char *text) {
    if (o->lead_nl) o->putc(o->ctx, '\n');
    emit(o, text);
    if (o->flush) o->flush(o->ctx);
}

static int cb_send(void *c, const uint8_t *d, size_t n) {
    (void)c;
    return tcp_send(d, n) == (int)n ? 0 : -1;
}

static int cb_recv(void *c, uint8_t *b, size_t max, long wait) {
    (void)c;
    int r = tcp_recv(b, max, wait);
    if (r >= 0) return r;                              /* байты или 0 — закрыто */
    return r == TCP_ERR_TIMEOUT ? SSH_RECV_TIMEOUT : -1;
}

/* Вывод удалённой стороны: наш экран не понимает escape-последовательностей,
 * поэтому они вырезаются; табуляция раскрывается пробелами. */
static void cb_put(void *c, const uint8_t *d, size_t n) {
    ssh_ctx_t *s = (ssh_ctx_t *)c;
    const net_out_t *o = s->o;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = d[i];
        switch (s->esc) {
        case 0:
            if (ch == 0x1b) s->esc = 1;
            else if (ch == '\n') o->putc(o->ctx, '\n');
            else if (ch == '\t') { for (int k = 0; k < 4; k++) o->putc(o->ctx, ' '); }
            else if (ch == 8) o->putc(o->ctx, '\b');
            else if (ch >= 32 && ch < 127) o->putc(o->ctx, (char)ch);
            else if (ch >= 0xC0) o->putc(o->ctx, '?');            /* начало многобайтового символа */
            break;                                                 /* \r, BEL и продолжения UTF-8 — пропуск */
        case 1:
            s->esc = (ch == '[') ? 2 : (ch == ']') ? 3 : (ch == '(' || ch == ')') ? 4 : 0;
            break;
        case 2: if (ch >= 0x40 && ch <= 0x7e) s->esc = 0; break;   /* CSI до финального байта */
        case 3: if (ch == 7) s->esc = 0; else if (ch == 0x1b) s->esc = 1; break;   /* OSC до BEL / ESC \ */
        default: s->esc = 0; break;
        }
    }
    if (o->flush) o->flush(o->ctx);
}

static int cb_key(void *c) {
    const net_out_t *o = ((ssh_ctx_t *)c)->o;
    return o->getkey ? o->getkey(o->ctx) : -1;
}
static int cb_abort(void *c) { (void)c; return net_aborted(); }
static void cb_log(void *c, const char *m) { info(((ssh_ctx_t *)c)->o, m); }
static int cb_entropy(void *c, uint8_t *b, size_t n) { (void)c; return hal_entropy(b, n); }
static void cb_session(void *c, int on) { raw(((ssh_ctx_t *)c)->o, on); }

static int cb_password(void *c, char *buf, size_t max) {
    const net_out_t *o = ((ssh_ctx_t *)c)->o;
    prompt(o, "password: ");
    return read_keys_line(o, buf, max, 0);
}

static int cb_confirm(void *c, const char *fp, const uint8_t hash[32]) {
    ssh_ctx_t *s = (ssh_ctx_t *)c;
    const net_out_t *o = s->o;
    sb_t m;

    for (int i = 0; i < KNOWN_MAX; i++) {
        if (!known[i].used || !str_eq(known[i].host, s->host)) continue;
        int same = 1;
        for (int k = 0; k < 32; k++) if (known[i].hash[k] != hash[k]) same = 0;
        if (same) return 1;
        fail(o, "WARNING: HOST KEY HAS CHANGED!");
        fail(o, "someone may be eavesdropping, or the server was reinstalled");
        sb_init(&m); sb_str(&m, "new key: "); sb_str(&m, fp); fail(o, m.b);
        fail(o, "host key verification failed");
        return 0;
    }

    sb_init(&m); sb_str(&m, "host "); sb_str(&m, s->host); sb_str(&m, " is not known yet");
    info(o, m.b);
    sb_init(&m); sb_str(&m, "ed25519 key: "); sb_str(&m, fp);
    info(o, m.b);
    prompt(o, "continue connecting? (yes/no) ");

    char ans[8];
    if (read_keys_line(o, ans, sizeof ans, 1) < 0 || !str_eq(ans, "yes")) return 0;

    int slot = known_next++ % KNOWN_MAX;
    known[slot].used = 1;
    for (int k = 0; k < 47 && s->host[k]; k++) { known[slot].host[k] = s->host[k]; known[slot].host[k + 1] = 0; }
    for (int k = 0; k < 32; k++) known[slot].hash[k] = hash[k];
    return 1;
}

/* ssh [-p порт] пользователь@хост [команда...] */
static void cmd_ssh(const net_out_t *o, int argc, char *argv[]) {
    if (!o->getkey) { fail(o, "ssh: keyboard input is not available here"); return; }

    uint16_t port = 22;
    int i = 1;
    if (i + 1 < argc && str_eq(argv[i], "-p")) {
        if (parse_port(argv[i + 1], &port) != 0) { fail(o, "ssh: bad port"); return; }
        i += 2;
    }
    if (i >= argc) { fail(o, "usage: ssh [-p port] user@host [command]"); return; }

    char user[40];
    const char *at = argv[i];
    size_t ul = 0;
    while (at[ul] && at[ul] != '@') ul++;
    if (!at[ul] || ul == 0 || ul >= sizeof user || str_len(at + ul + 1) == 0 || str_len(at + ul + 1) >= 40) {
        fail(o, "usage: ssh [-p port] user@host [command]");
        return;
    }
    for (size_t k = 0; k < ul; k++) user[k] = at[k];
    user[ul] = '\0';
    const char *hname = at + ul + 1;
    i++;

    char command[200];
    size_t cl = 0;
    for (; i < argc; i++) {
        if (cl && cl < sizeof command - 1) command[cl++] = ' ';
        for (const char *p = argv[i]; *p && cl < sizeof command - 1; p++) command[cl++] = *p;
    }
    command[cl] = '\0';

    uint8_t ip[4];
    if (resolve_host(o, hname, ip) != 0) return;

    sb_t s;
    sb_init(&s); sb_str(&s, "connecting to "); sb_ip(&s, ip); sb_str(&s, ":"); sb_dec(&s, port);
    say(o, &s);

    int r = tcp_connect(ip, port);
    if (r != TCP_OK) {
        if (!was_interrupted(o))
            fail(o, r == TCP_ERR_RESET   ? "ssh: connection refused"
                  : r == TCP_ERR_TIMEOUT ? "ssh: connect timeout"
                                         : "ssh: connect error");
        return;
    }

    static ssh_ctx_t ctx;
    ctx.o = o;
    ctx.esc = 0;
    sb_init(&s); sb_str(&s, hname); sb_str(&s, ":"); sb_dec(&s, port);
    for (size_t k = 0; k < sizeof ctx.host - 1 && s.b[k]; k++) { ctx.host[k] = s.b[k]; ctx.host[k + 1] = 0; }

    ssh_io_t io = { &ctx, cb_send, cb_recv, cb_put, cb_key, cb_abort, cb_log, cb_confirm,
                    cb_entropy, cb_session, cb_password };
    ssh_params_t p = { 0 };
    p.user = user;
    p.password = 0;                                    /* спросим после проверки ключа сервера */
    p.command = cl ? command : 0;
    p.pty = cl ? 0 : 1;
    p.cols = 40;                                       /* ширина экрана терминала: подстройте под gconsole */
    p.rows = 12;
    p.term = "dumb";

    r = ssh_run(&io, &p);
    tcp_close();

    if (r == SSH_OK) {
        if (!o->lead_nl) { /* строка закончилась сама */ }
        if (p.exit_status > 0) {
            sb_init(&s); sb_str(&s, "ssh: remote exit status "); sb_dec(&s, (unsigned)p.exit_status);
            say(o, &s);
        } else {
            info(o, "ssh: connection closed");
        }
    } else if (r == SSH_ERR_ABORT || net_aborted()) {
        was_interrupted(o);
    } else {
        sb_init(&s); sb_str(&s, "ssh: "); sb_str(&s, ssh_strerror(r));
        fail(o, s.b);
    }
}

/* ---------------- таблица и вход ---------------- */

static const char *const NAMES[] = {
    "ifconfig", "arping", "ping", "dhcp", "udpsend", "nslookup", "http", "ssh"
};

size_t netcmd_count(void) { return sizeof NAMES / sizeof NAMES[0]; }
const char *netcmd_name(size_t i) { return i < netcmd_count() ? NAMES[i] : 0; }

int netcmd_is(const char *name) {
    for (size_t i = 0; i < netcmd_count(); i++)
        if (str_eq(name, NAMES[i])) return 1;
    return 0;
}

int netcmd_exec(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 1 || !netcmd_is(argv[0])) return 0;

    net_abort_clear();
    const char *c = argv[0];
    if      (str_eq(c, "ifconfig")) cmd_ifconfig(o);
    else if (str_eq(c, "arping"))   cmd_arping(o, argc, argv);
    else if (str_eq(c, "ping"))     cmd_ping(o, argc, argv);
    else if (str_eq(c, "dhcp"))     cmd_dhcp(o);
    else if (str_eq(c, "udpsend"))  cmd_udpsend(o, argc, argv);
    else if (str_eq(c, "nslookup")) cmd_nslookup(o, argc, argv);
    else if (str_eq(c, "http"))     cmd_http(o, argc, argv);
    else if (str_eq(c, "ssh"))      cmd_ssh(o, argc, argv);
    return 1;
}
