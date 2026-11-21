/* net/ssh/chachapoly.c — ChaCha20 + Poly1305 и упаковка пакетов SSH
 * (chacha20-poly1305@openssh.com, см. PROTOCOL.chacha20poly1305 в OpenSSH). */
#include "chachapoly.h"

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---------------- ChaCha20 ---------------- */

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROL32(d, 16); \
    c += d; b ^= c; b = ROL32(b, 12); \
    a += b; d ^= a; d = ROL32(d, 8);  \
    c += d; b ^= c; b = ROL32(b, 7)

static void chacha_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + in[i]);
}

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[8], uint64_t counter,
                  const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t st[16];
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[12] = (uint32_t)counter;
    st[13] = (uint32_t)(counter >> 32);
    st[14] = le32(nonce);
    st[15] = le32(nonce + 4);

    uint8_t ks[64];
    while (len) {
        chacha_block(st, ks);
        if (++st[12] == 0) st[13]++;
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
}

/* ---------------- Poly1305 (лимбы по 26 бит, как в poly1305-donna-32) ---------------- */

void poly1305(const uint8_t key[32], const uint8_t *m, size_t bytes, uint8_t mac[16]) {
    uint32_t r0 = le32(key)          & 0x3ffffff;
    uint32_t r1 = (le32(key + 3) >> 2) & 0x3ffff03;
    uint32_t r2 = (le32(key + 6) >> 4) & 0x3ffc0ff;
    uint32_t r3 = (le32(key + 9) >> 6) & 0x3f03fff;
    uint32_t r4 = (le32(key + 12) >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    while (bytes) {
        uint8_t blk[16];
        uint32_t hibit = 1u << 24;
        size_t n = bytes < 16 ? bytes : 16;
        for (size_t i = 0; i < n; i++) blk[i] = m[i];
        if (n < 16) {                                /* последний неполный блок: добавляем байт 1 */
            blk[n] = 1;
            for (size_t i = n + 1; i < 16; i++) blk[i] = 0;
            hibit = 0;
        }
        h0 += le32(blk) & 0x3ffffff;
        h1 += (le32(blk + 3) >> 2) & 0x3ffffff;
        h2 += (le32(blk + 6) >> 4) & 0x3ffffff;
        h3 += (le32(blk + 9) >> 6) & 0x3ffffff;
        h4 += (le32(blk + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

        m += n;
        bytes -= n;
    }

    /* полная свёртка */
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    /* h - p, и выбор между h и h - p без ветвления */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    /* h mod 2^128 в четыре 32-битных слова */
    uint32_t w0 = (h0 | (h1 << 26));
    uint32_t w1 = ((h1 >> 6) | (h2 << 20));
    uint32_t w2 = ((h2 >> 12) | (h3 << 14));
    uint32_t w3 = ((h3 >> 18) | (h4 << 8));

    /* + s (вторая половина ключа) */
    uint64_t f;
    f = (uint64_t)w0 + le32(key + 16);                  put_le32(mac,      (uint32_t)f);
    f = (uint64_t)w1 + le32(key + 20) + (f >> 32);      put_le32(mac + 4,  (uint32_t)f);
    f = (uint64_t)w2 + le32(key + 24) + (f >> 32);      put_le32(mac + 8,  (uint32_t)f);
    f = (uint64_t)w3 + le32(key + 28) + (f >> 32);      put_le32(mac + 12, (uint32_t)f);
}

/* ---------------- упаковка пакетов SSH ---------------- */

void ssh_chachapoly_init(ssh_chachapoly_t *c, const uint8_t key64[64]) {
    for (int i = 0; i < 32; i++) { c->k_main[i] = key64[i]; c->k_hdr[i] = key64[32 + i]; }
}

static void seq_nonce(uint32_t seq, uint8_t n[8]) {
    n[0] = n[1] = n[2] = n[3] = 0;
    n[4] = (uint8_t)(seq >> 24); n[5] = (uint8_t)(seq >> 16);
    n[6] = (uint8_t)(seq >> 8);  n[7] = (uint8_t)seq;
}

uint32_t ssh_chachapoly_peek_len(const ssh_chachapoly_t *c, uint32_t seq, const uint8_t enc_len[4]) {
    uint8_t n[8], b[4];
    seq_nonce(seq, n);
    chacha20_xor(c->k_hdr, n, 0, enc_len, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static void poly_key(const ssh_chachapoly_t *c, const uint8_t n[8], uint8_t pk[32]) {
    static const uint8_t zeros[32] = {0};
    chacha20_xor(c->k_main, n, 0, zeros, pk, 32);
}

void ssh_chachapoly_seal(const ssh_chachapoly_t *c, uint32_t seq, uint8_t *buf, size_t plen) {
    uint8_t n[8], pk[32];
    seq_nonce(seq, n);
    poly_key(c, n, pk);
    chacha20_xor(c->k_hdr, n, 0, buf, buf, 4);
    chacha20_xor(c->k_main, n, 1, buf + 4, buf + 4, plen);
    poly1305(pk, buf, 4 + plen, buf + 4 + plen);
}

int ssh_chachapoly_open(const ssh_chachapoly_t *c, uint32_t seq, uint8_t *buf, size_t plen) {
    uint8_t n[8], pk[32], tag[16];
    seq_nonce(seq, n);
    poly_key(c, n, pk);
    poly1305(pk, buf, 4 + plen, tag);

    uint32_t diff = 0;                                /* сравнение за постоянное время */
    for (int i = 0; i < 16; i++) diff |= (uint32_t)(tag[i] ^ buf[4 + plen + i]);
    if (diff) return -1;

    chacha20_xor(c->k_main, n, 1, buf + 4, buf + 4, plen);
    return 0;
}
