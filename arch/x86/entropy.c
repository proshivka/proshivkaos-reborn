/* arch/x86/entropy.c — источник случайности для x86 (нужен SSH).
 *
 * ВНИМАНИЕ: это не аппаратный генератор. Собирается дрожание времени:
 * разница RDTSC вокруг чтения счётчика PIT и CMOS-часов, плюс предыдущий
 * накопленный «пул». На эмуляторе дрожание скромное, поэтому берётся много
 * замеров, и всё сводится хэшем SHA-256. Для учебной ОС приемлемо; если
 * нужны настоящие секреты — подключайте RDRAND/RDSEED или внешний источник.
 */
#include <stdint.h>
#include <stddef.h>
#include "ports.h"
#include "sha2.h"
#include "entropy.h"

static uint8_t pool[32];
static uint32_t calls;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint16_t pit_counter(void) {
    outb(0x43, 0x00);                         /* защёлкнуть счётчик канала 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)((hi << 8) | lo);
}

static uint8_t cmos_seconds(void) {
    outb(0x70, 0x00);
    return inb(0x71);
}

int hal_entropy(uint8_t *buf, size_t n) {
    sha256_t h;
    sha256_init(&h);
    sha256_update(&h, pool, sizeof pool);
    sha256_update(&h, &calls, sizeof calls);
    calls++;

    /* 1024 замера: младшие биты разностей RDTSC вокруг медленных обращений к портам */
    uint8_t samples[256];
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 256; i++) {
            uint64_t t0 = rdtsc();
            uint16_t p = pit_counter();
            uint8_t s = cmos_seconds();
            uint64_t t1 = rdtsc();
            samples[i] = (uint8_t)((t1 - t0) ^ (t1 >> 3) ^ p ^ (p >> 8) ^ s);
        }
        sha256_update(&h, samples, sizeof samples);
        uint64_t t = rdtsc();
        sha256_update(&h, &t, sizeof t);
    }
    sha256_final(&h, pool);

    /* Выдача: счётчик-режим от пула. Пул после выдачи перемешивается заново. */
    size_t off = 0;
    uint32_t ctr = 0;
    while (off < n) {
        sha256_t o;
        uint8_t blk[32];
        sha256_init(&o);
        sha256_update(&o, pool, sizeof pool);
        sha256_update(&o, &ctr, sizeof ctr);
        sha256_final(&o, blk);
        size_t take = n - off < 32 ? n - off : 32;
        for (size_t i = 0; i < take; i++) buf[off + i] = blk[i];
        off += take;
        ctr++;
    }
    sha256(pool, sizeof pool, pool);          /* необратимо сдвигаем пул */
    return 0;
}
