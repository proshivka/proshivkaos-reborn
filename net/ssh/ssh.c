/* net/ssh/ssh.c — клиент SSH-2 (RFC 4253/4252/4254, RFC 8731 и
 * PROTOCOL.chacha20poly1305 из OpenSSH). Все буферы статические. */
#include "ssh.h"
#include "sha2.h"
#include "ed25519.h"
#include "chachapoly.h"

/* ---------------- номера сообщений ---------------- */
enum {
    MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4,
    MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6,
    MSG_KEXINIT = 20, MSG_NEWKEYS = 21,
    MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY = 31,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52,
    MSG_USERAUTH_BANNER = 53,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_FAILURE = 82,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92,
    MSG_CHANNEL_WINDOW_ADJUST = 93, MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95,
    MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
    MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100
};

#define CLIENT_VERSION   "SSH-2.0-ProshivkaOS_0.1"
#define RX_MAX           20000
#define PKT_MAX          16384          /* больше этого полезной нагрузки не принимаем */
#define LOCAL_WINDOW     8192
#define LOCAL_MAX_PACKET 2048

/* ---------------- состояние ---------------- */

typedef struct {
    const ssh_io_t *io;

    uint8_t  rx[RX_MAX];  size_t rx_len;           /* принятое из сети, ещё не разобранное */
    uint8_t  in[PKT_MAX];                          /* полезная нагрузка текущего пакета */
    uint8_t  out[PKT_MAX + 64];                    /* собираемый исходящий пакет */

    uint32_t seq_in, seq_out;
    int      enc_in, enc_out;
    ssh_chachapoly_t cc_in, cc_out;

    uint8_t  session_id[32];
    uint8_t  v_c[64];  size_t v_c_len;
    uint8_t  v_s[256]; size_t v_s_len;
    uint8_t  i_c[640]; size_t i_c_len;
    uint8_t  i_s[8192]; size_t i_s_len;

    uint8_t  rng_key[32];
    uint64_t rng_ctr;

    /* канал */
    uint32_t remote_chan, remote_window, remote_max_pkt;
    uint32_t local_window;
    int      sent_close, got_close, got_eof;
} ssh_t;

static ssh_t S;

static void wipe(void *p, size_t n) { volatile uint8_t *v = (volatile uint8_t *)p; while (n--) *v++ = 0; }
static void copy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int mem_eq(const uint8_t *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

static void say(const char *msg) { if (S.io->log) S.io->log(S.io->ctx, msg); }

/* ---------------- чтение/запись полей ---------------- */

typedef struct { const uint8_t *p; size_t n, o; int bad; } rd_t;
static rd_t rd_new(const uint8_t *p, size_t n) { rd_t r = { p, n, 0, 0 }; return r; }
static uint8_t rd_u8(rd_t *r) { if (r->o + 1 > r->n) { r->bad = 1; return 0; } return r->p[r->o++]; }
static uint32_t rd_u32(rd_t *r) {
    if (r->o + 4 > r->n) { r->bad = 1; return 0; }
    uint32_t v = ((uint32_t)r->p[r->o] << 24) | ((uint32_t)r->p[r->o+1] << 16) |
                 ((uint32_t)r->p[r->o+2] << 8) | r->p[r->o+3];
    r->o += 4;
    return v;
}
static const uint8_t *rd_str(rd_t *r, size_t *len) {
    uint32_t l = rd_u32(r);
    if (r->bad || l > r->n - r->o) { r->bad = 1; *len = 0; return 0; }
    const uint8_t *p = r->p + r->o;
    r->o += l;
    *len = l;
    return p;
}

typedef struct { uint8_t *p; size_t n, max; int bad; } wr_t;
static wr_t wr_new(uint8_t *p, size_t max) { wr_t w = { p, 0, max, 0 }; return w; }
static void wr_u8(wr_t *w, uint8_t v) { if (w->n + 1 > w->max) { w->bad = 1; return; } w->p[w->n++] = v; }
static void wr_u32(wr_t *w, uint32_t v) {
    wr_u8(w, (uint8_t)(v >> 24)); wr_u8(w, (uint8_t)(v >> 16)); wr_u8(w, (uint8_t)(v >> 8)); wr_u8(w, (uint8_t)v);
}
static void wr_bytes(wr_t *w, const void *d, size_t n) {
    if (w->n + n > w->max) { w->bad = 1; return; }
    copy(w->p + w->n, d, n); w->n += n;
}
static void wr_str(wr_t *w, const void *d, size_t n) { wr_u32(w, (uint32_t)n); wr_bytes(w, d, n); }
static void wr_cstr(wr_t *w, const char *s) { wr_str(w, s, slen(s)); }

/* ---------------- случайные числа ----------------
 * Источник даёт «сырую» случайность (на x86 — дрожание таймеров), здесь она
 * пропускается через SHA-256 в ключ ChaCha20 и размножается. После каждой
 * выдачи ключ заменяется (fast key erasure) — прошлые выходы не восстановить. */

static int rng_seed(void) {
    uint8_t e[64];
    if (!S.io->entropy || S.io->entropy(S.io->ctx, e, sizeof e) != 0) return SSH_ERR_ENTROPY;
    sha256_t h;
    sha256_init(&h);
    sha256_update(&h, S.rng_key, 32);
    sha256_update(&h, e, sizeof e);
    sha256_final(&h, S.rng_key);
    wipe(e, sizeof e);
    return SSH_OK;
}

static void rng_fill(uint8_t *out, size_t n) {
    while (n) {
        uint8_t blk[64], nonce[8], zeros[64] = {0};
        for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)(S.rng_ctr >> (56 - 8 * i));
        S.rng_ctr++;
        chacha20_xor(S.rng_key, nonce, 0, zeros, blk, 64);
        size_t take = n < 32 ? n : 32;                 /* первые 32 байта — вывод, вторые 32 — новый ключ */
        copy(out, blk, take);
        copy(S.rng_key, blk + 32, 32);
        wipe(blk, sizeof blk);
        out += take; n -= take;
    }
}

/* ---------------- пакетный уровень ---------------- */

static int send_raw(const uint8_t *d, size_t n) {
    return S.io->send(S.io->ctx, d, n) == 0 ? SSH_OK : SSH_ERR_IO;
}

static int send_packet(const uint8_t *payload, size_t n) {
    if (n > PKT_MAX - 300) return SSH_ERR_TOOBIG;

    /* Длина выравнивается до 8; при шифре chacha поле длины в выравнивание не входит. */
    size_t base = S.enc_out ? 1 + n : 4 + 1 + n;
    size_t pad = 8 - (base % 8);
    if (pad < 4) pad += 8;
    uint32_t plen = (uint32_t)(1 + n + pad);

    uint8_t *b = S.out;
    b[0] = (uint8_t)(plen >> 24); b[1] = (uint8_t)(plen >> 16); b[2] = (uint8_t)(plen >> 8); b[3] = (uint8_t)plen;
    b[4] = (uint8_t)pad;
    copy(b + 5, payload, n);
    rng_fill(b + 5 + n, pad);

    size_t total = 4 + plen;
    if (S.enc_out) {
        ssh_chachapoly_seal(&S.cc_out, S.seq_out, b, plen);
        total += 16;
    }
    S.seq_out++;
    return send_raw(b, total);
}

static int fill_rx(long wait) {
    if (S.rx_len >= RX_MAX) return SSH_ERR_TOOBIG;
    int n = S.io->recv(S.io->ctx, S.rx + S.rx_len, RX_MAX - S.rx_len, wait);
    if (n > 0) { S.rx_len += (size_t)n; return n; }
    if (n == 0) return SSH_ERR_IO;                      /* закрыто */
    if (n == SSH_RECV_TIMEOUT) return 0;
    return SSH_ERR_IO;
}

static void rx_consume(size_t n) {
    for (size_t i = 0; i + n < S.rx_len; i++) S.rx[i] = S.rx[i + n];
    S.rx_len -= n;
}

/* 1 — пакет собран в S.in (длина в *n), 0 — пока неполный, < 0 — ошибка. */
static int try_extract(size_t *n) {
    if (S.rx_len < 4) return 0;

    uint32_t plen;
    if (S.enc_in) plen = ssh_chachapoly_peek_len(&S.cc_in, S.seq_in, S.rx);
    else plen = ((uint32_t)S.rx[0] << 24) | ((uint32_t)S.rx[1] << 16) | ((uint32_t)S.rx[2] << 8) | S.rx[3];

    if (plen < 5 || plen > PKT_MAX) return SSH_ERR_TOOBIG;
    size_t total = 4 + (size_t)plen + (S.enc_in ? 16 : 0);
    if (total > RX_MAX) return SSH_ERR_TOOBIG;
    if (S.rx_len < total) return 0;

    if (S.enc_in && ssh_chachapoly_open(&S.cc_in, S.seq_in, S.rx, plen) != 0) return SSH_ERR_PROTO;

    uint8_t pad = S.rx[4];
    if ((size_t)pad + 1 > plen) return SSH_ERR_PROTO;
    size_t plain = plen - pad - 1;
    copy(S.in, S.rx + 5, plain);
    rx_consume(total);
    S.seq_in++;
    *n = plain;
    return 1;
}

/* Получить следующий пакет. wait — как долго ждать новых байт из сети.
 * 1 — есть пакет, 0 — не дождались, < 0 — ошибка. */
static int recv_packet(size_t *n, long wait) {
    for (;;) {
        int r = try_extract(n);
        if (r != 0) return r;
        if (S.io->should_abort && S.io->should_abort(S.io->ctx)) return SSH_ERR_ABORT;
        int f = fill_rx(wait);
        if (f < 0) return f;
        if (f == 0) return 0;
    }
}

/* Служебные сообщения, которые в любой момент можно пропустить.
 * 1 — это был служебный пакет (обработан), 0 — нет, < 0 — ошибка. */
static int handle_transport(size_t n) {
    if (n < 1) return SSH_ERR_PROTO;
    switch (S.in[0]) {
    case MSG_IGNORE: case MSG_UNIMPLEMENTED:
        return 1;
    case MSG_DEBUG:
        return 1;
    case MSG_DISCONNECT: {
        rd_t r = rd_new(S.in, n);
        rd_u8(&r);
        uint32_t code = rd_u32(&r);
        size_t dl; const uint8_t *d = rd_str(&r, &dl);
        char msg[160] = "server disconnected: ";
        size_t o = slen(msg);
        if (!r.bad) for (size_t i = 0; i < dl && o < sizeof msg - 1; i++) msg[o++] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
        msg[o] = 0;
        say(msg);
        (void)code;
        return SSH_ERR_DISCONNECT;
    }
    case MSG_KEXINIT:
        return SSH_ERR_REKEY;
    default:
        return 0;
    }
}

/* Ждать пакет одного из нужных типов, пропуская служебные. */
static int expect(int t1, int t2, size_t *n) {
    for (;;) {
        int r = recv_packet(n, SSH_WAIT_LONG);
        if (r < 0) return r;
        if (r == 0) return SSH_ERR_TIMEOUT;
        if (*n >= 1 && (S.in[0] == t1 || S.in[0] == t2)) return SSH_OK;   /* сначала — то, чего ждём (иначе первый KEXINIT сочтётся сменой ключей) */
        int h = handle_transport(*n);
        if (h < 0) return h;
        if (h == 1) continue;
        return SSH_ERR_PROTO;
    }
}

/* ---------------- вспомогательное ---------------- */

static int list_has(const uint8_t *list, size_t n, const char *name) {
    size_t nl = slen(name), i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == nl && mem_eq(list + i, name, nl)) return 1;
        i = j + 1;
    }
    return 0;
}

static void b64_nopad(const uint8_t *d, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = T[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = T[v & 63];
    }
    out[o] = 0;
}

static void h_str(sha256_t *h, const void *d, size_t n) {
    uint8_t l[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    sha256_update(h, l, 4);
    sha256_update(h, d, n);
}

/* Общий секрет как mpint: без ведущих нулей, с нулевым байтом впереди,
 * если старший бит установлен. Возвращает длину в buf (с 4-байтовым префиксом). */
static size_t mpint_encode(const uint8_t k[32], uint8_t *buf) {
    size_t i = 0;
    while (i < 32 && k[i] == 0) i++;
    size_t len = 32 - i;
    int pad = (len > 0 && (k[i] & 0x80)) ? 1 : 0;
    uint32_t total = (uint32_t)(len + pad);
    buf[0] = (uint8_t)(total >> 24); buf[1] = (uint8_t)(total >> 16);
    buf[2] = (uint8_t)(total >> 8);  buf[3] = (uint8_t)total;
    size_t o = 4;
    if (pad) buf[o++] = 0;
    for (size_t j = 0; j < len; j++) buf[o++] = k[i + j];
    return o;
}

static void derive_key(const uint8_t *k_mpint, size_t klen, const uint8_t H[32], char letter, uint8_t out[64]) {
    sha256_t h;
    sha256_init(&h);
    sha256_update(&h, k_mpint, klen);
    sha256_update(&h, H, 32);
    sha256_update(&h, &letter, 1);
    sha256_update(&h, S.session_id, 32);
    sha256_final(&h, out);

    sha256_init(&h);
    sha256_update(&h, k_mpint, klen);
    sha256_update(&h, H, 32);
    sha256_update(&h, out, 32);
    sha256_final(&h, out + 32);
}

/* ---------------- рукопожатие ---------------- */

static int exchange_versions(void) {
    static const char ver[] = CLIENT_VERSION "\r\n";
    S.v_c_len = slen(CLIENT_VERSION);
    copy(S.v_c, CLIENT_VERSION, S.v_c_len);
    int r = send_raw((const uint8_t *)ver, sizeof ver - 1);
    if (r) return r;

    for (int lines = 0; lines < 32; lines++) {
        size_t nl;
        for (;;) {
            nl = S.rx_len;
            for (size_t i = 0; i < S.rx_len; i++) if (S.rx[i] == '\n') { nl = i; break; }
            if (nl < S.rx_len) break;
            if (S.rx_len > 255 + 2) return SSH_ERR_PROTO;
            int f = fill_rx(SSH_WAIT_LONG);
            if (f < 0) return f;
            if (f == 0) return SSH_ERR_TIMEOUT;
        }
        size_t len = nl;
        if (len && S.rx[len - 1] == '\r') len--;
        int is_ver = len >= 4 && mem_eq(S.rx, "SSH-", 4);
        if (is_ver) {
            if (!(len >= 8 && (mem_eq(S.rx, "SSH-2.0-", 8) || (len >= 9 && mem_eq(S.rx, "SSH-1.99-", 9)))))
                return SSH_ERR_PROTO;
            if (len > sizeof S.v_s) return SSH_ERR_PROTO;
            copy(S.v_s, S.rx, len);
            S.v_s_len = len;
            rx_consume(nl + 1);
            return SSH_OK;
        }
        rx_consume(nl + 1);                             /* строки до версии — приветствие, пропускаем */
    }
    return SSH_ERR_PROTO;
}

static int send_kexinit(void) {
    uint8_t pl[640];
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_KEXINIT);
    uint8_t cookie[16];
    rng_fill(cookie, 16);
    wr_bytes(&w, cookie, 16);
    wr_cstr(&w, "curve25519-sha256,curve25519-sha256@libssh.org");   /* kex */
    wr_cstr(&w, "ssh-ed25519");                                      /* host key */
    wr_cstr(&w, "chacha20-poly1305@openssh.com");                    /* enc c2s */
    wr_cstr(&w, "chacha20-poly1305@openssh.com");                    /* enc s2c */
    wr_cstr(&w, "hmac-sha2-256");                                    /* mac c2s (при AEAD не используется) */
    wr_cstr(&w, "hmac-sha2-256");                                    /* mac s2c */
    wr_cstr(&w, "none"); wr_cstr(&w, "none");                        /* compression */
    wr_cstr(&w, ""); wr_cstr(&w, "");                                /* languages */
    wr_u8(&w, 0);                                                    /* first_kex_packet_follows */
    wr_u32(&w, 0);                                                   /* reserved */
    if (w.bad) return SSH_ERR_TOOBIG;
    copy(S.i_c, pl, w.n);
    S.i_c_len = w.n;
    return send_packet(pl, w.n);
}

/* Разбор KEXINIT сервера и проверка, что у нас есть общие алгоритмы. */
static int check_server_kexinit(size_t n) {
    if (n > sizeof S.i_s) return SSH_ERR_TOOBIG;
    copy(S.i_s, S.in, n);
    S.i_s_len = n;

    rd_t r = rd_new(S.in, n);
    rd_u8(&r);
    r.o += 16;                                            /* cookie */
    size_t ln[5]; const uint8_t *l[5];
    for (int i = 0; i < 5; i++) l[i] = rd_str(&r, &ln[i]);   /* kex, hostkey, enc c2s, enc s2c, mac c2s */
    if (r.bad) return SSH_ERR_PROTO;

    int kex_ok = list_has(l[0], ln[0], "curve25519-sha256") || list_has(l[0], ln[0], "curve25519-sha256@libssh.org");
    int hk_ok  = list_has(l[1], ln[1], "ssh-ed25519");
    int enc_ok = list_has(l[2], ln[2], "chacha20-poly1305@openssh.com") &&
                 list_has(l[3], ln[3], "chacha20-poly1305@openssh.com");
    if (kex_ok && hk_ok && enc_ok) return SSH_OK;

    char msg[200];
    size_t o = 0;
    const char *what = !kex_ok ? "key exchange (curve25519-sha256)"
                     : !hk_ok  ? "host key type (ssh-ed25519)"
                               : "cipher (chacha20-poly1305@openssh.com)";
    const char *pre = "server does not offer ";
    for (size_t i = 0; pre[i]; i++) msg[o++] = pre[i];
    for (size_t i = 0; what[i] && o < sizeof msg - 1; i++) msg[o++] = what[i];
    msg[o] = 0;
    say(msg);
    return SSH_ERR_ALGO;
}

static int key_exchange(void) {
    size_t n;
    int r;

    r = send_kexinit();
    if (r) return r;
    r = expect(MSG_KEXINIT, MSG_KEXINIT, &n);
    if (r) return r;
    r = check_server_kexinit(n);
    if (r) return r;

    /* Временная пара X25519. */
    uint8_t priv[32], pub[32];
    rng_fill(priv, 32);
    x25519_base(pub, priv);

    uint8_t pl[64];
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_KEX_ECDH_INIT);
    wr_str(&w, pub, 32);
    r = send_packet(pl, w.n);
    if (r) { wipe(priv, 32); return r; }

    r = expect(MSG_KEX_ECDH_REPLY, MSG_KEX_ECDH_REPLY, &n);
    if (r) { wipe(priv, 32); return r; }

    rd_t rd = rd_new(S.in, n);
    rd_u8(&rd);
    size_t ks_len, qs_len, sg_len;
    const uint8_t *ks = rd_str(&rd, &ks_len);
    const uint8_t *qs = rd_str(&rd, &qs_len);
    const uint8_t *sg = rd_str(&rd, &sg_len);
    if (rd.bad || qs_len != 32) { wipe(priv, 32); return SSH_ERR_PROTO; }

    /* Ключ сервера: string "ssh-ed25519", string pk[32]. */
    rd_t kr = rd_new(ks, ks_len);
    size_t tl, pkl;
    const uint8_t *tname = rd_str(&kr, &tl);
    const uint8_t *hostpk = rd_str(&kr, &pkl);
    if (kr.bad || tl != 11 || !mem_eq(tname, "ssh-ed25519", 11) || pkl != 32) { wipe(priv, 32); return SSH_ERR_PROTO; }

    /* Подпись: string "ssh-ed25519", string sig[64]. */
    rd_t sr = rd_new(sg, sg_len);
    size_t sl, sigl;
    const uint8_t *sname = rd_str(&sr, &sl);
    const uint8_t *sig = rd_str(&sr, &sigl);
    if (sr.bad || sl != 11 || !mem_eq(sname, "ssh-ed25519", 11) || sigl != 64) { wipe(priv, 32); return SSH_ERR_PROTO; }

    /* Общий секрет. Нулевой результат означает «плохую» точку сервера. */
    uint8_t K[32];
    x25519(K, priv, qs);
    wipe(priv, 32);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= K[i];
    if (!acc) return SSH_ERR_PROTO;

    uint8_t kmp[40];
    size_t kmp_len = mpint_encode(K, kmp);
    wipe(K, 32);

    /* H = SHA256(V_C, V_S, I_C, I_S, K_S, Q_C, Q_S, K) */
    uint8_t H[32];
    sha256_t h;
    sha256_init(&h);
    h_str(&h, S.v_c, S.v_c_len);
    h_str(&h, S.v_s, S.v_s_len);
    h_str(&h, S.i_c, S.i_c_len);
    h_str(&h, S.i_s, S.i_s_len);
    h_str(&h, ks, ks_len);
    h_str(&h, pub, 32);
    h_str(&h, qs, 32);
    sha256_update(&h, kmp, kmp_len);
    sha256_final(&h, H);

    if (ed25519_verify(sig, H, 32, hostpk) != 0) { say("host key signature is invalid"); return SSH_ERR_HOSTKEY; }

    /* Отпечаток ключа и решение пользователя. */
    uint8_t fp_hash[32];
    sha256(ks, ks_len, fp_hash);
    char fp[64] = "SHA256:";
    b64_nopad(fp_hash, 32, fp + 7);
    if (!S.io->confirm_hostkey || !S.io->confirm_hostkey(S.io->ctx, fp, fp_hash)) {
        wipe(kmp, sizeof kmp);
        return SSH_ERR_HOSTKEY_REJ;
    }

    copy(S.session_id, H, 32);                           /* первый обмен ключами — это session id */

    /* NEWKEYS в обе стороны; ключи: C — клиент→сервер, D — сервер→клиент. */
    uint8_t nk = MSG_NEWKEYS;
    r = send_packet(&nk, 1);
    if (r) { wipe(kmp, sizeof kmp); return r; }

    uint8_t kc[64], kd[64];
    derive_key(kmp, kmp_len, H, 'C', kc);
    derive_key(kmp, kmp_len, H, 'D', kd);
    wipe(kmp, sizeof kmp);
    ssh_chachapoly_init(&S.cc_out, kc);
    ssh_chachapoly_init(&S.cc_in, kd);
    wipe(kc, 64); wipe(kd, 64);
    S.enc_out = 1;

    r = expect(MSG_NEWKEYS, MSG_NEWKEYS, &n);
    if (r) return r;
    S.enc_in = 1;
    return SSH_OK;
}

/* ---------------- вход ---------------- */

static int authenticate(ssh_params_t *p) {
    uint8_t pl[512];
    size_t n;
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_SERVICE_REQUEST);
    wr_cstr(&w, "ssh-userauth");
    int r = send_packet(pl, w.n);
    if (r) return r;
    r = expect(MSG_SERVICE_ACCEPT, MSG_SERVICE_ACCEPT, &n);
    if (r) return r;

    char pwbuf[208];
    const char *pw = p->password;
    if (!pw) {
        if (!S.io->get_password) return SSH_ERR_AUTH;
        int got = S.io->get_password(S.io->ctx, pwbuf, 200);
        if (got < 0) return SSH_ERR_ABORT;
        pwbuf[got] = 0;
        pw = pwbuf;
    }
    size_t ul = slen(p->user), pwl = slen(pw);
    if (ul > 100 || pwl > 200) { wipe(pwbuf, sizeof pwbuf); return SSH_ERR_TOOBIG; }
    w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_USERAUTH_REQUEST);
    wr_str(&w, p->user, ul);
    wr_cstr(&w, "ssh-connection");
    wr_cstr(&w, "password");
    wr_u8(&w, 0);
    wr_str(&w, pw, pwl);
    r = send_packet(pl, w.n);
    wipe(pl, sizeof pl);
    wipe(pwbuf, sizeof pwbuf);
    if (p->password) wipe(p->password, pwl);
    if (r) return r;

    for (;;) {
        for (;;) {
            r = recv_packet(&n, SSH_WAIT_LONG);
            if (r < 0) return r;
            if (r == 0) return SSH_ERR_TIMEOUT;
            int h = handle_transport(n);
            if (h < 0) return h;
            if (h == 0) break;
        }
        switch (S.in[0]) {
        case MSG_USERAUTH_SUCCESS: return SSH_OK;
        case MSG_USERAUTH_BANNER: {
            rd_t rd = rd_new(S.in, n);
            rd_u8(&rd);
            size_t bl; const uint8_t *b = rd_str(&rd, &bl);
            if (!rd.bad) {
                char msg[200];
                size_t o = 0;
                for (size_t i = 0; i < bl && o < sizeof msg - 1; i++)
                    if (b[i] >= 32 && b[i] < 127) msg[o++] = (char)b[i]; else if (b[i] == '\n' && o) { msg[o] = 0; say(msg); o = 0; }
                msg[o] = 0;
                if (o) say(msg);
            }
            break;
        }
        case MSG_USERAUTH_FAILURE: {
            rd_t rd = rd_new(S.in, n);
            rd_u8(&rd);
            size_t ml; const uint8_t *m = rd_str(&rd, &ml);
            if (!rd.bad && !list_has(m, ml, "password"))
                say("server does not allow password login");
            return SSH_ERR_AUTH;
        }
        case 60:                                         /* PASSWD_CHANGEREQ */
            say("server asks to change the password (not supported)");
            return SSH_ERR_AUTH;
        default:
            return SSH_ERR_PROTO;
        }
    }
}

/* ---------------- канал ---------------- */

/* Ответить на служебные запросы, пока ждём ответ на свой запрос канала.
 * Возвращает 1, если пакет обработан и надо читать дальше. */
static int channel_common(size_t n) {
    rd_t r = rd_new(S.in, n);
    uint8_t t = rd_u8(&r);
    if (t == MSG_CHANNEL_WINDOW_ADJUST) {
        rd_u32(&r);
        uint32_t add = rd_u32(&r);
        if (!r.bad) S.remote_window += add;
        return 1;
    }
    if (t == MSG_GLOBAL_REQUEST) {
        size_t l; rd_str(&r, &l);
        uint8_t want = rd_u8(&r);
        if (!r.bad && want) { uint8_t f = MSG_REQUEST_FAILURE; send_packet(&f, 1); }
        return 1;
    }
    return 0;
}

static int send_channel_request(const char *type, int want_reply, const uint8_t *extra, size_t extra_len) {
    uint8_t pl[PKT_MAX / 4];
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_CHANNEL_REQUEST);
    wr_u32(&w, S.remote_chan);
    wr_cstr(&w, type);
    wr_u8(&w, (uint8_t)want_reply);
    wr_bytes(&w, extra, extra_len);
    if (w.bad) return SSH_ERR_TOOBIG;
    return send_packet(pl, w.n);
}

static int wait_channel_reply(void) {
    size_t n;
    for (;;) {
        int r = recv_packet(&n, SSH_WAIT_LONG);
        if (r < 0) return r;
        if (r == 0) return SSH_ERR_TIMEOUT;
        int h = handle_transport(n);
        if (h < 0) return h;
        if (h == 1) continue;
        if (channel_common(n)) continue;
        if (S.in[0] == MSG_CHANNEL_SUCCESS) return SSH_OK;
        if (S.in[0] == MSG_CHANNEL_FAILURE) return SSH_ERR_CHANNEL;
        if (S.in[0] == MSG_CHANNEL_CLOSE) return SSH_ERR_CHANNEL;
        /* данные, пришедшие раньше ответа, не теряем */
        if (S.in[0] == MSG_CHANNEL_DATA || S.in[0] == MSG_CHANNEL_EXTENDED_DATA) {
            rd_t rd = rd_new(S.in, n);
            uint8_t t = rd_u8(&rd);
            rd_u32(&rd);
            if (t == MSG_CHANNEL_EXTENDED_DATA) rd_u32(&rd);
            size_t dl; const uint8_t *d = rd_str(&rd, &dl);
            if (!rd.bad && S.io->put_out) S.io->put_out(S.io->ctx, d, dl);
            continue;
        }
        return SSH_ERR_PROTO;
    }
}

static int open_session(ssh_params_t *p) {
    uint8_t pl[PKT_MAX / 4];
    size_t n;
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_CHANNEL_OPEN);
    wr_cstr(&w, "session");
    wr_u32(&w, 0);                                       /* наш номер канала */
    wr_u32(&w, LOCAL_WINDOW);
    wr_u32(&w, LOCAL_MAX_PACKET);
    int r = send_packet(pl, w.n);
    if (r) return r;

    for (;;) {
        r = recv_packet(&n, SSH_WAIT_LONG);
        if (r < 0) return r;
        if (r == 0) return SSH_ERR_TIMEOUT;
        int h = handle_transport(n);
        if (h < 0) return h;
        if (h == 1) continue;
        if (S.in[0] == MSG_GLOBAL_REQUEST && channel_common(n)) continue;
        if (S.in[0] == MSG_CHANNEL_OPEN_CONFIRMATION) break;
        if (S.in[0] == MSG_CHANNEL_OPEN_FAILURE) { say("server refused to open a session"); return SSH_ERR_CHANNEL; }
        return SSH_ERR_PROTO;
    }

    rd_t rd = rd_new(S.in, n);
    rd_u8(&rd);
    rd_u32(&rd);                                         /* наш номер */
    S.remote_chan = rd_u32(&rd);
    S.remote_window = rd_u32(&rd);
    S.remote_max_pkt = rd_u32(&rd);
    if (rd.bad) return SSH_ERR_PROTO;
    S.local_window = LOCAL_WINDOW;

    if (p->pty) {
        uint8_t ex[160];
        wr_t e = wr_new(ex, sizeof ex);
        wr_cstr(&e, p->term ? p->term : "dumb");
        wr_u32(&e, (uint32_t)(p->cols > 0 ? p->cols : 80));
        wr_u32(&e, (uint32_t)(p->rows > 0 ? p->rows : 24));
        wr_u32(&e, 0); wr_u32(&e, 0);
        wr_u32(&e, 1); wr_u8(&e, 0);                     /* режимы терминала: пусто (только TTY_OP_END) */
        r = send_channel_request("pty-req", 1, ex, e.n);
        if (r) return r;
        r = wait_channel_reply();
        if (r) { say("server refused the terminal (pty)"); return r; }
    }

    if (p->command) {
        uint8_t ex[600];
        wr_t e = wr_new(ex, sizeof ex);
        wr_cstr(&e, p->command);
        if (e.bad) return SSH_ERR_TOOBIG;
        r = send_channel_request("exec", 1, ex, e.n);
    } else {
        r = send_channel_request("shell", 1, 0, 0);
    }
    if (r) return r;
    r = wait_channel_reply();
    if (r) { say(p->command ? "server refused to run the command" : "server refused to start a shell"); return r; }
    return SSH_OK;
}

static int channel_send_data(const uint8_t *d, size_t n) {
    uint8_t pl[PKT_MAX / 4];
    while (n) {
        /* ждём окно, если сервер его закрыл (сервер откроет WINDOW_ADJUST'ом) */
        int spins = 0;
        while (S.remote_window == 0 && spins++ < 200) {
            size_t pn;
            int r = recv_packet(&pn, SSH_WAIT_SHORT);
            if (r < 0) return r;
            if (r == 1) { int h = handle_transport(pn); if (h < 0) return h; if (h == 0) channel_common(pn); }
        }
        if (S.remote_window == 0) return SSH_ERR_TIMEOUT;

        size_t chunk = n;
        if (chunk > S.remote_window) chunk = S.remote_window;
        if (chunk > S.remote_max_pkt) chunk = S.remote_max_pkt;
        if (chunk > 1024) chunk = 1024;

        wr_t w = wr_new(pl, sizeof pl);
        wr_u8(&w, MSG_CHANNEL_DATA);
        wr_u32(&w, S.remote_chan);
        wr_str(&w, d, chunk);
        int r = send_packet(pl, w.n);
        if (r) return r;
        S.remote_window -= (uint32_t)chunk;
        d += chunk; n -= chunk;
    }
    return SSH_OK;
}

static void channel_adjust_window(uint32_t consumed) {
    if (S.local_window >= consumed) S.local_window -= consumed; else S.local_window = 0;
    if (S.local_window < LOCAL_WINDOW / 2) {
        uint8_t pl[16];
        wr_t w = wr_new(pl, sizeof pl);
        wr_u8(&w, MSG_CHANNEL_WINDOW_ADJUST);
        wr_u32(&w, S.remote_chan);
        wr_u32(&w, LOCAL_WINDOW - S.local_window);
        send_packet(pl, w.n);
        S.local_window = LOCAL_WINDOW;
    }
}

static void channel_close(void) {
    if (S.sent_close) return;
    uint8_t pl[8];
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_CHANNEL_CLOSE);
    wr_u32(&w, S.remote_chan);
    send_packet(pl, w.n);
    S.sent_close = 1;
}

/* Один пакет из сеанса. 1 — продолжать, 0 — канал закрыт, < 0 — ошибка. */
static int session_packet(size_t n, ssh_params_t *p) {
    int h = handle_transport(n);
    if (h < 0) return h;
    if (h == 1) return 1;
    if (channel_common(n)) return 1;

    rd_t r = rd_new(S.in, n);
    uint8_t t = rd_u8(&r);
    switch (t) {
    case MSG_CHANNEL_DATA:
    case MSG_CHANNEL_EXTENDED_DATA: {
        rd_u32(&r);
        if (t == MSG_CHANNEL_EXTENDED_DATA) rd_u32(&r);
        size_t dl; const uint8_t *d = rd_str(&r, &dl);
        if (r.bad) return SSH_ERR_PROTO;
        if (S.io->put_out) S.io->put_out(S.io->ctx, d, dl);
        channel_adjust_window((uint32_t)dl);
        return 1;
    }
    case MSG_CHANNEL_EOF:
        S.got_eof = 1;
        return 1;
    case MSG_CHANNEL_CLOSE:
        S.got_close = 1;
        channel_close();
        return 0;
    case MSG_CHANNEL_REQUEST: {
        rd_u32(&r);
        size_t tl; const uint8_t *ty = rd_str(&r, &tl);
        uint8_t want = rd_u8(&r);
        if (r.bad) return SSH_ERR_PROTO;
        if (tl == 11 && mem_eq(ty, "exit-status", 11)) {
            uint32_t code = rd_u32(&r);
            if (!r.bad) p->exit_status = (int)code;
        } else if (want) {
            uint8_t pl[8];
            wr_t w = wr_new(pl, sizeof pl);
            wr_u8(&w, MSG_CHANNEL_FAILURE);
            wr_u32(&w, S.remote_chan);
            send_packet(pl, w.n);
        }
        return 1;
    }
    case MSG_CHANNEL_SUCCESS: case MSG_CHANNEL_FAILURE:
        return 1;
    default:
        return SSH_ERR_PROTO;
    }
}

/* Клавиатура → сеть. Как в OpenSSH: «~.» в начале строки закрывает сеанс. */
static int pump_keys(int *at_line_start, int *tilde, int *quit) {
    int k;
    while ((k = S.io->poll_key(S.io->ctx)) >= 0) {
        uint8_t b = (uint8_t)k;
        if (b == '\n') b = '\r';                           /* pty ждёт CR */
        if (b == '\b') b = 0x7f;                           /* DEL — стандартный «стереть» */

        if (*tilde) {
            *tilde = 0;
            if (b == '.') { *quit = 1; return SSH_OK; }
            if (b != '~') { uint8_t t = '~'; int r = channel_send_data(&t, 1); if (r) return r; }
        } else if (*at_line_start && b == '~') {
            *tilde = 1;
            continue;
        }
        *at_line_start = (b == '\r');
        int r = channel_send_data(&b, 1);
        if (r) return r;
    }
    return SSH_OK;
}

static int session_loop(ssh_params_t *p) {
    int at_line_start = 1, tilde = 0, quit = 0;
    for (;;) {
        size_t n;
        int r = recv_packet(&n, SSH_WAIT_SHORT);
        if (r < 0) return (S.got_close || S.got_eof) ? SSH_OK : r;
        if (r == 1) {
            int s = session_packet(n, p);
            if (s < 0) return s;
            if (s == 0) return SSH_OK;
            continue;                                      /* сперва выбираем всё, что уже пришло */
        }
        {
            r = pump_keys(&at_line_start, &tilde, &quit);
            if (r) return r;
            if (quit) {
                channel_close();
                /* дождаться ответного CLOSE (немного), чтобы разойтись по-хорошему */
                for (int i = 0; i < 100 && !S.got_close; i++) {
                    size_t dn;
                    int q = recv_packet(&dn, SSH_WAIT_SHORT);
                    if (q < 0) break;
                    if (q == 1) { int sp = session_packet(dn, p); if (sp <= 0) break; }
                }
                return SSH_OK;
            }
        }
    }
}

/* ---------------- вход в библиотеку ---------------- */

static void send_disconnect(void) {
    uint8_t pl[40];
    wr_t w = wr_new(pl, sizeof pl);
    wr_u8(&w, MSG_DISCONNECT);
    wr_u32(&w, 11);                                        /* BY_APPLICATION */
    wr_cstr(&w, "bye");
    wr_cstr(&w, "");
    send_packet(pl, w.n);
}

int ssh_run(const ssh_io_t *io, ssh_params_t *p) {
    wipe(&S, sizeof S);
    S.io = io;
    p->exit_status = -1;

    int r = rng_seed();
    if (r) return r;

    say("ssh: exchanging versions");
    r = exchange_versions();
    if (r) goto out;

    say("ssh: key exchange");
    r = key_exchange();
    if (r) goto out;

    say("ssh: authenticating");
    r = authenticate(p);
    if (r) goto out;

    r = open_session(p);
    if (r) goto out;

    int interactive = !p->command;
    if (interactive && io->on_session) io->on_session(io->ctx, 1);
    r = session_loop(p);
    if (interactive && io->on_session) io->on_session(io->ctx, 0);
    if (r == SSH_OK) send_disconnect();

out:
    wipe(S.rng_key, 32);
    wipe(&S.cc_in, sizeof S.cc_in);
    wipe(&S.cc_out, sizeof S.cc_out);
    return r;
}

const char *ssh_strerror(int code) {
    switch (code) {
    case SSH_OK:               return "ok";
    case SSH_ERR_IO:           return "connection lost";
    case SSH_ERR_PROTO:        return "protocol error";
    case SSH_ERR_ALGO:         return "no common algorithms";
    case SSH_ERR_HOSTKEY:      return "host key signature is invalid";
    case SSH_ERR_HOSTKEY_REJ:  return "host key rejected";
    case SSH_ERR_AUTH:         return "authentication failed";
    case SSH_ERR_CHANNEL:      return "session refused";
    case SSH_ERR_TIMEOUT:      return "timeout";
    case SSH_ERR_ENTROPY:      return "no entropy source";
    case SSH_ERR_ABORT:        return "interrupted";
    case SSH_ERR_TOOBIG:       return "packet too big";
    case SSH_ERR_DISCONNECT:   return "server closed the connection";
    case SSH_ERR_REKEY:        return "server requested rekey (not supported)";
    default:                   return "error";
    }
}
