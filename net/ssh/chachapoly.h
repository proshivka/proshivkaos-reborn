/* net/ssh/chachapoly.h — ChaCha20, Poly1305 и шифр SSH chacha20-poly1305@openssh.com. */
#ifndef SSH_CHACHAPOLY_H
#define SSH_CHACHAPOLY_H

#include <stdint.h>
#include <stddef.h>

/* Поток ChaCha20 (оригинальный вариант Бернштейна: 64-битный счётчик и
 * 64-битный nonce — именно его использует OpenSSH). */
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[8], uint64_t counter,
                  const uint8_t *in, uint8_t *out, size_t len);

void poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);

/* Ключ пакета SSH — 64 байта: первые 32 — K_2 (данные и ключ Poly1305),
 * следующие 32 — K_1 (шифрование поля длины). */
typedef struct { uint8_t k_main[32]; uint8_t k_hdr[32]; } ssh_chachapoly_t;

void ssh_chachapoly_init(ssh_chachapoly_t *c, const uint8_t key64[64]);

/* Расшифровать 4 байта длины пакета. */
uint32_t ssh_chachapoly_peek_len(const ssh_chachapoly_t *c, uint32_t seq, const uint8_t enc_len[4]);

/* Зашифровать пакет на месте: buf = [4 байта длины][payload длиной plen],
 * после вызова за payload дописывается тег (16 байт). Всего 4+plen+16. */
void ssh_chachapoly_seal(const ssh_chachapoly_t *c, uint32_t seq, uint8_t *buf, size_t plen);

/* Проверить тег и расшифровать payload на месте. buf = [4 байта длины
 * (шифртекст)][payload (шифртекст)][тег]. 0 — успех, -1 — тег неверен. */
int ssh_chachapoly_open(const ssh_chachapoly_t *c, uint32_t seq, uint8_t *buf, size_t plen);

#endif
