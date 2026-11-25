/* fs/ramfs_persist.c — сохранение RamFS на блочное устройство.
 *
 * БОЛЬШИЕ ФАЙЛЫ (формат v2). Запись слота по-прежнему хранит первые 4 КиБ
 * файла («окно»), но в заголовке теперь есть карта кластеров: хвост файла
 * лежит в пуле 64-КиБ кластеров, который начинается сразу за слотами.
 * Запись копирует, а не перезаписывает: новый хвост всегда уходит в СВОБОДНЫЕ
 * кластеры, а заголовок новой копии слота (коммит) переключает файл разом.
 * Оборвалось до коммита — файл прежний, а занятые кластеры никем не учтены
 * (занятость считается при монтировании по актуальным копиям слотов).
 * Поэтому перезапись большого файла требует свободного места под ВЕСЬ новый
 * хвост, даже если новый размер такой же, как у старого.
 *
 * Целость: заголовок (с картой) и окно закрыты CRC. Хвост закрыт отдельной
 * CRC в заголовке, но она проверяется только когда монтирование вынуждено
 * откатиться на старую копию слота (новая не сошлась по CRC) — иначе пришлось
 * бы читать все файлы при каждой загрузке. В обычном случае целость хвоста
 * держится на порядке «хвост -> flush -> заголовок -> flush».
 *
 * Совместимость: диски v1 монтируются как есть (в v1 карта кластеров в
 * заголовке всегда нулевая, CRC считается так же) и при монтировании
 * обновляются до v2 — переписывается суперблок. Старая прошивка v1 после
 * этого видит «другую геометрию» и диск не трогает. Образ можно увеличить:
 * пул кластеров занимает всё место после слотов и считается по размеру диска.
 *
 * ЗАЧЕМ. RamFS целиком лежит в ОЗУ и исчезает при перезагрузке. Этот слой
 * делает её копию на диске и поднимает при старте. Сам RamFS остаётся
 * кэшем в памяти, каждая запись сквозная (write-through): ramfs_write,
 * touch и mkdir сразу пишут изменившуюся запись на диск. Вызывающий код
 * (shell, терминал, файловый менеджер, редактор) об этом не знает.
 *
 * Это ПЕРЕХОДНЫЙ формат: 48 записей, окно 4 КиБ + до 64 кластеров на файл.
 * Настоящая файловая система придёт позже и заменит его; интерфейс
 * hal_blk.h при этом не изменится.
 *
 * РАСКЛАДКА ДИСКА (сектора по 512 байт):
 *
 *   сектор 0                     суперблок (магия, геометрия, CRC)
 *   сектора 1 ...                48 слотов по 18 секторов; слот i — запись
 *                                RamFS с индексом i; в слоте ДВЕ копии
 *                                по 9 секторов:
 *                                  [заголовок][окно: 8 секторов]
 *   POOL_LBA ...                 (кратно кластеру) пул кластеров по 64 КиБ —
 *                                хвосты больших файлов
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
#define PS_VERSION      2u
#define PS_VERSION_V1   1u

#define CLUSTER_BYTES   RAMFS_CLUSTER_BYTES
#define CLUSTER_SECTORS (CLUSTER_BYTES / HAL_BLK_SECTOR)
#define MAX_CLUSTERS    RAMFS_MAX_CLUSTERS
#define MAX_POOL        8192u         /* кластеров в пуле: до 512 МиБ */

#define DATA_SECTORS    (RAMFS_MAX_FILE_SIZE / HAL_BLK_SECTOR)
#define COPY_SECTORS    (1u + DATA_SECTORS)
#define SLOT_SECTORS    (2u * COPY_SECTORS)
#define TOTAL_SECTORS   (1u + (uint64_t)RAMFS_MAX_ENTRIES * SLOT_SECTORS)
/* Пул начинается с границы кластера: выравнивание полезно устройствам. */
#define POOL_LBA        (((TOTAL_SECTORS + CLUSTER_SECTORS - 1u) / CLUSTER_SECTORS) * CLUSTER_SECTORS)

#define F_USED          1u
#define F_DIR           2u

/* Смещения в секторе-заголовке слота */
#define H_MAGIC   0
#define H_SEQ     4
#define H_FLAGS   8
#define H_SIZE    12
#define H_CRC     16
#define H_NCL     20      /* v2: сколько кластеров в хвосте (в v1 там нули) */
#define H_TCRC    24      /* v2: CRC хвоста (0, если хвоста нет)             */
#define H_PATH    32
#define H_LEN     (H_PATH + RAMFS_MAX_PATH_LEN)   /* фиксированная часть заголовка */
#define H_MAP     H_LEN                           /* v2: карта кластеров, u32 на кластер */

/* CRC слота: по [0, H_MAP + 4*ncl) (поле CRC обнулено) + окно. При ncl == 0
 * это ровно то, что считала v1 — старые слоты читаются без изменений. */

_Static_assert(H_MAP + MAX_CLUSTERS * 4u <= HAL_BLK_SECTOR, "карта кластеров не помещается в заголовок");
_Static_assert(CLUSTER_BYTES % HAL_BLK_SECTOR == 0, "кластер должен быть кратен сектору");
_Static_assert(RAMFS_MAX_FILE_SIZE % HAL_BLK_SECTOR == 0, "размер файла должен быть кратен сектору");
_Static_assert(RAMFS_MAX_FILE_SIZE <= 4096, "g_chunk рассчитан на окно до 4 КиБ");

static uint8_t  g_sec[HAL_BLK_SECTOR];
static uint32_t g_seq[RAMFS_MAX_ENTRIES];   /* seq актуальной копии слота      */
static uint8_t  g_cur[RAMFS_MAX_ENTRIES];   /* какая копия (0/1) актуальна     */
static int      g_active = 0;               /* пишем ли мы на диск             */
static int      g_big    = 0;               /* можно ли класть хвосты на диск  */
static uint32_t g_npool  = 0;               /* кластеров в пуле                */
static uint32_t g_nfree  = 0;               /* из них свободных                */
static uint8_t  g_bm[MAX_POOL / 8];         /* занятость кластеров (1 = занят) */
static uint32_t g_map[RAMFS_MAX_ENTRIES][MAX_CLUSTERS]; /* карта актуальной копии */
static uint8_t  g_ncl[RAMFS_MAX_ENTRIES];   /* длина этой карты                */
static uint8_t  g_chunk[4096];              /* буфер проверки хвоста           */
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

/* CRC-32 (IEEE, как в zip/ethernet) по полубайту: таблица из 16 слов, а
 * хвосты больших файлов — это мегабайты. Результат тот же, что у побитового
 * варианта (на нём писались слоты v1). state передаётся между вызовами,
 * начало — 0xFFFFFFFF. */
static const uint32_t crc_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc_nib[crc & 15u];
        crc = (crc >> 4) ^ crc_nib[crc & 15u];
    }
    return crc;
}

/* Размеры считаем в uint32: на x86 в ядре нет 64-битного деления (__udivdi3). */
static uint32_t window_len(uint32_t size) {
    return size > RAMFS_MAX_FILE_SIZE ? RAMFS_MAX_FILE_SIZE : size;
}
static uint32_t ncl_for(uint32_t size) {
    return size > RAMFS_MAX_FILE_SIZE
        ? (size - RAMFS_MAX_FILE_SIZE + CLUSTER_BYTES - 1u) / CLUSTER_BYTES : 0u;
}

/* ---------------- Пул кластеров ---------------- */

static int  bm_get(uint32_t c) { return (g_bm[c >> 3] >> (c & 7u)) & 1; }
static void bm_set(uint32_t c) { g_bm[c >> 3] |= (uint8_t)(1u << (c & 7u)); }
static void bm_clr(uint32_t c) { g_bm[c >> 3] &= (uint8_t)~(1u << (c & 7u)); }

static uint64_t cluster_lba(uint32_t c) {
    return (uint64_t)POOL_LBA + (uint64_t)c * CLUSTER_SECTORS;
}

/* Занять n свободных кластеров (первые по порядку). Вызывающий уже
 * проверил n <= g_nfree. */
static void alloc_clusters(uint32_t *out, uint32_t n) {
    uint32_t k = 0;
    for (uint32_t c = 0; c < g_npool && k < n; c++)
        if (!bm_get(c)) { bm_set(c); out[k++] = c; }
    g_nfree -= n;
}

static void release_clusters(const uint32_t *m, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) bm_clr(m[k]);
    g_nfree += n;
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
    put32(s + 20, CLUSTER_BYTES);
    put32(s + 24, MAX_CLUSTERS);
    put32(s + 28, ~crc32_update(0xFFFFFFFFu, s, 28));
}

/* 1 — суперблок v2, геометрия совпадает; 2 — суперблок v1 (старый диск,
 * геометрия совпадает — обновим до v2); 0 — не наш; -1 — наш, но геометрия
 * другая (сборка с иными лимитами) */
static int sb_check(const uint8_t *s) {
    if (get32(s) != PS_SB_MAGIC) return 0;
    uint32_t ver = get32(s + 4);

    if (ver == PS_VERSION_V1) {                 /* v1: CRC в s+20 по первым 20 байтам */
        if (get32(s + 20) != ~crc32_update(0xFFFFFFFFu, s, 20)) return 0;
        if (get32(s + 8)  != RAMFS_MAX_ENTRIES ||
            get32(s + 12) != RAMFS_MAX_FILE_SIZE ||
            get32(s + 16) != RAMFS_MAX_PATH_LEN) return -1;
        return 2;
    }

    if (get32(s + 28) != ~crc32_update(0xFFFFFFFFu, s, 28)) return 0;
    if (ver != PS_VERSION ||
        get32(s + 8)  != RAMFS_MAX_ENTRIES ||
        get32(s + 12) != RAMFS_MAX_FILE_SIZE ||
        get32(s + 16) != RAMFS_MAX_PATH_LEN ||
        get32(s + 20) != CLUSTER_BYTES ||
        get32(s + 24) != MAX_CLUSTERS) return -1;
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
    uint32_t seq, flags, size, crc, ncl, tcrc;
    char     path[RAMFS_MAX_PATH_LEN];
    uint32_t map[MAX_CLUSTERS];
} hdr_t;

static void read_hdr(size_t idx, unsigned copy, hdr_t *h) {
    h->ok = 0;
    if (hal_blk_read(slot_lba(idx, copy), 1, g_sec) != 0) { g_errors++; return; }
    if (get32(g_sec + H_MAGIC) != PS_SLOT_MAGIC) return;

    h->seq   = get32(g_sec + H_SEQ);
    h->flags = get32(g_sec + H_FLAGS);
    h->size  = get32(g_sec + H_SIZE);
    h->crc   = get32(g_sec + H_CRC);
    h->ncl   = get32(g_sec + H_NCL);
    h->tcrc  = get32(g_sec + H_TCRC);
    if (h->size > RAMFS_MAX_BIG_SIZE) return;
    if (!(h->flags & F_USED)) return;
    if ((h->flags & F_DIR) && h->size != 0) return;

    /* Карта обязана быть ровно той длины, что следует из размера: v1 даёт
       size <= окно и нули в H_NCL — это тоже проходит. */
    if (h->ncl != ncl_for(h->size)) return;
    for (uint32_t k = 0; k < h->ncl; k++) {
        h->map[k] = get32(g_sec + H_MAP + 4u * k);
        if (h->map[k] >= g_npool) return;                  /* вне пула */
        for (uint32_t j = 0; j < k; j++)
            if (h->map[j] == h->map[k]) return;            /* кластер дважды */
    }

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

/* Читает хвост по карте и сверяет его CRC. 1 — хвост целый. */
static int tail_ok(const hdr_t *h) {
    uint32_t left = h->size - RAMFS_MAX_FILE_SIZE;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < h->ncl; k++) {
        uint32_t in_cl = left < CLUSTER_BYTES ? left : CLUSTER_BYTES;
        uint64_t lba = cluster_lba(h->map[k]);
        uint32_t off = 0;
        while (off < in_cl) {
            uint32_t n = in_cl - off;
            if (n > sizeof g_chunk) n = sizeof g_chunk;
            uint32_t nsec = (n + HAL_BLK_SECTOR - 1) / HAL_BLK_SECTOR;
            if (hal_blk_read(lba + off / HAL_BLK_SECTOR, nsec, g_chunk) != 0) { g_errors++; return 0; }
            crc = crc32_update(crc, g_chunk, n);
            off += n;
        }
        left -= in_cl;
    }
    return ~crc == h->tcrc;
}

/* Читает окно копии в e->data и сверяет CRC. verify_tail — ещё и хвост
 * (когда копия выбрана в обход более новой). 1 — копия целая. */
static int load_copy(size_t idx, unsigned copy, const hdr_t *h, ramfs_file_t *e, int verify_tail) {
    uint32_t wlen = window_len(h->size);
    uint32_t nsec = (wlen + HAL_BLK_SECTOR - 1) / HAL_BLK_SECTOR;
    if (nsec && hal_blk_read(slot_lba(idx, copy) + 1, nsec, e->data) != 0) {
        g_errors++;
        return 0;
    }

    /* Заголовок читаем заново: g_sec мог быть занят чем-то другим. */
    if (hal_blk_read(slot_lba(idx, copy), 1, g_sec) != 0) { g_errors++; return 0; }
    put32(g_sec + H_CRC, 0);
    uint32_t crc = crc32_update(0xFFFFFFFFu, g_sec, H_MAP + 4u * h->ncl);
    crc = ~crc32_update(crc, e->data, wlen);
    if (crc != h->crc) return 0;

    if (verify_tail && h->ncl && !tail_ok(h)) return 0;
    return 1;
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
    g_ncl[idx] = 0;
    e->used = 0;

    for (unsigned t = 0; t < 2; t++) {
        unsigned c = first ^ t;
        if (!h[c].ok) continue;
        if (!load_copy(idx, c, &h[c], e, t == 1)) continue;

        for (size_t i = 0; i < RAMFS_MAX_PATH_LEN; i++) e->path[i] = h[c].path[i];
        e->is_dir = (h[c].flags & F_DIR) ? 1 : 0;
        e->size   = h[c].size;
        e->used   = 1;
        g_seq[idx] = h[c].seq;
        g_cur[idx] = (uint8_t)c;
        g_ncl[idx] = (uint8_t)h[c].ncl;
        for (uint32_t k = 0; k < h[c].ncl; k++) g_map[idx][k] = h[c].map[k];
        return;
    }

    /* Целой копии нет. Если одна из них битая, но с seq больше —
       следующая запись должна его превзойти, иначе при следующей загрузке
       старая и новая окажутся с равным номером. */
    for (unsigned c = 0; c < 2; c++)
        if (h[c].ok && (int32_t)(h[c].seq - g_seq[idx]) > 0) g_seq[idx] = h[c].seq;
}

/* Занятость кластеров — по актуальным копиям слотов. Кластер, на который
 * претендуют две записи, — повреждение: такую запись из дерева убираем
 * (на диске она остаётся нетронутой), чтобы не отдать чужие данные. */
static void build_bitmap(ramfs_file_t *tbl) {
    for (size_t i = 0; i < sizeof g_bm; i++) g_bm[i] = 0;

    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++) {
        if (!tbl[i].used || g_ncl[i] == 0) continue;
        int clash = 0;
        for (uint32_t k = 0; k < g_ncl[i]; k++)
            if (bm_get(g_map[i][k])) clash = 1;
        if (clash) { tbl[i].used = 0; g_ncl[i] = 0; g_errors++; continue; }
        for (uint32_t k = 0; k < g_ncl[i]; k++) bm_set(g_map[i][k]);
    }

    uint32_t used = 0;
    for (uint32_t c = 0; c < g_npool; c++) used += (uint32_t)bm_get(c);
    g_nfree = g_npool - used;
}

/* ---------------- Публичный интерфейс ---------------- */

int ramfs_persist_attach(ramfs_file_t *tbl, size_t n) {
    g_active = 0;
    g_big    = 0;
    g_npool  = 0;
    g_nfree  = 0;
    g_state  = RAMFS_PERSIST_NONE;
    for (size_t i = 0; i < sizeof g_bm; i++) g_bm[i] = 0;
    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++) g_ncl[i] = 0;
    if (n != RAMFS_MAX_ENTRIES) return 0;

    if (!hal_blk_init()) return 0;

    uint64_t secs = hal_blk_sectors();
    if (secs < TOTAL_SECTORS) { g_state = RAMFS_PERSIST_TOO_SMALL; return 0; }

    /* Размер пула: всё место за слотами, не больше MAX_POOL кластеров.
       Режем до uint32 заранее — 64-битного деления в ядре нет. */
    if (secs >= (uint64_t)POOL_LBA + CLUSTER_SECTORS) {
        uint64_t avail = secs - POOL_LBA;
        if (avail > (uint64_t)MAX_POOL * CLUSTER_SECTORS) avail = (uint64_t)MAX_POOL * CLUSTER_SECTORS;
        g_npool = (uint32_t)avail / CLUSTER_SECTORS;
    }

    if (hal_blk_read(0, 1, g_sec) != 0) { g_state = RAMFS_PERSIST_ERROR; return 0; }

    int sb = sb_check(g_sec);
    if (sb < 0) { g_state = RAMFS_PERSIST_BAD_GEOMETRY; return 0; }

    if (sb == 0) {
        if (!sector_is_zero(g_sec)) { g_state = RAMFS_PERSIST_FOREIGN; return 0; }
        if (format_disk() != 0)     { g_state = RAMFS_PERSIST_ERROR;   return 0; }
        for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++) { g_seq[i] = 0; g_cur[i] = 1; }
        g_nfree  = g_npool;
        g_big    = g_npool > 0;
        g_active = 1;
        g_state  = RAMFS_PERSIST_FORMATTED;
        return 0;                       /* дерево пусто — заводим по умолчанию */
    }

    if (sb == 2) {
        /* Диск v1: слоты читаются как есть. Суперблок переписываем на v2,
           чтобы старая прошивка не стала затирать слоты, не понимая карту.
           Не вышло — монтируем, но хвосты на диск не кладём. */
        sb_build(g_sec);
        if (hal_blk_write(0, 1, g_sec) == 0 && hal_blk_flush() == 0) sb = 1;
        else g_errors++;
    }
    g_big = (sb == 1) && g_npool > 0;

    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++) load_slot(i, &tbl[i]);
    build_bitmap(tbl);
    g_active = 1;
    g_state  = RAMFS_PERSIST_MOUNTED;
    return 1;
}

/* Коммит: записывает заголовок новой копии слота и, когда он лёг, переключает
 * слот на неё. map/ncl — карта хвоста этой копии (кластеры уже заняты и
 * записаны). Старые кластеры файла освобождаются ПОСЛЕ коммита. 0 — успех. */
static int commit(size_t idx, const ramfs_file_t *e, uint32_t size, const uint8_t *win,
                  const uint32_t *map, uint32_t ncl, uint32_t tcrc) {
    unsigned copy = (unsigned)(g_cur[idx] ^ 1u);
    uint64_t lba  = slot_lba(idx, copy);
    uint32_t seq  = g_seq[idx] + 1;

    for (size_t i = 0; i < HAL_BLK_SECTOR; i++) g_sec[i] = 0;
    put32(g_sec + H_MAGIC, PS_SLOT_MAGIC);
    put32(g_sec + H_SEQ,   seq);
    put32(g_sec + H_FLAGS, F_USED | (e->is_dir ? F_DIR : 0));
    put32(g_sec + H_SIZE,  size);
    put32(g_sec + H_CRC,   0);
    put32(g_sec + H_NCL,   ncl);
    put32(g_sec + H_TCRC,  tcrc);
    for (size_t i = 0; i < RAMFS_MAX_PATH_LEN && e->path[i]; i++)
        g_sec[H_PATH + i] = (uint8_t)e->path[i];
    for (uint32_t k = 0; k < ncl; k++) put32(g_sec + H_MAP + 4u * k, map[k]);

    uint32_t crc = crc32_update(0xFFFFFFFFu, g_sec, H_MAP + 4u * ncl);
    crc = ~crc32_update(crc, win, window_len(size));
    put32(g_sec + H_CRC, crc);

    if (hal_blk_write(lba, 1, g_sec) != 0) { g_errors++; return -1; }
    if (hal_blk_flush() != 0) { g_errors++; return -1; }

    /* Коммит прошёл: старые кластеры свободны, карта и номер копии новые. */
    release_clusters(g_map[idx], g_ncl[idx]);
    for (uint32_t k = 0; k < ncl; k++) g_map[idx][k] = map[k];
    g_ncl[idx] = (uint8_t)ncl;
    g_cur[idx] = (uint8_t)copy;
    g_seq[idx] = seq;
    return 0;
}

void ramfs_persist_save(const ramfs_file_t *tbl, size_t idx) {
    if (!g_active || idx >= RAMFS_MAX_ENTRIES) return;
    const ramfs_file_t *e = &tbl[idx];

    uint32_t size = e->is_dir ? 0 : (uint32_t)e->size;
    if (size > RAMFS_MAX_FILE_SIZE) return;     /* большой файл — только через write_big */
    uint32_t nsec = (size + HAL_BLK_SECTOR - 1) / HAL_BLK_SECTOR;

    /* 1. Окно. Пишем целыми секторами прямо из e->data: массив на
          4096 байт, хвост последнего сектора — просто мусор после size,
          в CRC он не входит. */
    unsigned copy = (unsigned)(g_cur[idx] ^ 1u);
    if (nsec && hal_blk_write(slot_lba(idx, copy) + 1, nsec, e->data) != 0) { g_errors++; return; }
    if (hal_blk_flush() != 0 && nsec) { g_errors++; return; }

    /* 2. Заголовок — коммит (заодно освобождает хвост, если файл усох). */
    commit(idx, e, size, e->data, NULL, 0, 0);
}

int ramfs_persist_big_ok(void) { return g_active && g_big; }

/* Пишет хвост по кластерам (последний кластер добивается нулями до сектора). */
static int write_tail(const uint32_t *map, uint32_t ncl, const uint8_t *p, uint32_t len) {
    uint32_t left = len;
    for (uint32_t k = 0; k < ncl; k++) {
        uint32_t in_cl = left < CLUSTER_BYTES ? left : CLUSTER_BYTES;
        uint32_t full = in_cl / HAL_BLK_SECTOR, rem = in_cl % HAL_BLK_SECTOR;
        uint64_t lba = cluster_lba(map[k]);

        if (full && hal_blk_write(lba, full, p) != 0) return -1;
        if (rem) {
            for (size_t i = 0; i < HAL_BLK_SECTOR; i++) g_sec[i] = i < rem ? p[full * HAL_BLK_SECTOR + i] : 0;
            if (hal_blk_write(lba + full, 1, g_sec) != 0) return -1;
        }
        p += in_cl;
        left -= in_cl;
    }
    return 0;
}

int ramfs_persist_write_big(ramfs_file_t *tbl, size_t idx, const uint8_t *data, size_t len) {
    if (!g_active || !g_big || idx >= RAMFS_MAX_ENTRIES) return -2;
    if (len <= RAMFS_MAX_FILE_SIZE || len > RAMFS_MAX_BIG_SIZE) return -2;
    const ramfs_file_t *e = &tbl[idx];
    if (e->is_dir) return -1;

    uint32_t size = (uint32_t)len;
    uint32_t tail = size - RAMFS_MAX_FILE_SIZE;
    uint32_t ncl  = ncl_for(size);
    if (ncl > g_nfree) return -2;                       /* нет места под весь новый хвост */

    uint32_t nm[MAX_CLUSTERS];
    alloc_clusters(nm, ncl);

    uint32_t tcrc = ~crc32_update(0xFFFFFFFFu, data + RAMFS_MAX_FILE_SIZE, tail);
    unsigned copy = (unsigned)(g_cur[idx] ^ 1u);

    /* Окно -> хвост -> flush -> заголовок (в commit). */
    if (hal_blk_write(slot_lba(idx, copy) + 1, DATA_SECTORS, data) != 0 ||
        write_tail(nm, ncl, data + RAMFS_MAX_FILE_SIZE, tail) != 0 ||
        hal_blk_flush() != 0) {
        g_errors++;
        release_clusters(nm, ncl);
        return -1;
    }
    if (commit(idx, e, size, data, nm, ncl, tcrc) != 0) {
        /* Кластеры НЕ освобождаем: заголовок мог всё же лечь на диск (упал
           только flush), и тогда на них ссылается актуальная копия. Отдать их
           другому файлу значило бы тихо испортить этот. Они просто остаются
           занятыми до перезагрузки — при монтировании занятость считается заново. */
        return -1;
    }
    return 0;
}

int ramfs_persist_read_tail(size_t idx, size_t off, void *buf, size_t len) {
    if (!g_active || idx >= RAMFS_MAX_ENTRIES) return -1;
    if (len == 0) return 0;
    if ((uint64_t)off + len > (uint64_t)g_ncl[idx] * CLUSTER_BYTES) return -1;

    uint8_t *out = (uint8_t *)buf;
    uint32_t pos = (uint32_t)off;           /* хвост не больше 4 МиБ — влезает в uint32 */
    uint32_t left = (uint32_t)len;
    while (left) {
        uint32_t cl    = pos / CLUSTER_BYTES;
        uint32_t in_cl = pos % CLUSTER_BYTES;
        uint64_t lba   = cluster_lba(g_map[idx][cl]) + in_cl / HAL_BLK_SECTOR;
        uint32_t soff  = in_cl % HAL_BLK_SECTOR;

        if (soff == 0 && left >= HAL_BLK_SECTOR) {
            /* целые сектора — прямо в буфер, не дальше конца кластера */
            uint32_t n = left / HAL_BLK_SECTOR;
            uint32_t room = (CLUSTER_BYTES - in_cl) / HAL_BLK_SECTOR;
            if (n > room) n = room;
            if (hal_blk_read(lba, n, out) != 0) { g_errors++; return -1; }
            out += n * HAL_BLK_SECTOR; pos += n * HAL_BLK_SECTOR; left -= n * HAL_BLK_SECTOR;
        } else {
            uint32_t n = HAL_BLK_SECTOR - soff;
            if (n > left) n = left;
            if (hal_blk_read(lba, 1, g_sec) != 0) { g_errors++; return -1; }
            for (uint32_t i = 0; i < n; i++) out[i] = g_sec[soff + i];
            out += n; pos += n; left -= n;
        }
    }
    return 0;
}

void ramfs_persist_space(uint32_t *free_bytes, uint32_t *total_bytes) {
    uint32_t t = 0, f = 0;
    if (g_active && g_big) { t = g_npool * CLUSTER_BYTES; f = g_nfree * CLUSTER_BYTES; }
    if (free_bytes)  *free_bytes  = f;
    if (total_bytes) *total_bytes = t;
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
