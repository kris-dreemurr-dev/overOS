#ifndef CONTHEME_H
#define CONTHEME_H
// contheme.h — палитры текстовой консоли overOS.
//
// Консолей у нас две, и выглядеть они должны по-разному — ровно как в Linux,
// где системная консоль осталась классической, а эмулятор терминала в
// графическом сеансе оформлен по вкусу темы:
//
//   * tty1..tty6 (текстовые консоли на экране) — классический вид: чёрный фон
//     и яркие цвета VGA. Это то, что видно при загрузке и в аварийном режиме,
//     и менять его не нужно: так консоль узнаётся с первого взгляда.
//   * псевдотерминалы (окно overTerm) — спокойная тёмная тема в духе One Dark:
//     мягкий серо-синий фон и приглушённые цвета, на которых текст читается, а
//     не светится.
//
// Пишут в консоль все одними и теми же именами CON_* (ядро, /sys/sh.prg,
// fastfetch, вход в систему). Это цвета ТЕРМИНАЛА; для текстовой консоли ядро
// переводит их в классические при отрисовке (con_classic в kernel.c), так что
// вызывающему не нужно знать, куда он пишет.

// --- Терминал (псевдотерминал в окне) ---
#define CON_BG        0x1E2127   // фон
#define CON_BG_ALT    0x282C34   // полоса заголовка и выделение
#define CON_FG        0xABB2BF   // обычный текст
#define CON_DIM       0x7F848E   // служебные подписи, заголовки таблиц
#define CON_CURSOR    0xABB2BF

#define CON_RED       0xE06C75   // ошибки
#define CON_GREEN     0x98C379   // имя пользователя, успех
#define CON_YELLOW    0xE5C07B   // предупреждения, имена потоков ядра
#define CON_BLUE      0x61AFEF   // пути
#define CON_MAGENTA   0xC678DD
#define CON_CYAN      0x56B6C2   // подсказки, заголовки разделов
#define CON_WHITE     0xFFFFFF   // то, что набрал пользователь

// --- Текстовая консоль tty1..tty6 (классический вид) ---
#define CONT_BG       0x000000
#define CONT_BG_ALT   0x202020
#define CONT_FG       0xFFFFFF
#define CONT_DIM      0xAAAAAA
#define CONT_CURSOR   0xFFFFFF

#define CONT_RED      0xFF5555
#define CONT_GREEN    0x55FF55
#define CONT_YELLOW   0xFFFF55
#define CONT_BLUE     0x5555FF
#define CONT_MAGENTA  0xFF55FF
#define CONT_CYAN     0x55FFFF
#define CONT_WHITE    0xFFFFFF

#endif // CONTHEME_H
