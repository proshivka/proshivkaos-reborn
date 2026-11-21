/* hal/hal_entropy_stub.c — заглушка источника случайности для платформ, где
 * его пока нет (ARM64). Честно сообщает «нет»: SSH тогда отказывается
 * работать ("no entropy source"), а не использует предсказуемые числа.
 * Когда появится источник (например, генератор SoC), реализуйте
 * hal_entropy() в arch/<платформа>/ и уберите этот файл из сборки. */
#include <stdint.h>
#include <stddef.h>
#include "entropy.h"

int hal_entropy(uint8_t *buf, size_t n) {
    (void)buf; (void)n;
    return -1;
}
