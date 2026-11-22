/* arch/x86/ata_pio.c — диск IDE/ATA в режиме PIO (первичный канал).
 *
 * Реализует интерфейс hal/hal_blk.h для x86. Это самый старый и самый
 * простой способ говорить с диском: пять-восемь портов ввода-вывода по
 * адресам 0x1F0..0x1F7, данные идут через порт 0x1F0 по 16 бит за раз
 * (поэтому PIO — "программный ввод-вывод": процессор сам переносит каждое
 * слово). Ни DMA, ни прерываний, ни работы с кэшем — как раз то, чего у
 * системы пока нет. Скорость скромная, но для диска, на котором лежит
 * RamFS, её с запасом хватает.
 *
 * QEMU отдаёт такой диск по `-drive file=disk.img,format=raw,if=ide`
 * (первый IDE-диск становится master первичного канала). На современном
 * железе IDE-режим в BIOS называется "legacy/compatibility"; в режиме AHCI
 * этот драйвер диска не увидит — тогда hal_blk_init() вернёт 0, и система
 * останется в ОЗУ, как раньше.
 *
 * Адресация — LBA28: до 2^28 секторов (128 ГиБ). Больше нам не нужно.
 *
 * ПРОТОКОЛ, КОРОТКО.
 *   1. Выбрать диск и старшие 4 бита LBA (порт 0x1F6), выдержать паузу
 *      в 400 нс — столько стандарт даёт диску на смену состояния; читаем
 *      порт альтернативного статуса четыре раза, каждое чтение ~100 нс.
 *   2. Записать число секторов и младшие 24 бита LBA, затем команду.
 *   3. Для каждого сектора: ждать, пока BSY сбросится и поднимется DRQ,
 *      после чего перенести 256 слов через порт данных.
 *
 * ТАЙМАУТЫ. Ждём опросом с ограничением по числу чтений порта. Диск,
 * который не ответил, не портит нашу память (данные идут через порты, а не
 * через DMA), поэтому в отличие от virtio-blk здесь сбой касается только
 * текущей операции, а не всего диска.
 */
#include "hal_blk.h"
#include "ports.h"

#define ATA_IO          0x1F0
#define ATA_CTL         0x3F6

#define R_DATA          (ATA_IO + 0)
#define R_COUNT         (ATA_IO + 2)
#define R_LBA0          (ATA_IO + 3)
#define R_LBA1          (ATA_IO + 4)
#define R_LBA2          (ATA_IO + 5)
#define R_DRIVE         (ATA_IO + 6)
#define R_STATUS        (ATA_IO + 7)     /* чтение: статус, запись: команда */
#define R_CMD           (ATA_IO + 7)

#define ST_ERR          0x01
#define ST_DRQ          0x08
#define ST_DF           0x20
#define ST_BSY          0x80

#define CMD_READ        0x20
#define CMD_WRITE       0x30
#define CMD_FLUSH       0xE7
#define CMD_IDENTIFY    0xEC

#define POLL_LIMIT      2000000u         /* чтений порта до объявления таймаута */
#define MAX_CHUNK       128u             /* секторов в одной команде            */
#define LBA28_LIMIT     (1ull << 28)

static int      g_state = 0;             /* 0 — не искали, 1 — есть, 2 — нет */
static int      g_slave = 0;
static uint64_t g_sectors = 0;

/* 400 нс паузы: четыре чтения порта альтернативного статуса. */
static void ata_delay(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_CTL);
}

/* Ждать, пока BSY сбросится. Возвращает статус или -1 по таймауту. */
static int wait_not_busy(void) {
    for (uint32_t n = 0; n < POLL_LIMIT; n++) {
        uint8_t s = inb(R_STATUS);
        if (!(s & ST_BSY)) return s;
    }
    return -1;
}

/* Ждать готовности к передаче данных. 0 — можно, -1 — ошибка или таймаут. */
static int wait_drq(void) {
    for (uint32_t n = 0; n < POLL_LIMIT; n++) {
        uint8_t s = inb(R_STATUS);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -1;
        if (s & ST_DRQ) return 0;
    }
    return -1;
}

static void select_drive(int slave, uint32_t lba_top4) {
    outb(R_DRIVE, (uint8_t)(0xE0 | (slave ? 0x10 : 0x00) | (lba_top4 & 0x0F)));
    ata_delay();
}

/* IDENTIFY одного диска. 1 — это ATA-диск с LBA, ёмкость записана в *sectors. */
static int identify(int slave, uint64_t *sectors) {
    outb(R_DRIVE, (uint8_t)(0xA0 | (slave ? 0x10 : 0x00)));
    ata_delay();
    outb(R_COUNT, 0); outb(R_LBA0, 0); outb(R_LBA1, 0); outb(R_LBA2, 0);
    outb(R_CMD, CMD_IDENTIFY);
    ata_delay();

    if (inb(R_STATUS) == 0) return 0;            /* диска нет */
    if (wait_not_busy() < 0) return 0;

    /* У ATAPI (CD-ROM) и SATA в эмуляции IDE после сброса в LBA1/LBA2
       лежит сигнатура; у обычного ATA-диска там нули. */
    if (inb(R_LBA1) || inb(R_LBA2)) return 0;
    if (wait_drq() != 0) return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(R_DATA);

    if (!(id[49] & (1u << 9))) return 0;         /* LBA не поддерживается */
    uint64_t n = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (n == 0) return 0;
    *sectors = n;
    return 1;
}

int hal_blk_init(void) {
    if (g_state != 0) return g_state == 1;
    g_state = 2;

    /* Пустая шина читается как 0xFF: контроллера IDE нет вовсе. */
    if (inb(R_STATUS) == 0xFF) return 0;

    outb(ATA_CTL, 0x02);                         /* nIEN: прерывания диска нам не нужны */

    for (int slave = 0; slave < 2; slave++) {
        uint64_t n;
        if (identify(slave, &n)) {
            g_slave   = slave;
            g_sectors = (n > LBA28_LIMIT) ? LBA28_LIMIT : n;
            g_state   = 1;
            return 1;
        }
    }
    return 0;
}

uint64_t hal_blk_sectors(void) {
    return (g_state == 1) ? g_sectors : 0;
}

static void issue(uint8_t cmd, uint64_t lba, uint32_t n) {
    select_drive(g_slave, (uint32_t)(lba >> 24));
    outb(R_COUNT, (uint8_t)n);                   /* 128 влезает; 0 означало бы 256 */
    outb(R_LBA0, (uint8_t)lba);
    outb(R_LBA1, (uint8_t)(lba >> 8));
    outb(R_LBA2, (uint8_t)(lba >> 16));
    outb(R_CMD, cmd);
    ata_delay();
}

static int range_ok(uint64_t lba, uint32_t count) {
    return g_state == 1 && lba < g_sectors && (uint64_t)count <= g_sectors - lba;
}

int hal_blk_read(uint64_t lba, uint32_t count, void *buf) {
    if (count == 0) return (g_state == 1) ? 0 : -1;
    if (!range_ok(lba, count)) return -1;
    uint8_t *p = (uint8_t *)buf;

    while (count) {
        uint32_t n = (count < MAX_CHUNK) ? count : MAX_CHUNK;
        issue(CMD_READ, lba, n);

        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq() != 0) return -1;
            for (int i = 0; i < 256; i++) {
                uint16_t w = inw(R_DATA);
                p[0] = (uint8_t)w;               /* побайтово: буфер может быть невыровнен */
                p[1] = (uint8_t)(w >> 8);
                p += 2;
            }
        }
        lba += n; count -= n;
    }
    return 0;
}

int hal_blk_write(uint64_t lba, uint32_t count, const void *buf) {
    if (count == 0) return (g_state == 1) ? 0 : -1;
    if (!range_ok(lba, count)) return -1;
    const uint8_t *p = (const uint8_t *)buf;

    while (count) {
        uint32_t n = (count < MAX_CHUNK) ? count : MAX_CHUNK;
        issue(CMD_WRITE, lba, n);

        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq() != 0) return -1;
            for (int i = 0; i < 256; i++) {
                outw(R_DATA, (uint16_t)(p[0] | (p[1] << 8)));
                p += 2;
            }
        }
        /* Последний сектор диск ещё записывает: дожидаемся и смотрим
           итоговый статус — ошибка записи появляется именно здесь. */
        int st = wait_not_busy();
        if (st < 0 || (st & (ST_ERR | ST_DF))) return -1;

        lba += n; count -= n;
    }
    return 0;
}

int hal_blk_flush(void) {
    if (g_state != 1) return -1;
    select_drive(g_slave, 0);
    outb(R_CMD, CMD_FLUSH);
    ata_delay();
    int st = wait_not_busy();
    return (st < 0 || (st & (ST_ERR | ST_DF))) ? -1 : 0;
}
