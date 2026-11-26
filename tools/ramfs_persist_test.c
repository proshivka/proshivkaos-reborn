/* tools/ramfs_persist_test.c — проверка fs/ramfs_persist.c на компьютере.
 *
 * Диск здесь поддельный (массив в памяти) с возможностью "отключить питание"
 * после N записанных секторов. Проверяется: сохранение и загрузка, чередование
 * копий, обрыв питания в любой момент, порча данных, чужой диск, отсутствие
 * диска; большие файлы (хвост в кластерах): границы размеров, перезапись,
 * нехватка места, обрыв питания на каждом секторе, откат на старую копию,
 * диск формата v1, битая карта кластеров. Запуск: make test-fs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramfs.h"
#include "hal_blk.h"

#define CB  RAMFS_CLUSTER_BYTES
#define WIN RAMFS_MAX_FILE_SIZE

/* ---- поддельный диск в памяти ---- */
#define DISK_SECTORS 4096   /* 2 МиБ: пул = (4096-896)/128 = 25 кластеров */
static uint8_t disk[DISK_SECTORS * 512];
static int have_disk = 1;
static long writes_left = -1;   /* -1: без ограничений; иначе после N записей "питание пропало" */
static long total_writes = 0;
static uint64_t small_disk = 0;  /* если не 0 — диск "урезан" до стольких секторов */

/* Кэш записи: до flush сектора «в полёте». При потере питания каждый из них
 * независимо либо дошёл до носителя, либо пропал (устройство вправе переставлять
 * записи) — с вероятностью adv_keep_pct % «дошёл». Высокая вероятность — это
 * как раз опасный случай «заголовок и окно дошли, а часть хвоста нет»: так
 * проверяется порядок «данные -> flush -> заголовок». Генератор детерминирован. */
#define UNDO_MAX 4096
static struct { uint64_t lba; uint8_t old[512]; } undo[UNDO_MAX];
static int n_undo = 0;
static int power_lost = 0;
static unsigned adv_keep_pct = 50;
static uint32_t adv_rng = 12345u;
static long fail_write_at = 0;   /* >0: N-я по счёту запись сектора вернёт ошибку (питание при этом есть) */
static void lose_power(void) {
    if (power_lost) return;
    power_lost = 1;
    for (int i = 0; i < n_undo; i++) {
        adv_rng = adv_rng * 1664525u + 1013904223u;
        if ((adv_rng >> 16) % 100 >= adv_keep_pct)       /* эта запись не дошла */
            memcpy(disk + undo[i].lba * 512, undo[i].old, 512);
    }
    n_undo = 0;
}

int hal_blk_init(void) { return have_disk; }
uint64_t hal_blk_sectors(void) { return !have_disk ? 0 : small_disk ? small_disk : DISK_SECTORS; }
int hal_blk_read(uint64_t lba, uint32_t n, void *b) {
    if (!have_disk || lba + n > hal_blk_sectors()) return -1;
    memcpy(b, disk + lba * 512, n * 512); return 0;
}
int hal_blk_write(uint64_t lba, uint32_t n, const void *b) {
    if (!have_disk || lba + n > hal_blk_sectors()) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (fail_write_at > 0 && --fail_write_at == 0) return -1;   /* разовая ошибка записи */
        if (writes_left == 0) { lose_power(); return -1; }   /* питание пропало */
        if (writes_left > 0) writes_left--;
        if (n_undo < UNDO_MAX) {
            undo[n_undo].lba = lba + i;
            memcpy(undo[n_undo].old, disk + (lba + i) * 512, 512);
            n_undo++;
        } else n_undo = 0;                          /* журнал переполнен — считаем сброшенным */
        memcpy(disk + (lba + i) * 512, (const uint8_t *)b + i * 512, 512);
        total_writes++;
    }
    return 0;
}
int hal_blk_flush(void) {
    if (writes_left == 0) { lose_power(); return -1; }
    n_undo = 0;                                     /* всё дошло до носителя */
    return 0;
}

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


/* ---- помощники для больших файлов ---- */
static uint8_t bufA[RAMFS_MAX_BIG_SIZE + 16], bufB[RAMFS_MAX_BIG_SIZE + 16], bufC[RAMFS_MAX_BIG_SIZE + 16];

static uint8_t pb(size_t i, int seed) { return (uint8_t)(i * 31u + (i >> 8) * 7u + (i >> 16) * 13u + (unsigned)seed); }
static void fill_pat(uint8_t *b, size_t n, int seed) { for (size_t i = 0; i < n; i++) b[i] = pb(i, seed); }

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Эталонная CRC-32 — побитовая, независимая от реализации в ramfs_persist.c
 * (на ней писались диски v1): заодно проверяет, что быстрая версия совпадает. */
static uint32_t ref_crc(const uint8_t *p, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc;
}

static uint8_t *slot_hdr(int idx, int copy) { return disk + (1 + (size_t)idx * 18 + (size_t)copy * 9) * 512; }

/* Слот с данным путём; *newer — номер более свежей копии. -1, если нет. */
static int find_slot_of(const char *path, int *newer) {
    for (int idx = 0; idx < 48; idx++) {
        uint8_t *h0 = slot_hdr(idx, 0), *h1 = slot_hdr(idx, 1);
        int m0 = rd32(h0) == 0x544C5350u && !strcmp((char *)h0 + 32, path);
        int m1 = rd32(h1) == 0x544C5350u && !strcmp((char *)h1 + 32, path);
        if (!m0 && !m1) continue;
        *newer = (m1 && (!m0 || (int32_t)(rd32(h1 + 4) - rd32(h0 + 4)) > 0)) ? 1 : 0;
        return idx;
    }
    return -1;
}

/* Пересчитать CRC заголовка после ручной правки (симулирует «валидную» порчу). */
static void rehash(int idx, int copy) {
    uint8_t *h = slot_hdr(idx, copy);
    uint32_t size = rd32(h + 12), ncl = rd32(h + 20);
    uint32_t wlen = size > RAMFS_MAX_FILE_SIZE ? RAMFS_MAX_FILE_SIZE : size;
    wr32(h + 16, 0);
    uint32_t c = ref_crc(h, 128 + 4 * ncl, 0xFFFFFFFFu);
    wr32(h + 16, ~ref_crc(h + 512, wlen, c));
}

/* Весь файл == шаблон seed? Читаем неровными кусками, чтобы задеть границы секторов. */
static int has_big(const char *path, size_t len, int seed) {
    const ramfs_file_t *f = ramfs_lookup(path);
    if (!f || f->is_dir || f->size != len) return 0;
    uint8_t chunk[997];
    for (size_t off = 0; off < len; ) {
        long n = ramfs_read(path, off, chunk, sizeof chunk);
        if (n <= 0) return 0;
        for (long i = 0; i < n; i++) if (chunk[i] != pb(off + (size_t)i, seed)) return 0;
        off += (size_t)n;
    }
    return 1;
}

/* Точечные чтения вокруг границ окна, кластеров и конца файла. */
static int spot_reads(const char *path, size_t len, int seed) {
    size_t offs[] = { 0, 1, 511, 512, 4090, 4096, 4097, 4096 + 511, 4096 + 512,
                      4096 + CB - 3, 4096 + CB, 4096 + CB + 1, len > 40 ? len - 40 : 0, len - 1 };
    uint8_t b[600];
    for (size_t k = 0; k < sizeof offs / sizeof *offs; k++) {
        size_t off = offs[k];
        if (off >= len) continue;
        size_t want = sizeof b; if (want > len - off) want = len - off;
        long n = ramfs_read(path, off, b, sizeof b);
        if (n != (long)want) return 0;
        for (size_t i = 0; i < want; i++) if (b[i] != pb(off + i, seed)) return 0;
    }
    return 1;
}

/* Свободное место в пуле == всё минус то, что реально занято файлами в /big. */
static int pool_consistent(void) {
    const ramfs_file_t *lst[48];
    size_t n = ramfs_list_dir("/big", lst, 48), used = 0;
    for (size_t i = 0; i < n; i++)
        if (!lst[i]->is_dir && lst[i]->size > RAMFS_MAX_FILE_SIZE)
            used += (lst[i]->size - RAMFS_MAX_FILE_SIZE + RAMFS_CLUSTER_BYTES - 1) / RAMFS_CLUSTER_BYTES;
    uint32_t fr, tot; ramfs_space(&fr, &tot);
    return tot > 0 && fr == tot - used * RAMFS_CLUSTER_BYTES;
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
        writes_left = -1; power_lost = 0; n_undo = 0;
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


    /* ======================= БОЛЬШИЕ ФАЙЛЫ ======================= */

    /* 9. Границы размеров: пишем, перезагружаемся, читаем кусками и точечно */
    memset(disk, 0, sizeof disk);
    ramfs_init();
    ramfs_mkdir("/big");
    ramfs_touch("/big/f");
    {
        size_t sizes[] = { WIN + 1, WIN + CB - 1, WIN + CB, WIN + CB + 1,
                           WIN + 3 * CB + 17, WIN + 20 * (size_t)CB };
        for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
            ramfs_write("/big/f", "x", 1);                       /* освобождаем кластеры */
            fill_pat(bufA, sizes[k], (int)k + 1);
            int rc = ramfs_write("/big/f", (const char *)bufA, sizes[k]);
            if (rc != 0) printf("size %zu: rc=%d\n", sizes[k], rc);
            CHECK(rc == 0);
            CHECK(ramfs_lookup("/big/f")->size == sizes[k]);
            ramfs_init();                                         /* перезагрузка */
            CHECK(ramfs_persist_state() == RAMFS_PERSIST_MOUNTED);
            CHECK(has_big("/big/f", sizes[k], (int)k + 1));
            CHECK(pool_consistent());
            CHECK(spot_reads("/big/f", sizes[k], (int)k + 1));
        }
        CHECK(ramfs_read("/big/f", WIN + 20 * (size_t)CB, bufB, 10) == 0);   /* за концом */
        CHECK(ramfs_read("/big/f", 0, bufB, 0) == 0);
    }

    /* 10. Перезапись большого большим (копирование, старые кластеры освобождаются) */
    ramfs_write("/big/f", "x", 1);
    fill_pat(bufA, WIN + 3 * (size_t)CB, 11);
    CHECK(ramfs_write("/big/f", (const char *)bufA, WIN + 3 * (size_t)CB) == 0);
    fill_pat(bufA, WIN + 4 * (size_t)CB + 5, 12);
    CHECK(ramfs_write("/big/f", (const char *)bufA, WIN + 4 * (size_t)CB + 5) == 0);
    ramfs_init();
    CHECK(has_big("/big/f", WIN + 4 * (size_t)CB + 5, 12));
    CHECK(pool_consistent());
    {   /* усыхание большого в малый освобождает хвост */
        uint32_t fr0, tot; ramfs_space(&fr0, &tot);
        CHECK(ramfs_write("/big/f", "tiny", 4) == 0);
        uint32_t fr1; ramfs_space(&fr1, &tot);
        CHECK(fr1 == fr0 + 5 * CB);
        ramfs_init();
        CHECK(has_content("/big/f", "tiny"));
        CHECK(pool_consistent());
    }

    /* 11. Не хватает места: отказ, старое содержимое цело */
    fill_pat(bufA, WIN + 20 * (size_t)CB, 21);
    CHECK(ramfs_write("/big/f", (const char *)bufA, WIN + 20 * (size_t)CB) == 0);   /* занято 20 из 25 */
    ramfs_touch("/big/g");
    fill_pat(bufB, WIN + 10 * (size_t)CB, 22);
    CHECK(ramfs_write("/big/g", (const char *)bufB, WIN + 10 * (size_t)CB) == -2);
    CHECK(ramfs_lookup("/big/g")->size == 0);
    fill_pat(bufB, WIN + 21 * (size_t)CB, 23);                                      /* перезапись нужна 21, свободно 5 */
    CHECK(ramfs_write("/big/f", (const char *)bufB, WIN + 21 * (size_t)CB) == -2);
    CHECK(has_big("/big/f", WIN + 20 * (size_t)CB, 21));
    CHECK(ramfs_write("/big/f", (const char *)bufB, RAMFS_MAX_BIG_SIZE + 1) == -2);  /* больше лимита */
    ramfs_init();
    CHECK(has_big("/big/f", WIN + 20 * (size_t)CB, 21));
    CHECK(pool_consistent());
    ramfs_write("/big/f", "x", 1);                                                  /* вернули место */

    /* 12. Обрыв питания на КАЖДОМ секторе записи большого файла */
    ramfs_write("/big/g", "x", 1);
    fill_pat(bufA, WIN + 3 * (size_t)CB + 100, 31);
    CHECK(ramfs_write("/big/f", (const char *)bufA, WIN + 3 * (size_t)CB + 100) == 0);
    {
        static uint8_t snap[sizeof disk];
        memcpy(snap, disk, sizeof disk);
        fill_pat(bufB, WIN + 4 * (size_t)CB + 7, 32);
        long t0 = total_writes;
        CHECK(ramfs_write("/big/f", (const char *)bufB, WIN + 4 * (size_t)CB + 7) == 0);
        long need = total_writes - t0;
        int saw_old = 0, saw_new = 0;
        static const unsigned keeps[3] = { 50, 90, 99 };
        for (long cut = 0; cut <= need + 1; cut++)
        for (int trial = 0; trial < 3; trial++) {
            adv_keep_pct = keeps[trial];
            memcpy(disk, snap, sizeof disk);
            ramfs_init();
            writes_left = cut;
            int rc = ramfs_write("/big/f", (const char *)bufB, WIN + 4 * (size_t)CB + 7);
            writes_left = -1; power_lost = 0; n_undo = 0;
            ramfs_init();                                          /* перезагрузка после обрыва */
            int o = has_big("/big/f", WIN + 3 * (size_t)CB + 100, 31);
            int n = has_big("/big/f", WIN + 4 * (size_t)CB + 7, 32);
            if (!(o || n)) printf("cut=%ld: ни старая, ни новая версия цела!\n", cut);
            CHECK(o || n);
            CHECK(rc != 0 || n);                                   /* сказали «ok» — значит новая */
            CHECK(pool_consistent());                              /* оборванные кластеры не потеряны */
            saw_old += o; saw_new += n;
            /* система обязана писать дальше */
            fill_pat(bufC, WIN + 2 * (size_t)CB, 33);
            CHECK(ramfs_write("/big/f", (const char *)bufC, WIN + 2 * (size_t)CB) == 0);
            ramfs_init();
            CHECK(has_big("/big/f", WIN + 2 * (size_t)CB, 33));
            CHECK(pool_consistent());
        }
        CHECK(saw_old > 0 && saw_new > 0);
        adv_keep_pct = 50;
        printf("обрыв питания: %ld точек x 3 режима кэша, старая %d, новая %d\n", need + 2, saw_old, saw_new);

        /* 12б. Сбой коммита БЕЗ перезагрузки: header не лёг (cut = need-1) или
              лёг, но упал flush (cut = need). Дальше система живёт и пишет
              другой файл — он не должен занять кластеры, на которые может
              ссылаться диск. После перезагрузки f цел (старый или новый), g цела. */
        for (int mode = 0; mode < 3; mode++) {
            /* mode 0: ошибка записи заголовка (питание есть); 1: заголовок лёг,
               но упал flush (обрыв после него); 2: то же при «щедром» кэше */
            memcpy(disk, snap, sizeof disk);
            power_lost = 0; n_undo = 0;
            ramfs_init();
            long cut = need;
            if (mode == 0) fail_write_at = need;
            else { writes_left = cut; adv_keep_pct = mode == 1 ? 50 : 99; }
            int rc = ramfs_write("/big/f", (const char *)bufB, WIN + 4 * (size_t)CB + 7);
            fail_write_at = 0; writes_left = -1; power_lost = 0; n_undo = 0; adv_keep_pct = 50;
            CHECK(rc != 0);
            fill_pat(bufC, WIN + 4 * (size_t)CB + 7, 34);
            CHECK(ramfs_write("/big/g", (const char *)bufC, WIN + 4 * (size_t)CB + 7) == 0);
            ramfs_init();
            int o = has_big("/big/f", WIN + 3 * (size_t)CB + 100, 31);
            int n = has_big("/big/f", WIN + 4 * (size_t)CB + 7, 32);
            if (!(o || n)) printf("12б cut=%ld: файл f испорчен!\n", cut);
            CHECK(o || n);
            CHECK(has_big("/big/g", WIN + 4 * (size_t)CB + 7, 34));
            CHECK(pool_consistent());
        }
    }

    /* 13. Новая копия испорчена -> откат на старую. Но если её кластеры уже
          отданы другому файлу, хвост старой копии — чужие данные: такой
          откат нельзя выдавать за файл (проверяется CRC хвоста). */
    memset(disk, 0, sizeof disk);
    ramfs_init();
    ramfs_mkdir("/big"); ramfs_touch("/big/f"); ramfs_touch("/big/g");
    fill_pat(bufA, WIN + 2 * (size_t)CB, 41);
    CHECK(ramfs_write("/big/f", (const char *)bufA, WIN + 2 * (size_t)CB) == 0);   /* копия OLD */
    fill_pat(bufB, WIN + 2 * (size_t)CB, 42);
    CHECK(ramfs_write("/big/f", (const char *)bufB, WIN + 2 * (size_t)CB) == 0);   /* копия NEW, OLD-хвост свободен */
    {
        int newer; int idx = find_slot_of("/big/f", &newer);
        CHECK(idx >= 0);
        /* (а) откат при целом старом хвосте: возвращается ровно старая версия */
        uint8_t save_hdr[512]; memcpy(save_hdr, slot_hdr(idx, newer), 512);
        slot_hdr(idx, newer)[40] ^= 0x55;                          /* портим путь -> CRC новой не сойдётся */
        ramfs_init();
        CHECK(has_big("/big/f", WIN + 2 * (size_t)CB, 41));
        CHECK(pool_consistent());
        memcpy(slot_hdr(idx, newer), save_hdr, 512);               /* вернули */
        ramfs_init();
        CHECK(has_big("/big/f", WIN + 2 * (size_t)CB, 42));

        /* (б) старые кластеры отданы файлу g, потом новая копия f испорчена */
        fill_pat(bufC, WIN + 2 * (size_t)CB, 43);
        CHECK(ramfs_write("/big/g", (const char *)bufC, WIN + 2 * (size_t)CB) == 0);
        slot_hdr(idx, newer)[40] ^= 0x55;
        ramfs_init();
        CHECK(ramfs_lookup("/big/f") == NULL);                     /* не выдаём чужие данные за f */
        CHECK(has_big("/big/g", WIN + 2 * (size_t)CB, 43));        /* а g цела */
    }

    /* 14. Диск v1 (собран вручную): монтируется, апгрейдится до v2, дальше работает */
    memset(disk, 0, sizeof disk);
    {
        uint8_t *sb = disk;
        wr32(sb + 0, 0x50534642u); wr32(sb + 4, 1); wr32(sb + 8, RAMFS_MAX_ENTRIES);
        wr32(sb + 12, RAMFS_MAX_FILE_SIZE); wr32(sb + 16, RAMFS_MAX_PATH_LEN);
        wr32(sb + 20, ~ref_crc(sb, 20, 0xFFFFFFFFu));
        const char *v1_data = "v1 data";
        struct { int slot; const char *path; int dir; const char *data; } ents[] = {
            { 0, "/", 1, "" }, { 1, "/old.txt", 0, v1_data } };
        for (int k = 0; k < 2; k++) {
            uint8_t *h = slot_hdr(ents[k].slot, 0);
            uint32_t size = (uint32_t)strlen(ents[k].data);
            wr32(h + 0, 0x544C5350u); wr32(h + 4, 7); wr32(h + 8, 1u | (ents[k].dir ? 2u : 0u));
            wr32(h + 12, size); strcpy((char *)h + 32, ents[k].path);
            memcpy(h + 512, ents[k].data, size);
            uint32_t c = ref_crc(h, 128, 0xFFFFFFFFu);
            wr32(h + 16, ~ref_crc(h + 512, size, c));
        }
    }
    ramfs_init();
    CHECK(ramfs_persist_state() == RAMFS_PERSIST_MOUNTED);
    CHECK(has_content("/old.txt", "v1 data"));
    CHECK(rd32(disk + 4) == 2);                                    /* суперблок стал v2 */
    fill_pat(bufA, WIN + CB + 50, 51);
    ramfs_touch("/photo");
    CHECK(ramfs_write("/photo", (const char *)bufA, WIN + CB + 50) == 0);
    ramfs_write("/old.txt", "v2 now", 6);
    ramfs_init();
    CHECK(has_big("/photo", WIN + CB + 50, 51));
    CHECK(has_content("/old.txt", "v2 now"));
    {   /* повреждённый суперблок v1 — не наш диск, не форматируем поверх */
        memset(disk, 0, sizeof disk);
        wr32(disk, 0x50534642u); wr32(disk + 4, 1); wr32(disk + 20, 0xDEADBEEFu);
        ramfs_init();
        CHECK(ramfs_persist_state() == RAMFS_PERSIST_FOREIGN);
    }

    /* 15. Геометрия v2 не совпадает (другой размер кластера) -> диск не трогаем */
    memset(disk, 0, sizeof disk);
    ramfs_init();                                                  /* форматирует v2 */
    wr32(disk + 20, CB * 2);
    wr32(disk + 28, ~ref_crc(disk, 28, 0xFFFFFFFFu));
    {
        uint8_t snapshot2[sizeof disk]; memcpy(snapshot2, disk, sizeof disk);
        ramfs_init();
        CHECK(ramfs_persist_state() == RAMFS_PERSIST_BAD_GEOMETRY);
        CHECK(memcmp(snapshot2, disk, sizeof disk) == 0);
    }

    /* 16. Два файла претендуют на один кластер -> второй убираем из дерева */
    memset(disk, 0, sizeof disk);
    ramfs_init();
    ramfs_mkdir("/big"); ramfs_touch("/big/f1"); ramfs_touch("/big/f2");
    fill_pat(bufA, WIN + 2 * (size_t)CB, 61); fill_pat(bufB, WIN + 2 * (size_t)CB, 62);
    CHECK(ramfs_write("/big/f1", (const char *)bufA, WIN + 2 * (size_t)CB) == 0);
    CHECK(ramfs_write("/big/f2", (const char *)bufB, WIN + 2 * (size_t)CB) == 0);
    {
        int n1, n2; int i1 = find_slot_of("/big/f1", &n1), i2 = find_slot_of("/big/f2", &n2);
        CHECK(i1 >= 0 && i2 >= 0);
        uint8_t *h1 = slot_hdr(i1, n1), *h2 = slot_hdr(i2, n2);
        uint32_t keep_map1 = rd32(h2 + 128);
        wr32(h2 + 128, rd32(h1 + 128));                            /* f2 «забирает» кластер f1 */
        rehash(i2, n2);
        unsigned e0 = ramfs_persist_errors();
        ramfs_init();
        CHECK(has_big("/big/f1", WIN + 2 * (size_t)CB, 61));
        CHECK(ramfs_lookup("/big/f2") == NULL);
        CHECK(ramfs_persist_errors() == e0 + 1);
        (void)keep_map1;
    }

    /* 17. Карта указывает за пределы пула -> копия отбрасывается, файл откатывается */
    memset(disk, 0, sizeof disk);
    ramfs_init();
    ramfs_mkdir("/big"); ramfs_touch("/big/f");
    fill_pat(bufA, WIN + CB, 71);
    CHECK(ramfs_write("/big/f", (const char *)bufA, WIN + CB) == 0);
    {
        int nw; int i = find_slot_of("/big/f", &nw);
        uint8_t *h = slot_hdr(i, nw);
        wr32(h + 128, 100000u);                                    /* кластера с таким номером нет */
        rehash(i, nw);
        ramfs_init();
        /* битая новая копия -> откат на прежнюю версию (пустой файл из touch) */
        const ramfs_file_t *f = ramfs_lookup("/big/f");
        CHECK(f && f->size == 0);
        CHECK(pool_consistent());
    }

    /* 18. Нет диска: большой файл не пишется и не обрезается молча */
    have_disk = 0;
    ramfs_init();
    ramfs_touch("/m");
    ramfs_write("/m", "small", 5);
    fill_pat(bufA, WIN + 10, 81);
    CHECK(ramfs_write("/m", (const char *)bufA, WIN + 10) == -2);
    CHECK(has_content("/m", "small"));
    {
        uint32_t fr = 1, tot = 1; ramfs_space(&fr, &tot);
        CHECK(fr == 0 && tot == 0);
    }
    have_disk = 1;

    /* 19. Диск меньше пула: обычные файлы работают, большие — отказ */
    memset(disk, 0, sizeof disk);
    small_disk = 900;                                              /* слоты (865) есть, пула (с 896) — нет */
    ramfs_init();
    CHECK(ramfs_persist_state() == RAMFS_PERSIST_FORMATTED);
    ramfs_touch("/s"); ramfs_write("/s", "ok", 2);
    fill_pat(bufA, WIN + 10, 91);
    CHECK(ramfs_write("/s", (const char *)bufA, WIN + 10) == -2);
    ramfs_init();
    CHECK(has_content("/s", "ok"));
    small_disk = 0;

    printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
    return fails != 0;
}
