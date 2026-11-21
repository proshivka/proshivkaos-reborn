/* net/ssh/ed25519.h — X25519 (обмен ключами) и проверка подписи Ed25519.
 * Реализация по TweetNaCl (общественное достояние): компактно и без
 * таблиц. Не оптимизирована и не защищена от атак по времени — для
 * учебной ОС этого достаточно, для защиты чего-то настоящего — нет. */
#ifndef SSH_ED25519_H
#define SSH_ED25519_H

#include <stdint.h>
#include <stddef.h>

/* out = scalar * point (X25519). Для открытого ключа point = base point. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* 0 — подпись верна, -1 — нет. */
int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32]);

#endif
