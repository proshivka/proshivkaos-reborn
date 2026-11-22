/* fs/ramfs_persist.c — сохранение RamFS на блочное устройство.
 *
 * ЗАЧЕМ. RamFS целиком лежит в ОЗУ и исчезает при перезагрузке. Этот слой
 * делает её копию на диске и поднимает при старте. Сам RamFS остаётся
 * кэшем в памяти, каждая запись сквозная (write-through): ramfs_write,
 * touch и mkdir сразу пишут изменившуюся запись на диск. Вызывающий код
 * (shell, терминал, файловый менеджер, редактор) об этом не знает.
 *
 * Это ПЕРЕХОДНЫЙ формат: лимиты те же, что у RamFS (48 записей, 4 КиБ на
 * файл). Настоящая файловая система придёт позже и заменит его; интерфейс
 * hal_blk.h при этом не изменится.
 *
 * РАСКЛАДКА ДИСКА (сектора по 512 байт):
 *
 *   сектор 0                     суперблок (магия, геометрия, CRC)
 *   сектора 1 ...                48 слотов по 18 секторов; слот i — запись
 *                                RamFS с индексом i; в слоте ДВЕ копии
 *                                по 9 секторов:
 *                                  [заголовок][данные 8 секторов]
 *
 * ПОЧЕМУ ДВЕ КОПИИ. Питание может пропасть посреди записи. Если писать
 * поверх единственной копии, оборванная запись уничтожит и старую версию
 * файла, и новую. Поэтому запись всегда идёт в ДРУГУЮ копию слота, с
 * номером версии (seq) на единицу больше; при старте берётся копия с
 * самым большим seq из тех, у которых сошлась CRC. Оборвалось на середине —
 * CRC новой не сойдётся, и мы откатимся к предыдущей, целой.
 *
 * ПОРЯДОК ЗАПИСИ: данные -> flush -> заголовок (с CRC данных) -> flush.
 * Заголовок — это "коммит": пока он не лёг, копия невидима.
 *
 * ЧУЖИЕ ДИСКИ НЕ ТРОГАЕМ. Диск форматируется, только если его нулевой
 * сектор целиком нулевой (чистый образ). Если там что-то другое — это
 * чужая файловая система или наш формат другой геометрии, и система
 * остаётся в ОЗУ, ничего на диск не записывая. Принудительно очистить
 * образ под QEMU: make disk-reset.
 */
#include "ramfs.h"
#include "hal_blk.h"

#define PS_SB_MAGIC     0x50534642u   /* "BFSP" в little-endian */
#define PS_SLOT_MAGIC   0x544C5350u   /* "PSLT" */
#define PS_VERSION      1u

#define DATA_SECTORS    (RAMFS_MAX_FILE_SIZE / HAL_BLK_SECTOR)
#define COPY_SECTORS    (1u + DATA_SECTORS)
#define SLOT_SECTORS    (2u * COPY_SECTORS)
#define TOTAL_SECTORS   (1u + (uint64_t)RAMFS_MAX_ENTRIES * SLOT_SECTORS)

#define F_USED          1u
#define F_DIR           2u

/* Смещения в секторе-заголовке слота */
#define H_MAGIC   0
#define H_SEQ     4
#define H_FLAGS   8
#define H_SIZE    12
#define H_CRC     16
#define H_PATH    32
#define H_LEN     (H_PATH + RAMFS_MAX_PATH_LEN)   /* CRC считается по [0, H_LEN) + данные */

_Static_assert(H_LEN <= HAL_BLK_SECTOR, "заголовок слота не помещается в сектор");
_Static_assert(RAMFS_MAX_FILE_SIZE % HAL_BLK_SECTOR == 0, "размер файла должен быть кратен сектору");

static uint8_t  g_sec[HAL_BLK_SECTOR];
static uint32_t g_seq[RAMFS_MAX_ENTRIES];   /* seq актуальной копии слота      */
static uint8_t  g_cur[RAMFS_MAX_ENTRIES];   /* какая копия (0/1) актуальна     */
static int      g_active = 0;               /* пишем ли мы на диск             */
static int      g_state  = RAMFS_PERSIST_NONE;
static unsigned g_errors = 0;

/* ---------------- Мелочи без libc ---------------- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC-32 (IEEE, как в zip/ethernet), побитово: данных мало, таблица
 * не нужна. state передаётся между вызовами, начало — 0xFFFFFFFF. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc;
}

static uint64_t slot_lba(size_t idx, unsigned copy) {
    return 1u + (uint64_t)idx * SLOT_SECTORS + (uint64_t)copy * COPY_SECTORS;
}

static int sector_is_zero(const uint8_t *s) {
    for (size_t i = 0; i < HAL_BLK_SECTOR; i++) if (s[i]) return 0;
    return 1;
}

/* ---------------- Суперблок ---------------- */

static void sb_build(uint8_t *s) {
    for (size_t i = 0; i < HAL_BLK_SECTOR; i++) s[i] = 0;
    put32(s + 0,  PS_SB_MAGIC);
    put32(s + 4,  PS_VERSION);
    put32(s + 8,  RAMFS_MAX_ENTRIES);
    put32(s + 12, RAMFS_MAX_FILE_SIZE);
    put32(s + 16, RAMFS_MAX_PATH_LEN);
    put32(s + 20, ~crc32_update(0xFFFFFFFFu, s, 20));
}

/* 1 — суперблок наш и геометрия совпадает; 0 — не наш; -1 — наш, но
 * геометрия другая (сборка с иными лимитами) */
static int sb_check(const uint8_t *s) {
    if (get32(s) != PS_SB_MAGIC) return 0;
    if (get32(s + 20) != ~crc32_update(0xFFFFFFFFu, s, 20)) return 0;
    if (get32(s + 4)  != PS_VERSION ||
        get32(s + 8)  != RAMFS_MAX_ENTRIES ||
        get32(s + 12) != RAMFS_MAX_FILE_SIZE ||
        get32(s + 16) != RAMFS_MAX_PATH_LEN) return -1;
    return 1;
}

static int format_disk(void) {
    for (size_t i = 0; i < HAL_BLK_SECTOR; i++) g_sec[i] = 0;

    /* Сначала гасим заголовки всех копий, суперблок — последним: если
       форматирование оборвётся, диск останется "чистым" и повторится. */
    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++)
        for (unsigned c = 0; c < 2; c++)
            if (hal_blk_write(slot_lba(i, c), 1, g_sec) != 0) return -1;
    if (hal_blk_flush() != 0) return -1;

    sb_build(g_sec);
    if (hal_blk_write(0, 1, g_sec) != 0) return -1;
    if (hal_blk_flush() != 0) return -1;
    return 0;
}

/* ---------------- Загрузка ---------------- */

typedef struct {
    int      ok;       /* заголовок разобран и правдоподобен */
    uint32_t seq, flags, size, crc;
    char     path[RAMFS_MAX_PATH_LEN];
} hdr_t;

static void read_hdr(size_t idx, unsigned copy, hdr_t *h) {
    h->ok = 0;
    if (hal_blk_read(slot_lba(idx, copy), 1, g_sec) != 0) { g_errors++; return; }
    if (get32(g_sec + H_MAGIC) != PS_SLOT_MAGIC) return;

    h->seq   = get32(g_sec + H_SEQ);
    h->flags = get32(g_sec + H_FLAGS);
    h->size  = get32(g_sec + H_SIZE);
    h->crc   = get32(g_sec + H_CRC);
    if (h->size > RAMFS_MAX_FILE_SIZE) return;
    if (!(h->flags & F_USED)) return;
    if ((h->flags & F_DIR) && h->size != 0) return;

    int term = 0;
    for (size_t i = 0; i < RAMFS_MAX_PATH_LEN; i++) {
        h->path[i] = (char)g_sec[H_PATH + i];
        if (!h->path[i]) { term = 1; break; }
    }
    if (!term || h->path[0] != '/') return;

    /* CRC здесь не проверяем: она покрывает и данные, которых мы ещё не
       читали. Полная проверка — в load_copy(). */
    h->ok = 1;
}

/* Читает данные копии в e->data и сверяет CRC. 1 — копия целая. */
static int load_copy(size_t idx, unsigned copy, const hdr_t *h, ramfs_file_t *e) {
    uint32_t nsec = (h->size + HAL_BLK_SECTOR - 1) / HAL_BLK_SECTOR;
    if (nsec && hal_blk_read(slot_lba(idx, copy) + 1, nsec, e->data) != 0) {
        g_errors++;
        return 0;
    }

    /* Заголовок читаем заново: g_sec мог быть занят чем-то другим. */
    if (hal_blk_read(slot_lba(idx, copy), 1, g_sec) != 0) { g_errors++; return 0; }
    put32(g_sec + H_CRC, 0);
    uint32_t crc = crc32_update(0xFFFFFFFFu, g_sec, H_LEN);
    crc = ~crc32_update(crc, e->data, h->size);
    return crc == h->crc;
}

static void load_slot(size_t idx, ramfs_file_t *e) {
    hdr_t h[2];
    read_hdr(idx, 0, &h[0]);
    read_hdr(idx, 1, &h[1]);

    /* Какую копию пробовать первой: с большим seq (сравнение с учётом
       переполнения счётчика). */
    unsigned first = 0;
    if (h[1].ok && (!h[0].ok || (int32_t)(h[1].seq - h[0].seq) > 0)) first = 1;

    g_seq[idx] = 0;
    g_cur[idx] = 1;          /* следующая запись пойдёт в копию 0 */
    e->used = 0;

    for (unsigned t = 0; t < 2; t++) {
        unsigned c = first ^ t;
        if (!h[c].ok) continue;
        if (!load_copy(idx, c, &h[c], e)) continue;

        for (size_t i = 0; i < RAMFS_MAX_PATH_LEN; i++) e->path[i] = h[c].path[i];
        e->is_dir = (h[c].flags & F_DIR) ? 1 : 0;
        e->size   = h[c].size;
        e->used   = 1;
        g_seq[idx] = h[c].seq;
        g_cur[idx] = (uint8_t)c;
        return;
    }

    /* Целой копии нет. Если одна из них битая, но с seq больше —
       следующая запись должна его превзойти, иначе при следующей загрузке
       старая и новая окажутся с равным номером. */
    for (unsigned c = 0; c < 2; c++)
        if (h[c].ok && (int32_t)(h[c].seq - g_seq[idx]) > 0) g_seq[idx] = h[c].seq;
}

/* ---------------- Публичный интерфейс ---------------- */

int ramfs_persist_attach(ramfs_file_t *tbl, size_t n) {
    g_active = 0;
    g_state  = RAMFS_PERSIST_NONE;
    if (n != RAMFS_MAX_ENTRIES) return 0;

    if (!hal_blk_init()) return 0;

    if (hal_blk_sectors() < TOTAL_SECTORS) { g_state = RAMFS_PERSIST_TOO_SMALL; return 0; }

    if (hal_blk_read(0, 1, g_sec) != 0) { g_state = RAMFS_PERSIST_ERROR; return 0; }

    int sb = sb_check(g_sec);
    if (sb < 0) { g_state = RAMFS_PERSIST_BAD_GEOMETRY; return 0; }

    if (sb == 0) {
        if (!sector_is_zero(g_sec)) { g_state = RAMFS_PERSIST_FOREIGN; return 0; }
        if (format_disk() != 0)     { g_state = RAMFS_PERSIST_ERROR;   return 0; }
        for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++) { g_seq[i] = 0; g_cur[i] = 1; }
        g_active = 1;
        g_state  = RAMFS_PERSIST_FORMATTED;
        return 0;                       /* дерево пусто — заводим по умолчанию */
    }

    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++) load_slot(i, &tbl[i]);
    g_active = 1;
    g_state  = RAMFS_PERSIST_MOUNTED;
    return 1;
}

void ramfs_persist_save(const ramfs_file_t *tbl, size_t idx) {
    if (!g_active || idx >= RAMFS_MAX_ENTRIES) return;
    const ramfs_file_t *e = &tbl[idx];

    uint32_t size = e->is_dir ? 0 : (uint32_t)e->size;
    if (size > RAMFS_MAX_FILE_SIZE) size = RAMFS_MAX_FILE_SIZE;
    uint32_t nsec = (size + HAL_BLK_SECTOR - 1) / HAL_BLK_SECTOR;

    unsigned copy = (unsigned)(g_cur[idx] ^ 1u);
    uint64_t lba  = slot_lba(idx, copy);
    uint32_t seq  = g_seq[idx] + 1;

    /* 1. Данные. Пишем целыми секторами прямо из e->data: массив на
          4096 байт, хвост последнего сектора — просто мусор после size,
          в CRC он не входит. */
    if (nsec && hal_blk_write(lba + 1, nsec, e->data) != 0) { g_errors++; return; }
    if (hal_blk_flush() != 0 && nsec) { g_errors++; return; }

    /* 2. Заголовок — коммит. */
    for (size_t i = 0; i < HAL_BLK_SECTOR; i++) g_sec[i] = 0;
    put32(g_sec + H_MAGIC, PS_SLOT_MAGIC);
    put32(g_sec + H_SEQ,   seq);
    put32(g_sec + H_FLAGS, F_USED | (e->is_dir ? F_DIR : 0));
    put32(g_sec + H_SIZE,  size);
    put32(g_sec + H_CRC,   0);
    for (size_t i = 0; i < RAMFS_MAX_PATH_LEN && e->path[i]; i++)
        g_sec[H_PATH + i] = (uint8_t)e->path[i];

    uint32_t crc = crc32_update(0xFFFFFFFFu, g_sec, H_LEN);
    crc = ~crc32_update(crc, e->data, size);
    put32(g_sec + H_CRC, crc);

    if (hal_blk_write(lba, 1, g_sec) != 0) { g_errors++; return; }
    if (hal_blk_flush() != 0) { g_errors++; return; }

    g_cur[idx] = (uint8_t)copy;
    g_seq[idx] = seq;
}

int ramfs_persist_state(void)      { return g_state; }
unsigned ramfs_persist_errors(void) { return g_errors; }

const char *ramfs_persist_state_name(void) {
    switch (g_state) {
    case RAMFS_PERSIST_MOUNTED:       return "disk podklyuchen";
    case RAMFS_PERSIST_FORMATTED:     return "disk otformatirovan";
    case RAMFS_PERSIST_FOREIGN:       return "chuzhoy disk, ne trogayu";
    case RAMFS_PERSIST_BAD_GEOMETRY:  return "format drugoy geometrii";
    case RAMFS_PERSIST_TOO_SMALL:     return "disk slishkom mal";
    case RAMFS_PERSIST_ERROR:         return "oshibka diska";
    default:                          return "net diska";
    }
}
