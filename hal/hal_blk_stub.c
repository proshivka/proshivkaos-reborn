/* hal/hal_blk_stub.c — слабые заглушки блочного устройства.
 *
 * Для платформ, где драйвера диска ещё нет (x86, телефон до появления
 * SDHCI). Настоящий драйвер (arch/arm64/virtio_blk.c) определяет те же
 * символы без weak и перекрывает эти. Файловая система, увидев
 * hal_blk_init() == 0, просто остаётся в ОЗУ.
 */
#include "hal_blk.h"

__attribute__((weak)) int      hal_blk_init(void)    { return 0; }
__attribute__((weak)) uint64_t hal_blk_sectors(void) { return 0; }
__attribute__((weak)) int      hal_blk_read(uint64_t lba, uint32_t n, void *b)
    { (void)lba; (void)n; (void)b; return -1; }
__attribute__((weak)) int      hal_blk_write(uint64_t lba, uint32_t n, const void *b)
    { (void)lba; (void)n; (void)b; return -1; }
__attribute__((weak)) int      hal_blk_flush(void)   { return -1; }
