/* net/ssh/sha2.h — SHA-256 и SHA-512 (FIPS 180-4). Без malloc, без libc. */
#ifndef SSH_SHA2_H
#define SSH_SHA2_H

#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t h[8]; uint8_t buf[64];  uint64_t len; size_t fill; } sha256_t;
typedef struct { uint64_t h[8]; uint8_t buf[128]; uint64_t len; size_t fill; } sha512_t;

void sha256_init(sha256_t *c);
void sha256_update(sha256_t *c, const void *data, size_t n);
void sha256_final(sha256_t *c, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);

void sha512_init(sha512_t *c);
void sha512_update(sha512_t *c, const void *data, size_t n);
void sha512_final(sha512_t *c, uint8_t out[64]);
void sha512(const void *data, size_t n, uint8_t out[64]);

#endif
