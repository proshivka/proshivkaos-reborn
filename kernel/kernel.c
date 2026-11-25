/* kernel/kernel.c — точка входа ядра. Ничего не знает про x86/ARM64,
 * только про hal.h.
 */
#include "hal.h"
#include "ramfs.h"

void shell_run(void);   /* shell/shell.c */

void kmain(uint32_t multiboot_magic, void *multiboot_info) {
    (void)multiboot_magic;

    hal_arch_init(multiboot_info);
    hal_console_init();
    hal_mem_init();
    ramfs_init();

    hal_console_write("proshivkaOS NEXT [ранний этап]\n");
    hal_console_write("HAL: console/mem/fs initialized.\n");
    hal_console_write("fs: ");
    hal_console_write(ramfs_persist_state_name());
    hal_console_write("\n\n");

    shell_run();

    hal_panic("shell_run() returned unexpectedly");
}
