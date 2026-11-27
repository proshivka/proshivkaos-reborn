/* arch/x86/keyboard.c — минимальный опросный (не по прерываниям) PS/2-драйвер.
 * Прерывания подключим на этапе с IDT; пока hal_console_getc_blocking()
 * крутится в цикле и читает порт 0x60, когда бит "буфер полон" установлен.
 *
 * Поддерживает:
 *  - Ctrl-модификатор: буквы под Ctrl превращаются в управляющие коды
 *    1..26 (стандартная схема terminal 'letter & 0x1F' — Ctrl+S=0x13,
 *    Ctrl+Q=0x11, Ctrl+F=0x06 и т.д., см. apps/editor.c).
 *  - Shift-модификатор: заглавные буквы + верхний ряд символов (только
 *    раскладка US QWERTY — другие не поддержаны).
 *  - Расширенные скан-коды (префикс 0xE0): стрелки, Home/End, Delete,
 *    PageUp/PageDown — возвращаются как HAL_KEY_* (см. hal.h), это коды
 *    выше 0xFF, так что с обычным ASCII они никогда не пересекаются.
 *
 * Расширенная последовательность приходит DVUMYA байтами не за один опрос,
 * поэтому состояние "только что видели 0xE0" хранится в pending_e0 между
 * вызовами keyboard_poll() — сам опрос всё ещё неблокирующий, просто на
 * "половинчатый" байт отвечает -1 (нет готового события), как раньше
 * отвечал на события, которых не было в таблице совсем.
 */
#include <stdint.h>
#include "ports.h"
#include "hal.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_OUTPUT_FULL 0x01

/* Скан-коды набора 1 (Set 1), нижний регистр — как и раньше. */
static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0 /*ctrl*/,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0 /*lshift*/,'\\','z','x','c','v','b','n','m',',','.','/',
    0 /*rshift*/,'*',
    0 /*alt*/,' ',
    /* остальное на первом этапе не нужно */
};

/* Символы верхнего ряда под Shift — только там, где они отличаются от
 * заглавной буквы (для букв верхний регистр считается на лету: c-32).
 * Индексы совпадают со scancode_to_ascii выше (US QWERTY). */
static const char shifted_symbols[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+',
    [0x1A]='{', [0x1B]='}',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|',
    [0x33]='<', [0x34]='>', [0x35]='?',
};

static int shift_down  = 0;
static int ctrl_down   = 0;
static int alt_down    = 0;
static int ru_layout   = 0;   /* 0 = EN, 1 = RU (переключение Alt+Shift) */
static int pending_e0  = 0;   /* предыдущий байт был префиксом расширенного скан-кода */

/* Русская раскладка ЙЦУКЕН (Set 1, нижний регистр, коды Юникода). */
static const uint16_t scancode_to_ru[128] = {
    [0x10] = 0x0439, [0x11] = 0x0446, [0x12] = 0x0443, [0x13] = 0x043A, /* й ц у к */
    [0x14] = 0x0435, [0x15] = 0x043D, [0x16] = 0x0433, [0x17] = 0x0448, /* е н г ш */
    [0x18] = 0x0449, [0x19] = 0x0437, [0x1A] = 0x0445, [0x1B] = 0x044A, /* щ з х ъ */
    [0x1E] = 0x0444, [0x1F] = 0x044B, [0x20] = 0x0432, [0x21] = 0x0430, /* ф ы в а */
    [0x22] = 0x043F, [0x23] = 0x0440, [0x24] = 0x043E, [0x25] = 0x043B, /* п р о л */
    [0x26] = 0x0434, [0x27] = 0x0436, [0x28] = 0x044D, [0x29] = 0x0451, /* д ж э ё */
    [0x2C] = 0x044F, [0x2D] = 0x0447, [0x2E] = 0x0441, [0x2F] = 0x043C, /* я ч с м */
    [0x30] = 0x0438, [0x31] = 0x0442, [0x32] = 0x044C, [0x33] = 0x0431, /* и т ь б */
    [0x34] = 0x044E, [0x35] = '.',                                      /* ю .     */
};

static const char ru_shifted_symbols[128] = {
    [0x02] = '!', [0x03] = '"', [0x04] = '#', [0x05] = ';', [0x06] = '%',
    [0x07] = ':', [0x08] = '?', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x2B] = '/', [0x35] = ',',
};

int keyboard_poll(void) {
    uint8_t status = inb(KBD_STATUS_PORT);

    if (!(status & KBD_OUTPUT_FULL))
        return -1;
    if (status & 0x20)          /* бит 5 = байт принадлежит мыши (AUX), не нам */
        return -1;

    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) {
        pending_e0 = 1;
        return -1;              /* сам по себе префикс — ещё не событие */
    }

    int is_extended = pending_e0;
    pending_e0 = 0;

    int     release = (sc & 0x80) != 0;
    uint8_t code    = sc & 0x7F;

    if (is_extended) {
        if (release) {
            if (code == 0x1D) ctrl_down = 0;   /* правый Ctrl отпущен */
            if (code == 0x38) alt_down  = 0;   /* правый Alt отпущен  */
            return -1;
        }
        switch (code) {
            case 0x48: return HAL_KEY_UP;
            case 0x50: return HAL_KEY_DOWN;
            case 0x4B: return HAL_KEY_LEFT;
            case 0x4D: return HAL_KEY_RIGHT;
            case 0x47: return HAL_KEY_HOME;
            case 0x4F: return HAL_KEY_END;
            case 0x53: return HAL_KEY_DEL;
            case 0x49: return HAL_KEY_PAGE_UP;
            case 0x51: return HAL_KEY_PAGE_DOWN;
            case 0x1D: ctrl_down = 1; return -1;   /* правый Ctrl нажат */
            case 0x38:
                if (!alt_down && shift_down) ru_layout = !ru_layout;
                alt_down = 1;
                return -1;                         /* правый Alt нажат  */
            default:   return -1;
        }
    }

    /* модификаторы обрабатываются на обеих сторонах нажатие/отпускание */
    if (code == 0x1D) { ctrl_down = !release; return -1; }                 /* левый Ctrl */
    if (code == 0x38) {
        if (!release && !alt_down && shift_down) ru_layout = !ru_layout;
        alt_down = !release;
        return -1;                                                         /* левый Alt  */
    }
    if (code == 0x2A || code == 0x36) {
        if (!release && !shift_down && alt_down) ru_layout = !ru_layout;
        shift_down = !release;
        return -1;                                                         /* Shift ЛП   */
    }

    if (release)
        return -1;

    if (code >= sizeof(scancode_to_ascii))
        return -1;

    if (ru_layout && !ctrl_down) {
        uint16_t ru = scancode_to_ru[code];
        if (ru) {
            if (shift_down) {
                if (ru >= 0x0430 && ru <= 0x044F) ru -= 0x20;
                else if (ru == 0x0451) ru = 0x0401;
                else if (ru == '.') ru = ',';
            }
            return (int)ru;
        }
        if (shift_down && ru_shifted_symbols[code])
            return (int)(unsigned char)ru_shifted_symbols[code];
    }

    char c = scancode_to_ascii[code];
    if (!c)
        return -1;

    if (shift_down) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (shifted_symbols[code]) c = shifted_symbols[code];
    }

    if (ctrl_down) {
        if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (int)(c - 'A' + 1);
    }

    return (int)(unsigned char)c;
}

int keyboard_get_layout(void) {
    return ru_layout;
}

char keyboard_getc_blocking(void) {
    int c;
    do {
        c = keyboard_poll();
        /* HAL_KEY_* (>255) сюда не помещаются — эта функция возвращает char
         * и предназначена для обычного текстового ввода (см. shell.c);
         * за полным диапазоном — hal_console_getc(), см. apps/editor.c. */
    } while (c == -1 || c > 255);
    return (char)c;
}
