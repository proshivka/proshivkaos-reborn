/* arch/arm64/virtio_blk.c — блочное устройство virtio-mmio (диск QEMU).
 *
 * Реализует интерфейс hal/hal_blk.h для эмулятора. Транспорт тот же, что у
 * virtio_input.c: те же регистры, те же две версии (legacy и modern) и тот
 * же способ обслуживать кэш — чистим то, что отдаём устройству, и
 * выбрасываем то, что устройство написало само. Отличие одно, но
 * принципиальное: ввод — это очередь, которую устройство заполняет без
 * спроса, а диск — это запрос-ответ. Поэтому здесь очередь одна, запросы
 * идут по одному, и каждый ждётся опросом до конца (прерываний у нас нет).
 *
 * ОДИН ЗАПРОС = ТРИ ДЕСКРИПТОРА, сцепленных в цепочку:
 *     [заголовок: тип, сектор]  -> читает устройство
 *     [данные, до 4 КиБ]        -> читает (запись) или пишет (чтение) устройство
 *     [байт статуса]            -> пишет устройство: 0 = успех
 * Для flush данных нет: заголовок -> статус.
 *
 * ДАННЫЕ ХОДЯТ ЧЕРЕЗ СВОЙ БУФЕР (bounce). Устройство читает и пишет память
 * мимо кэша процессора, а invalidate работает целыми строками кэша — на
 * чужом, невыровненном буфере он заодно выбросил бы соседние данные,
 * которые мы ещё не успели записать. Свой буфер выровнен по строке и
 * занимает строки целиком, поэтому такой беды нет. За это платим одним
 * копированием по 4 КиБ — для диска, живущего опросом, это не цена.
 *
 * ЕСЛИ УСТРОЙСТВО НЕ ОТВЕТИЛО за отведённое число опросов, диск объявляется
 * неисправным навсегда: оно ещё может дописать в наши буферы позже, и
 * продолжать работу с ними значило бы получить порчу памяти.
 *
 * На телефоне этот файл ничего не делает: узла virtio,mmio в дереве нет,
 * и опрос адресов QEMU (на Snapdragon он вешает шину) не выполняется.
 */
#include "arm64.h"
#include "fdt.h"
#include "hal_blk.h"

/* ---- Регистры транспорта (те же, что в virtio_input.c) ---- */
#define VM_MAGIC            0x000
#define VM_VERSION          0x004
#define VM_DEVICE_ID        0x008
#define VM_DEV_FEAT         0x010
#define VM_DEV_FEAT_SEL     0x014
#define VM_DRV_FEAT         0x020
#define VM_DRV_FEAT_SEL     0x024
#define VM_QUEUE_SEL        0x030
#define VM_QUEUE_NUM_MAX    0x034
#define VM_QUEUE_NUM        0x038
#define VM_QUEUE_NOTIFY     0x050
#define VM_INT_STATUS       0x060
#define VM_INT_ACK          0x064
#define VM_STATUS           0x070
#define VM_CONFIG           0x100

#define VM_GUEST_PAGE_SIZE  0x028     /* только legacy */
#define VM_QUEUE_ALIGN      0x03c
#define VM_QUEUE_PFN        0x040

#define VM_QUEUE_READY      0x044     /* только modern */
#define VM_QUEUE_DESC_LO    0x080
#define VM_QUEUE_DESC_HI    0x084
#define VM_QUEUE_DRIVER_LO  0x090
#define VM_QUEUE_DRIVER_HI  0x094
#define VM_QUEUE_DEVICE_LO  0x0a0
#define VM_QUEUE_DEVICE_HI  0x0a4

#define VIRTIO_MAGIC        0x74726976
#define VIRTIO_ID_BLOCK     2

#define ST_ACKNOWLEDGE      1
#define ST_DRIVER           2
#define ST_DRIVER_OK        4
#define ST_FEATURES_OK      8

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

/* ---- Специфика virtio-blk ---- */
#define BLK_F_RO            (1u << 5)    /* устройство только для чтения */
#define BLK_F_FLUSH         (1u << 9)    /* понимает запрос flush        */

#define BLK_T_IN            0u           /* чтение  */
#define BLK_T_OUT           1u           /* запись  */
#define BLK_T_FLUSH         4u

#define BLK_S_OK            0u

#define VQ_SIZE             16
#define VQ_ALIGN            4096
#define VQ_RING_SIZE        8192
#define BOUNCE_SECTORS      8u
#define BOUNCE_BYTES        (BOUNCE_SECTORS * HAL_BLK_SECTOR)
#define POLL_LIMIT          20000000u    /* опросов до объявления сбоя */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];
} vring_used_t;

/* Заголовок запроса занимает строку кэша целиком (64 байта), хотя сам
 * весит 16: см. замечание про invalidate выше. То же — статус. */
typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t  pad[48];
} blk_hdr_t;

static uint8_t   g_ring[VQ_RING_SIZE]  __attribute__((aligned(VQ_ALIGN)));
static blk_hdr_t g_hdr                 __attribute__((aligned(64)));
static uint8_t   g_status[64]          __attribute__((aligned(64)));
static uint8_t   g_bounce[BOUNCE_BYTES] __attribute__((aligned(64)));

static vring_desc_t  *g_desc;
static vring_avail_t *g_avail;
static vring_used_t  *g_used;
static uint32_t g_avail_off;
static uint32_t g_qsize;
static uint16_t g_last_used;
static uint16_t g_avail_idx;

static uint64_t g_base;
static uint64_t g_sectors;
static int g_has_flush;
static int g_read_only;

/* 0 — ещё не искали, 1 — диск работает, 2 — диска нет, 3 — сломался */
static int g_state = 0;

static int setup_device(uint64_t base, uint32_t version) {
    g_base = base;

    mmio_write32(base + VM_STATUS, 0);
    mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE);
    mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER);

    /* Из предложенных возможностей берём только две, обе безобидные:
       "только чтение" (чтобы знать об этом заранее) и flush. */
    mmio_write32(base + VM_DEV_FEAT_SEL, 0);
    uint32_t feat_lo = mmio_read32(base + VM_DEV_FEAT);
    uint32_t want_lo = feat_lo & (BLK_F_RO | BLK_F_FLUSH);
    g_read_only = (want_lo & BLK_F_RO) != 0;
    g_has_flush = (want_lo & BLK_F_FLUSH) != 0;

    mmio_write32(base + VM_DRV_FEAT_SEL, 0);
    mmio_write32(base + VM_DRV_FEAT, want_lo);

    if (version != 1) {
        mmio_write32(base + VM_DEV_FEAT_SEL, 1);
        uint32_t feat_hi = mmio_read32(base + VM_DEV_FEAT);
        mmio_write32(base + VM_DRV_FEAT_SEL, 1);
        mmio_write32(base + VM_DRV_FEAT, feat_hi & 1u);   /* VIRTIO_F_VERSION_1 */

        mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK);
        if (!(mmio_read32(base + VM_STATUS) & ST_FEATURES_OK))
            return 0;
    }

    mmio_write32(base + VM_QUEUE_SEL, 0);
    uint32_t max = mmio_read32(base + VM_QUEUE_NUM_MAX);
    if (max == 0) return 0;

    g_qsize = (max < VQ_SIZE) ? max : VQ_SIZE;
    mmio_write32(base + VM_QUEUE_NUM, g_qsize);

    /* Раскладка колец диктуется спецификацией — формула та же, что в
       virtio_input.c (в legacy устройство считает адреса само). */
    g_avail_off = 16u * g_qsize;
    uint32_t used_off = (g_avail_off + 6u + 2u * g_qsize + (VQ_ALIGN - 1)) & ~(uint32_t)(VQ_ALIGN - 1);
    if (used_off + 6u + 8u * g_qsize > VQ_RING_SIZE)
        return 0;

    g_desc  = (vring_desc_t  *)(g_ring);
    g_avail = (vring_avail_t *)(g_ring + g_avail_off);
    g_used  = (vring_used_t  *)(g_ring + used_off);

    for (size_t i = 0; i < sizeof(g_ring); i++) g_ring[i] = 0;

    uint64_t ring_pa = (uint64_t)(uintptr_t)g_ring;

    if (version == 1) {
        mmio_write32(base + VM_GUEST_PAGE_SIZE, VQ_ALIGN);
        mmio_write32(base + VM_QUEUE_ALIGN, VQ_ALIGN);
        mmio_write32(base + VM_QUEUE_PFN, (uint32_t)(ring_pa / VQ_ALIGN));
    } else {
        uint64_t avail_pa = ring_pa + g_avail_off;
        uint64_t used_pa  = ring_pa + used_off;
        mmio_write32(base + VM_QUEUE_DESC_LO,   (uint32_t)ring_pa);
        mmio_write32(base + VM_QUEUE_DESC_HI,   (uint32_t)(ring_pa >> 32));
        mmio_write32(base + VM_QUEUE_DRIVER_LO, (uint32_t)avail_pa);
        mmio_write32(base + VM_QUEUE_DRIVER_HI, (uint32_t)(avail_pa >> 32));
        mmio_write32(base + VM_QUEUE_DEVICE_LO, (uint32_t)used_pa);
        mmio_write32(base + VM_QUEUE_DEVICE_HI, (uint32_t)(used_pa >> 32));
        mmio_write32(base + VM_QUEUE_READY, 1);
    }

    g_last_used = 0;
    g_avail_idx = 0;

    /* Ёмкость — первое поле конфигурации (le64, в секторах по 512 байт).
       Читаем двумя 32-битными обращениями: 64-битное к регистрам virtio
       спецификация не гарантирует. */
    uint64_t lo = mmio_read32(base + VM_CONFIG + 0);
    uint64_t hi = mmio_read32(base + VM_CONFIG + 4);
    g_sectors = (hi << 32) | lo;

    arch_dcache_clean(g_ring, sizeof(g_ring));
    dsb_sy();

    mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER |
                                   (version == 1 ? 0 : ST_FEATURES_OK) | ST_DRIVER_OK);
    return 1;
}

int hal_blk_init(void) {
    if (g_state != 0) return g_state == 1;
    g_state = 2;

    /* Как и в virtio_input_init(): без узла virtio,mmio в дереве по
       адресам QEMU не ходим — на Snapdragon это вешает шину. Дерева нет
       вовсе — значит, мы, скорее всего, под эмулятором без дерева. */
    if (fdt_valid()) {
        fdt_node_t node;
        if (!fdt_find_compatible("virtio,mmio", &node))
            return 0;
    }

    for (int slot = 0; slot < VIRT_VIRTIO_SLOTS; slot++) {
        uint64_t base = VIRT_VIRTIO_BASE + (uint64_t)slot * VIRT_VIRTIO_STRIDE;

        if (mmio_read32(base + VM_MAGIC) != VIRTIO_MAGIC) continue;
        uint32_t version = mmio_read32(base + VM_VERSION);
        if (version != 1 && version != 2) continue;
        if (mmio_read32(base + VM_DEVICE_ID) != VIRTIO_ID_BLOCK) continue;

        if (!setup_device(base, version)) {
            uart_write("virtio-blk: slot ");
            uart_write_hex((uint64_t)slot);
            uart_write(" ne inicializirovan\n");
            continue;
        }

        uart_write("virtio-blk: slot ");
        uart_write_hex((uint64_t)slot);
        uart_write(" sektorov ");
        uart_write_hex(g_sectors);
        uart_write(g_read_only ? " (tolko chtenie)\n" : "\n");
        g_state = 1;
        return 1;
    }
    return 0;
}

uint64_t hal_blk_sectors(void) {
    return (g_state == 1) ? g_sectors : 0;
}

/* Один запрос от начала до конца. len — число байт данных в g_bounce
 * (0 для flush). 0 — успех. */
static int do_request(uint32_t type, uint64_t sector, uint32_t len) {
    g_hdr.type     = type;
    g_hdr.reserved = 0;
    g_hdr.sector   = sector;
    g_status[0]    = 0xFF;                       /* "ещё не отвечало" */

    uint16_t n = 0;
    g_desc[n].addr  = (uint64_t)(uintptr_t)&g_hdr;
    g_desc[n].len   = 16;
    g_desc[n].flags = VRING_DESC_F_NEXT;
    g_desc[n].next  = (uint16_t)(n + 1);
    n++;

    if (len) {
        g_desc[n].addr  = (uint64_t)(uintptr_t)g_bounce;
        g_desc[n].len   = len;
        g_desc[n].flags = VRING_DESC_F_NEXT | (type == BLK_T_IN ? VRING_DESC_F_WRITE : 0);
        g_desc[n].next  = (uint16_t)(n + 1);
        n++;
    }

    g_desc[n].addr  = (uint64_t)(uintptr_t)g_status;
    g_desc[n].len   = 1;
    g_desc[n].flags = VRING_DESC_F_WRITE;
    g_desc[n].next  = 0;

    /* Цепочка начинается с дескриптора 0. Сначала слот в avail-кольце,
       барьер, потом индекс — устройство не должно увидеть индекс раньше
       слота. */
    g_avail->ring[g_avail_idx % g_qsize] = 0;
    g_avail_idx++;
    dmb_sy();
    g_avail->idx = g_avail_idx;

    arch_dcache_clean(g_ring, g_avail_off + 6u + 2u * g_qsize);
    arch_dcache_clean(&g_hdr, sizeof(g_hdr));
    arch_dcache_clean(g_status, sizeof(g_status));
    dsb_sy();

    mmio_write32(g_base + VM_QUEUE_NOTIFY, 0);

    uint32_t spins = 0;
    for (;;) {
        arch_dcache_invalidate(g_used, 6u + 8u * g_qsize);
        if (g_last_used != g_used->idx) break;
        if (++spins >= POLL_LIMIT) {
            g_state = 3;                         /* больше к диску не лезем */
            uart_write("virtio-blk: tajmaut, disk otklyuchen\n");
            return -1;
        }
    }
    g_last_used++;

    uint32_t is = mmio_read32(g_base + VM_INT_STATUS);
    if (is) mmio_write32(g_base + VM_INT_ACK, is);

    arch_dcache_invalidate(g_status, sizeof(g_status));
    if (type == BLK_T_IN && len)
        arch_dcache_invalidate(g_bounce, BOUNCE_BYTES);

    return (g_status[0] == BLK_S_OK) ? 0 : -1;
}

static int xfer(int is_write, uint64_t lba, uint32_t count, uint8_t *buf) {
    if (g_state != 1) return -1;
    if (count == 0) return 0;
    if (lba >= g_sectors || (uint64_t)count > g_sectors - lba) return -1;
    if (is_write && g_read_only) return -1;

    while (count) {
        uint32_t n = (count < BOUNCE_SECTORS) ? count : BOUNCE_SECTORS;
        uint32_t bytes = n * HAL_BLK_SECTOR;

        if (is_write) {
            for (uint32_t i = 0; i < bytes; i++) g_bounce[i] = buf[i];
            arch_dcache_clean(g_bounce, BOUNCE_BYTES);
        }

        if (do_request(is_write ? BLK_T_OUT : BLK_T_IN, lba, bytes) != 0)
            return -1;

        if (!is_write)
            for (uint32_t i = 0; i < bytes; i++) buf[i] = g_bounce[i];

        lba   += n;
        buf   += bytes;
        count -= n;
    }
    return 0;
}

int hal_blk_read(uint64_t lba, uint32_t count, void *buf) {
    return xfer(0, lba, count, (uint8_t *)buf);
}

int hal_blk_write(uint64_t lba, uint32_t count, const void *buf) {
    return xfer(1, lba, count, (uint8_t *)(uintptr_t)buf);
}

int hal_blk_flush(void) {
    if (g_state != 1) return -1;
    if (!g_has_flush) return 0;     /* устройство пишет сквозь кэш — барьер не нужен */
    return do_request(BLK_T_FLUSH, 0, 0);
}
