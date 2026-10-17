// notify.c — уведомление из командной строки, как notify-send в Linux.
//
//   notify Заголовок текст уведомления...
//   notify "Сборка готова" Можно забирать образ
//
// Первое слово (или фраза в кавычках) -- заголовок, остальное -- текст.
// Показывает рабочий стол; в шапке будет имя этой программы ("notify"), а не
// то, что ей передали: композитор берёт его из исполняемого файла отправителя.

#include "ovos.h"
#include "ovnotify.h"

int main(void) {
    char arg[128];
    if (ov_get_arg(arg, sizeof(arg)) <= 0 || !arg[0]) {
        ov_print("использование: notify Заголовок текст...\n");
        ov_exit(1);
    }

    char title[64], body[120];
    const char* p = arg;
    while (*p == ' ') p++;
    int t = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && t < (int)sizeof(title) - 1) title[t++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && t < (int)sizeof(title) - 1) title[t++] = *p++;
    }
    title[t] = '\0';
    while (*p == ' ') p++;
    ov_strlcpy(body, p, sizeof(body));

    if (ov_notify(1, title, body, "") != 0) {
        ov_print("notify: рабочий стол не запущен\n");
        ov_exit(2);
    }
    ov_exit(0);
    return 0;
}
