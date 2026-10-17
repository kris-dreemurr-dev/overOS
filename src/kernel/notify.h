#ifndef NOTIFY_H
#define NOTIFY_H
// notify.h — уведомления от имени системы (см. notify.c).
//
// Ядро не рисует: оно кладёт уведомление в очередь, а рабочий стол забирает
// его сисколлом 133 и показывает всплывающей карточкой, как системный демон
// уведомлений в KDE. Уведомления программ идут мимо ядра, по оконному
// протоколу (WP_NOTIFY), но выглядят так же.

#include <stdint.h>

#define KNOTIFY_LOW       0
#define KNOTIFY_NORMAL    1
#define KNOTIFY_CRITICAL  2       // не исчезает сам: пока причина не ушла

// Раскладка совпадает с ov_knotify_t в ovos.h (копируется в Ring 3 как есть)
typedef struct {
    uint32_t id;                  // повторный id заменяет прежнюю карточку
    uint16_t urgency;             // KNOTIFY_*
    uint16_t timeout_s;           // 0 -- по умолчанию; у CRITICAL не действует
    char     icon[24];            // значок из /sys/icons
    char     title[64];
    char     body[200];
    char     act_label[2][20];    // кнопки ("Открыть", "Извлечь"), пусто -- нет
    char     act_cmd[2][64];      // что сделать: "open:/media/X", "eject:X"
    uint32_t close;               // 1 -- просто убрать карточку с этим id
} knotify_t;

// Положить уведомление в очередь. id 0 -- выдать новый. Возвращает id.
uint32_t knotify_post(const knotify_t* n);
// Убрать карточку (например, "флешка отключена", когда флешка вернулась)
void     knotify_close(uint32_t id);
// Забрать следующее (рабочий стол, sys_notify_poll): 1 -- есть, 0 -- пусто
int      knotify_pop(knotify_t* out);

// Удобная сборка: строки обрезаются по размеру полей
void     knotify_fill(knotify_t* n, const char* icon, const char* title, const char* body);
void     knotify_action(knotify_t* n, int i, const char* label, const char* cmd);

#endif
