#ifndef NOTIFIER_H
#define NOTIFIER_H
// notifier.h — уведомления рабочего стола (см. notifier.c): всплывающие
// карточки в правом нижнем углу, как у демона уведомлений Plasma.

#include <stdint.h>
#include "wproto.h"

void notifier_init(void);
// Раз в кадр цикла: забрать уведомления ядра, отсчитать время показа.
// 1 -- картинка изменилась.
int  notifier_tick(int mx, int my);
void notifier_render(int mx, int my);
// Клик левой кнопкой: 1 -- попал в уведомление (дальше не передавать)
int  notifier_click(int mx, int my);
int  notifier_inside(int mx, int my);
int  notifier_overlaps(int x, int y, int w, int h);
// Уведомление от программы (WP_NOTIFY) и его отзыв (WP_NOTIFY_CLOSE)
void notifier_post_app(const wp_notify_t* n, int sender_pid);
void notifier_close_app(uint32_t id, int sender_pid);
// Выход из сеанса: уведомления программ уходят вместе с ними
void notifier_clear_apps(void);

#endif
