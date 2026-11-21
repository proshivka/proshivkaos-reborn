/* net/ssh/ssh.h — клиент SSH-2.
 *
 * Что умеет (один набор алгоритмов — зато современный и проверенный):
 *   обмен ключами   curve25519-sha256 (RFC 8731)
 *   ключ сервера    ssh-ed25519
 *   шифр + MAC      chacha20-poly1305@openssh.com
 *   вход            по паролю
 *   сессия          интерактивная оболочка (с pty) или одна команда (exec)
 *
 * Чего нет: ключевой аутентификации, смены ключей посреди сеанса, сжатия,
 * перенаправления портов, других алгоритмов (старые серверы без ed25519 и
 * chacha20 не подойдут — клиент скажет, чего именно не хватает).
 *
 * Ядро не знает ни про сеть, ни про экран: всё идёт через ssh_io_t, поэтому
 * его можно проверять на хосте с обычными сокетами (tests/).
 */
#ifndef SSH_SSH_H
#define SSH_SSH_H

#include <stdint.h>
#include <stddef.h>

#define SSH_OK                0
#define SSH_ERR_IO           -1   /* соединение оборвалось или не удалось отправить */
#define SSH_ERR_PROTO        -2   /* сервер нарушил протокол */
#define SSH_ERR_ALGO         -3   /* нет общих алгоритмов */
#define SSH_ERR_HOSTKEY      -4   /* подпись сервера не сошлась */
#define SSH_ERR_HOSTKEY_REJ  -5   /* ключ сервера не принят пользователем */
#define SSH_ERR_AUTH         -6   /* пароль не подошёл или метод не разрешён */
#define SSH_ERR_CHANNEL      -7   /* сервер не открыл сессию / отказал в оболочке */
#define SSH_ERR_TIMEOUT      -8
#define SSH_ERR_ENTROPY      -9   /* нечем сгенерировать случайные числа */
#define SSH_ERR_ABORT       -10   /* прервано пользователем */
#define SSH_ERR_TOOBIG      -11   /* пакет не помещается в буфер */
#define SSH_ERR_DISCONNECT  -12   /* сервер сам закрыл соединение (причина в журнале) */
#define SSH_ERR_REKEY       -13   /* сервер потребовал смену ключей — не поддерживается */

/* «Итерации ожидания» — такая же единица, как в tcp_recv(). */
#define SSH_WAIT_LONG   20000000L   /* рукопожатие: ждать долго */
#define SSH_WAIT_SHORT  3000L       /* интерактивный цикл: заглянуть в сеть и вернуться */

#define SSH_RECV_TIMEOUT (-2)

typedef struct {
    void *ctx;

    /* Сеть. send: 0 — ок. recv: > 0 — число байт; 0 — собеседник закрыл
     * соединение; SSH_RECV_TIMEOUT — за отведённое время ничего не пришло
     * (можно вызвать ещё раз); любое другое отрицательное — ошибка. */
    int (*send)(void *ctx, const uint8_t *data, size_t len);
    int (*recv)(void *ctx, uint8_t *buf, size_t max, long wait);

    /* Экран и клавиатура. */
    void (*put_out)(void *ctx, const uint8_t *data, size_t len);   /* вывод удалённой стороны */
    int  (*poll_key)(void *ctx);                                   /* байт 0..255 или -1 */
    int  (*should_abort)(void *ctx);                               /* 1 — прервать (можно NULL) */

    /* Сообщения клиента пользователю ("connecting...", причины отказов). */
    void (*log)(void *ctx, const char *msg);

    /* Ключ сервера: fp — отпечаток "SHA256:...", hash — сам SHA-256 блоба
     * ключа. Вернуть 1, если ключ принят (известный хост или пользователь
     * подтвердил). */
    int (*confirm_hostkey)(void *ctx, const char *fp, const uint8_t hash[32]);

    /* Источник случайности: заполнить buf, вернуть 0. Отдельный от
     * генератора внутри — тот лишь размножает то, что дал источник. */
    int (*entropy)(void *ctx, uint8_t *buf, size_t len);

    /* Интерактивный сеанс (оболочка) начался (1) / закончился (0). Нужен,
     * чтобы терминал на время сеанса отдавал клавиши удалённой стороне
     * (в том числе Ctrl+C), а не обрабатывал их сам. Можно NULL. */
    void (*on_session)(void *ctx, int active);

    /* Запрос пароля в момент входа (после проверки ключа сервера). Заполнить
     * buf, вернуть длину; < 0 — пользователь отказался. Используется, если
     * params.password == NULL. */
    int (*get_password)(void *ctx, char *buf, size_t max);
} ssh_io_t;

typedef struct {
    const char *user;
    char       *password;     /* стирается после отправки */
    const char *command;      /* NULL — интерактивная оболочка */
    int         pty;          /* 1 — запросить терминал (для оболочки) */
    int         cols, rows;
    const char *term;         /* "dumb" по умолчанию: наш экран не понимает escape-последовательностей */

    int         exit_status;  /* на выходе: код возврата удалённой команды (-1, если не пришёл) */
} ssh_params_t;

int ssh_run(const ssh_io_t *io, ssh_params_t *params);

const char *ssh_strerror(int code);

#endif
