/* net/entropy.h — источник случайности платформы. Заполняет buf; 0 — успех.
 * На x86 — arch/x86/entropy.c. На платформах без источника определите
 * hal_entropy так, чтобы возвращал -1: SSH тогда честно откажется работать
 * ("no entropy source"), а не будет использовать предсказуемые числа. */
#ifndef NET_ENTROPY_H
#define NET_ENTROPY_H

#include <stdint.h>
#include <stddef.h>

int hal_entropy(uint8_t *buf, size_t n);

#endif
