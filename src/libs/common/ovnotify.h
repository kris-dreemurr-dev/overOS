#ifndef OVNOTIFY_H
#define OVNOTIFY_H
// ovnotify.h — уведомления из любой программы (как notify-send / libnotify).
//
//   ov_notify(1, "Готово", "Видео сохранено в ~/Видео", "");
//
// Сообщение уходит композитору (IPC "desktop"), ответа ждать не надо. Шапку
// уведомления -- значок и имя программы -- композитор берёт из её .prg, поэтому
// подписываться за систему или чужую программу бессмысленно. Кнопки: заполните
// actions и ловите WP_EV_NOTIFY_ACTION (в libui -- ui_notify_action).

#include "ovos.h"
#include "wproto.h"

static inline void ov_notify_copy(char* d, const char* s, int max) {
    int i = 0;
    if (s) while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    if (s && s[i]) while (i > 0 && (((uint8_t)s[i]) & 0xC0) == 0x80) i--;   // не посреди символа
    d[i] = '\0';
}

// Полная форма: срочность, время показа и до двух кнопок. 0 -- отправлено.
static inline int ov_notify_ex(uint32_t id, int urgency, int timeout_s, const char* icon,
                               const char* title, const char* body,
                               const char* action0, const char* action1) {
    int pid = ov_lookup(WP_SERVICE_NAME);
    if (pid <= 0) return -1;                 // рабочего стола нет (консоль)
    wp_notify_t n;
    uint8_t* z = (uint8_t*)&n;
    for (int i = 0; i < (int)sizeof(n); i++) z[i] = 0;
    n.type = WP_NOTIFY;
    n.id = id ? id : 1;
    n.urgency = (uint16_t)urgency;
    n.timeout_s = (uint16_t)timeout_s;
    ov_notify_copy(n.icon, icon, sizeof(n.icon));
    ov_notify_copy(n.title, title, sizeof(n.title));
    ov_notify_copy(n.body, body, sizeof(n.body));
    ov_notify_copy(n.actions[0], action0, sizeof(n.actions[0]));
    ov_notify_copy(n.actions[1], action1, sizeof(n.actions[1]));
    return ov_send(pid, &n, sizeof(n)) < 0 ? -1 : 0;
}

static inline int ov_notify(uint32_t id, const char* title, const char* body, const char* icon) {
    return ov_notify_ex(id, WP_URGENCY_NORMAL, 0, icon, title, body, 0, 0);
}

static inline void ov_notify_close(uint32_t id) {
    int pid = ov_lookup(WP_SERVICE_NAME);
    if (pid <= 0) return;
    wp_notify_close_t c = { WP_NOTIFY_CLOSE, id };
    ov_send(pid, &c, sizeof(c));
}

#endif
