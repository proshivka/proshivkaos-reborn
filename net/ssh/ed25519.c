/* net/ssh/ed25519.c — по мотивам TweetNaCl (D. J. Bernstein и др., общественное
 * достояние). Поле GF(2^255-19) хранится в 16 лимбах по 16 бит (int64_t).
 * Константы D, D2, X, Y, I сверены с вычислением по определению. */
#include "ed25519.h"
#include "sha2.h"

typedef int64_t i64;
typedef i64 gf[16];

static const gf gf0;
static const gf gf1 = {1};
static const gf k121665 = {0xDB41, 1};
static const gf D  = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf X  = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y  = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf I  = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

/* порядок группы L = 2^252 + 27742317777372353535851937790883648493, little-endian */
static const i64 L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

#define FOR(i, n) for (i = 0; i < (n); ++i)

/* ---------------- поле ---------------- */

static void set25519(gf r, const gf a) { int i; FOR(i, 16) r[i] = a[i]; }

static void car25519(gf o) {
    int i; i64 c;
    FOR(i, 16) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 t, c = ~(b - 1); int i;
    FOR(i, 16) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(uint8_t *o, const gf n) {
    int i, j, b; gf m, t;
    FOR(i, 16) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    FOR(j, 2) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    FOR(i, 16) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}

static int verify32(const uint8_t *x, const uint8_t *y) {
    uint32_t d = 0; int i;
    FOR(i, 32) d |= x[i] ^ y[i];
    return (1 & ((d - 1) >> 8)) - 1;                 /* 0 — равны, -1 — нет */
}

static int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a); pack25519(d, b);
    return verify32(c, d);
}

static uint8_t par25519(const gf a) { uint8_t d[32]; pack25519(d, a); return d[0] & 1; }

static void unpack25519(gf o, const uint8_t *n) {
    int i;
    FOR(i, 16) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { int i; FOR(i, 16) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { int i; FOR(i, 16) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b) {
    i64 i, j, t[31];
    FOR(i, 31) t[i] = 0;
    FOR(i, 16) FOR(j, 16) t[i + j] += a[i] * b[j];
    FOR(i, 15) t[i] += 38 * t[i + 16];
    FOR(i, 16) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i) {
    gf c; int a;
    FOR(a, 16) c[a] = i[a];
    for (a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    FOR(a, 16) o[a] = c[a];
}

static void pow2523(gf o, const gf i) {
    gf c; int a;
    FOR(a, 16) c[a] = i[a];
    for (a = 250; a >= 0; a--) { S(c, c); if (a != 1) M(c, c, i); }
    FOR(a, 16) o[a] = c[a];
}

/* ---------------- X25519 ---------------- */

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32]; i64 x[80], r, i; gf a, b, c, d, e, f;
    FOR(i, 31) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, point);
    FOR(i, 16) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r); sel25519(c, d, (int)r);
        A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d);
        S(d, e); S(f, a); M(a, c, a); M(c, b, e);
        A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
        M(a, c, k121665); A(a, a, d); M(c, c, a);
        M(a, d, f); M(d, b, x); S(b, e);
        sel25519(a, b, (int)r); sel25519(c, d, (int)r);
    }
    FOR(i, 16) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32] = {9};
    x25519(out, scalar, base);
}

/* ---------------- Ed25519: группа ---------------- */

static void edadd(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], uint8_t b) {
    int i; FOR(i, 4) sel25519(p[i], q[i], b);
}

static void edpack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void edscalarmult(gf p[4], gf q[4], const uint8_t *s) {
    int i;
    set25519(p[0], gf0); set25519(p[1], gf1); set25519(p[2], gf1); set25519(p[3], gf0);
    for (i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        edadd(q, p);
        edadd(p, p);
        cswap(p, q, b);
    }
}

static void edscalarbase(gf p[4], const uint8_t *s) {
    gf q[4];
    set25519(q[0], X); set25519(q[1], Y); set25519(q[2], gf1); M(q[3], X, Y);
    edscalarmult(p, q, s);
}

/* Распаковка точки с ОТРИЦАТЕЛЬНОЙ координатой x (так удобнее проверять
 * подпись: считаем h*(-A) + s*B и сравниваем с R). */
static int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den); S(den4, den2); M(den6, den4, den2);
    M(t, den6, num); M(t, t, den);

    pow2523(t, t);
    M(t, t, num); M(t, t, den); M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);

    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

/* ---------------- Ed25519: скаляры по модулю L ---------------- */

static void modL(uint8_t *r, i64 x[64]) {
    i64 carry, i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    FOR(j, 32) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    FOR(j, 32) x[j] -= carry * L[j];
    FOR(i, 32) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t r[64]) {
    i64 x[64]; int i;
    FOR(i, 64) x[i] = (i64)r[i];
    FOR(i, 64) r[i] = 0;
    modL(r, x);
}

/* ---------------- проверка подписи ---------------- */

int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32]) {
    uint8_t t[32], h[64];
    gf p[4], q[4];
    sha512_t hc;

    /* s должно быть меньше L, иначе подпись неканонична (её можно подделать
       добавлением L) — такие отвергаем */
    {
        int i, lt = 0, decided = 0;
        for (i = 31; i >= 0 && !decided; i--) {
            if (sig[32 + i] < L[i]) { lt = 1; decided = 1; }
            else if (sig[32 + i] > L[i]) { decided = 1; }
        }
        if (!lt) return -1;
    }

    if (unpackneg(q, pk)) return -1;

    sha512_init(&hc);
    sha512_update(&hc, sig, 32);                     /* R */
    sha512_update(&hc, pk, 32);                      /* A */
    sha512_update(&hc, msg, len);                    /* M */
    sha512_final(&hc, h);
    reduce(h);

    edscalarmult(p, q, h);
    edscalarbase(q, sig + 32);
    edadd(p, q);
    edpack(t, p);

    return verify32(sig, t);
}
