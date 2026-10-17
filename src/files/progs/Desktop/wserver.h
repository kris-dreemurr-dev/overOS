#ifndef WSERVER_H
#define WSERVER_H

#include <stdint.h>

// Композитор: серверная часть оконного протокола (src/lib/wproto.h)

int  wserver_init(void);               // регистрирует IPC-имя "desktop"
// Ждёт сообщения клиентов до wait_ms и обрабатывает все накопившиеся. 1 — нужно перерисовать.
int  wserver_poll(uint32_t wait_ms);
int  wserver_has_commits(void);        // есть окна с новым, ещё не выведенным кадром
void wserver_frame_done(void);         // после вывода кадра: WP_FRAME_DONE тем, кто прислал COMMIT
int  wserver_tick(void);               // упавшие клиенты, просроченное закрытие. 1 — нужно перерисовать.
void wserver_close_all(void);          // выход из сеанса
// Отдать путь уже открытой программе класса app (WP_EV_OPEN). 1 -- нашлась.
int  wserver_hand_over(const char* app, const char* path);

#endif
