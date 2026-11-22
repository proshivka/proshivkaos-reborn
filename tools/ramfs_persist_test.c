/* tools/ramfs_persist_test.c — проверка fs/ramfs_persist.c на компьютере.
 *
 * Диск здесь поддельный (массив в памяти) с возможностью "отключить питание"
 * после N записанных секторов. Проверяется: сохранение и загрузка, чередование
 * копий, обрыв питания в любой момент, порча данных, чужой диск, отсутствие
 * диска. Запуск: make test-fs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramfs.h"
#include "hal_blk.h"

/* ---- поддельный диск в памяти ---- */
#define DISK_SECTORS 2048
static uint8_t disk[DISK_SECTORS * 512];
static int have_disk = 1;
static long writes_left = -1;   /* -1: без ограничений; иначе после N записей "питание пропало" */
static long total_writes = 0;

int hal_blk_init(void) { return have_disk; }
uint64_t hal_blk_sectors(void) { return have_disk ? DISK_SECTORS : 0; }
int hal_blk_read(uint64_t lba, uint32_t n, void *b) {
    if (!have_disk || lba + n > DISK_SECTORS) return -1;
    memcpy(b, disk + lba * 512, n * 512); return 0;
}
int hal_blk_write(uint64_t lba, uint32_t n, const void *b) {
    if (!have_disk || lba + n > DISK_SECTORS) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (writes_left == 0) return -1;            /* питание пропало */
        if (writes_left > 0) writes_left--;
        memcpy(disk + (lba + i) * 512, (const uint8_t *)b + i * 512, 512);
        total_writes++;
    }
    return 0;
}
int hal_blk_flush(void) { return writes_left == 0 ? -1 : 0; }

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int has_content(const char *path, const char *want) {
    const ramfs_file_t *f = ramfs_lookup(path);
    return f && !f->is_dir && f->size == strlen(want) && memcmp(f->data, want, f->size) == 0;
}
/* welcome.txt в исходном ramfs.c пишется длиной 34 — это строка из 33 символов + NUL */
static int welcome_ok(void) {
    const ramfs_file_t *f = ramfs_lookup("/welcome.txt");
    return f && f->size == 34 && memcmp(f->data, "proshivkaOS NEXT RamFS rabotaet.\n", 33) == 0;
}

int main(void) {
    /* 1. Чистый диск: форматируется, дерево по умолчанию появляется */
    memset(disk, 0, sizeof disk);
    ramfs_init();
    CHECK(ramfs_persist_state() == RAMFS_PERSIST_FORMATTED);
    CHECK(welcome_ok());

    ramfs_mkdir("/root");
    ramfs_touch("/root/a.txt");
    ramfs_write("/root/a.txt", "hello persist", 13);
    ramfs_touch("/root/big.bin");
    char big[4096]; for (int i = 0; i < 4096; i++) big[i] = (char)(i * 7);
    ramfs_write("/root/big.bin", big, 4096);

    /* 2. "Перезагрузка": состояние в ОЗУ стирается, поднимаем с диска */
    ramfs_init();
    CHECK(ramfs_persist_state() == RAMFS_PERSIST_MOUNTED);
    CHECK(ramfs_is_dir("/root"));
    CHECK(has_content("/root/a.txt", "hello persist"));
    CHECK(ramfs_lookup("/root/big.bin") && ramfs_lookup("/root/big.bin")->size == 4096 &&
          memcmp(ramfs_lookup("/root/big.bin")->data, big, 4096) == 0);
    CHECK(welcome_ok());
    const ramfs_file_t *f = ramfs_lookup("/root/a.txt");
    CHECK(f && strcmp(f->name, "a.txt") == 0);
    const ramfs_file_t *lst[48];
    CHECK(ramfs_list_dir("/root", lst, 48) == 2);

    /* 3. Перезапись файла несколько раз (чередование копий A/B) */
    for (int i = 0; i < 5; i++) {
        char buf[32]; int n = snprintf(buf, sizeof buf, "version %d", i);
        ramfs_write("/root/a.txt", buf, (size_t)n);
        ramfs_init();
        char want[32]; snprintf(want, sizeof want, "version %d", i);
        CHECK(has_content("/root/a.txt", want));
    }

    /* 4. Обрыв питания в разные моменты записи: после загрузки файл —
          либо старая версия, либо новая, но никогда не мусор и не потеря */
    for (long cut = 0; cut < 6; cut++) {
        ramfs_write("/root/a.txt", "OLD-VERSION", 11);
        ramfs_init();
        CHECK(has_content("/root/a.txt", "OLD-VERSION"));

        writes_left = cut;                       /* питание пропадёт через cut секторов */
        ramfs_write("/root/a.txt", "NEW-VERSION-LONGER", 18);
        writes_left = -1;
        ramfs_init();                            /* перезагрузка */
        int old_ok = has_content("/root/a.txt", "OLD-VERSION");
        int new_ok = has_content("/root/a.txt", "NEW-VERSION-LONGER");
        if (!(old_ok || new_ok)) printf("cut=%ld: ни старой, ни новой версии!\n", cut);
        CHECK(old_ok || new_ok);
        /* после обрыва система должна уметь писать дальше */
        ramfs_write("/root/a.txt", "AFTER", 5);
        ramfs_init();
        CHECK(has_content("/root/a.txt", "AFTER"));
    }

    /* 5. Порча данных на диске: бит-флип в секторе данных актуальной копии
          -> откат к предыдущей версии, а не мусор */
    ramfs_write("/root/a.txt", "GOOD-1", 6);
    ramfs_write("/root/a.txt", "GOOD-2", 6);
    {
        /* найдём слот и испортим актуальную копию */
        for (int idx = 0; idx < 48; idx++) {
            /* слот a.txt: ищем по пути в заголовках */
            for (int c = 0; c < 2; c++) {
                uint8_t *h = disk + (1 + (size_t)idx * 18 + (size_t)c * 9) * 512;
                if (!strcmp((char *)h + 32, "/root/a.txt")) {
                    uint32_t seq = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
                    /* актуальная — та, у которой seq больше; портим по очереди */
                    (void)seq;
                }
            }
        }
        /* проще: портим обе копии по очереди и смотрим, что остаётся */
        int slot = -1;
        for (int idx = 0; idx < 48 && slot < 0; idx++)
            for (int c = 0; c < 2; c++)
                if (!strcmp((char *)disk + (1 + (size_t)idx * 18 + (size_t)c * 9) * 512 + 32, "/root/a.txt")) slot = idx;
        CHECK(slot >= 0);
        uint32_t s0 = *(uint32_t *)(disk + (1 + (size_t)slot * 18) * 512 + 4);
        uint32_t s1 = *(uint32_t *)(disk + (1 + (size_t)slot * 18 + 9) * 512 + 4);
        int newer = ((int32_t)(s1 - s0) > 0) ? 1 : 0;
        disk[(1 + (size_t)slot * 18 + (size_t)newer * 9 + 1) * 512] ^= 0xFF;   /* порча данных новой */
        ramfs_init();
        CHECK(has_content("/root/a.txt", "GOOD-1"));
    }

    /* 6. Чужой диск: нулевой сектор не нулевой и не наш -> ничего не пишем */
    memset(disk, 0, sizeof disk);
    memcpy(disk, "NOT-OUR-FS\x01\x02", 12);
    uint8_t snapshot[sizeof disk]; memcpy(snapshot, disk, sizeof disk);
    long w0 = total_writes;
    ramfs_init();
    CHECK(ramfs_persist_state() == RAMFS_PERSIST_FOREIGN);
    ramfs_touch("/x"); ramfs_write("/x", "y", 1);
    CHECK(total_writes == w0);
    CHECK(memcmp(snapshot, disk, sizeof disk) == 0);
    CHECK(has_content("/x", "y"));           /* но в ОЗУ работает */

    /* 7. Нет диска: поведение как раньше */
    have_disk = 0;
    ramfs_init();
    CHECK(ramfs_persist_state() == RAMFS_PERSIST_NONE);
    CHECK(welcome_ok());
    have_disk = 1;

    /* 8. Мусорные слоты не воскрешают файлы после повторного форматирования */
    memset(disk, 0, sizeof disk);
    unsigned err0 = ramfs_persist_errors();
    ramfs_init();
    ramfs_touch("/zzz"); ramfs_write("/zzz", "q", 1);
    memset(disk, 0, 512);                    /* "стёрли" суперблок */
    ramfs_init();                            /* форматируется заново */
    CHECK(!ramfs_lookup("/zzz"));
    CHECK(ramfs_persist_errors() == err0);

    printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
    return fails != 0;
}
