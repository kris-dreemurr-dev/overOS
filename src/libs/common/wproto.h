#ifndef WPROTO_H
#define WPROTO_H

// =============================================================================
// Оконный протокол overOS (по смыслу — маленький Wayland).
//
// Композитор (desktop.prg) регистрирует IPC-имя "desktop". Клиент:
//   1. создаёт буфер в разделяемой памяти (w*h*4 байт, 32 bpp XRGB) и отображает его к себе;
//   2. шлёт WP_CREATE с id буфера — композитор отображает тот же буфер и рисует окно
//      прямо из него, без копирования через ядро;
//   3. рисует в буфер и шлёт WP_COMMIT ("кадр готов");
//   4. ждёт WP_FRAME_DONE, прежде чем рисовать следующий кадр (композитор уже вывел
//      предыдущий) — так не бывает разрывов и лишних кадров, которых никто не увидит.
// События ввода (мышь, клавиатура, колесо, фокус, закрытие) приходят сообщениями.
// Все сообщения начинаются с uint32_t type; отправителя подписывает ядро.
// =============================================================================

#include <stdint.h>

#define WP_SERVICE_NAME "desktop"

// --- Клиент -> композитор ---
#define WP_CREATE       1
#define WP_COMMIT       2
#define WP_DESTROY      3
#define WP_SET_TITLE    4
#define WP_OPEN         5      // "отдай файл уже запущенной программе этого класса"
#define WP_RESIZE       6      // новый буфер под размер, который прислал композитор
#define WP_CURSOR       7      // какой курсор показывать над этим окном
#define WP_NOTIFY       8      // показать уведомление (окно для этого не нужно)
#define WP_NOTIFY_CLOSE 9      // убрать своё уведомление
#define WP_GRAB         10     // захватить мышь (игры): курсор прячется, приходят WP_EV_MOTION

// --- Композитор -> клиент ---
#define WP_CREATED      100
#define WP_FRAME_DONE   101
#define WP_EV_MOUSE     102
#define WP_EV_KEY       103
#define WP_EV_SCROLL    104
#define WP_EV_FOCUS     105
#define WP_EV_CLOSE     106    // пользователь нажал [X]: клиент должен завершиться
#define WP_EV_COMMAND   107    // команда из меню композитора (контекстное меню и т.п.)
#define WP_EV_OPEN      108    // открыть ещё один файл в уже работающей программе
#define WP_OPENED       109    // ответ на WP_OPEN: нашлась ли такая программа
#define WP_EV_NOTIFY_ACTION 112 // пользователь нажал кнопку (или на само уведомление)
#define WP_EV_MOTION    113    // захваченная мышь: смещение и кнопки (без упора в край экрана)
#define WP_EV_GRAB      114    // захват включён / снят (Meta, смена окна — композитором)
#define WP_EV_RESIZE    110    // изменить размер окна (разворачивание): клиент
                               // выделяет новый буфер и отвечает WP_RESIZE

// Флаги окна
#define WP_FLAG_FILE_BROWSER 0x01   // контекстное меню "создать файл/папку" (вместо угадывания по заголовку)
#define WP_FLAG_RESIZABLE    0x02   // окно умеет перерисовываться под другой размер (кнопка «развернуть»)

typedef struct {
    uint32_t type;           // WP_CREATE
    int32_t  shm_id;         // буфер w*h*4
    int32_t  w, h;
    uint32_t flags;          // WP_FLAG_*
    uint16_t icon[16];       // монохромная иконка 16x16 (0 — заглушка)
    char     title[64];      // UTF-8
    char     app[24];        // класс программы ("ovplayer"): по нему ищут уже открытое окно
} wp_create_t;

typedef struct {
    uint32_t type;           // WP_CREATED
    int32_t  win;            // id окна, < 0 — отказ
} wp_created_t;

typedef struct {
    uint32_t type;           // WP_COMMIT
    int32_t  win;
    // Смещение готового кадра в буфере клиента, в ПИКСЕЛЯХ. Буфер вдвое больше
    // окна: клиент рисует в одну половину, а показывает другую -- композитор
    // читает только законченный кадр. Без этого он успевал прочитать окно в
    // середине отрисовки (например, сразу после ui_clear), и при перетаскивании
    // окна видео мигало. 0 -- старый однобуферный клиент.
    uint32_t off;
} wp_commit_t;

typedef struct {
    uint32_t type;           // WP_DESTROY / WP_FRAME_DONE / WP_EV_CLOSE
    int32_t  win;
} wp_simple_t;

// Разворачивание окна. Буфер кадра выделяет клиент, поэтому поменять размер в
// одностороннем порядке композитор не может: он шлёт WP_EV_RESIZE с новым
// размером, клиент создаёт буфер и отвечает WP_RESIZE, и только тогда композитор
// переключается на новый буфер и новую геометрию (иначе один кадр показался бы
// мусором из буфера прежнего размера).
typedef struct {
    uint32_t type;           // WP_EV_RESIZE
    int32_t  win;
    int32_t  w, h;
} wp_resize_t;

typedef struct {
    uint32_t type;           // WP_RESIZE
    int32_t  win;
    int32_t  shm_id;         // новый буфер w*h*4
    int32_t  w, h;
} wp_newbuf_t;

// Форма курсора. Края и углы окна композитор подставляет сам, а внутри окна
// форму выбирает программа: текстовое поле просит WP_CUR_TEXT, долгая операция —
// WP_CUR_BUSY. Так же это устроено в X и Wayland: курсор принадлежит
// поверхности, а не экрану.
#define WP_CUR_ARROW   0
#define WP_CUR_TEXT    1       // двутавр над текстовым полем
#define WP_CUR_BUSY    2       // часы: программа занята
#define WP_CUR_HAND    3       // «палец»: ссылка, кликабельный элемент
#define WP_CUR_MOVE    4       // перетаскивание
#define WP_CUR_SIZE_H  5       // <-> горизонтальный край
#define WP_CUR_SIZE_V  6       // вертикальный край
#define WP_CUR_SIZE_D1 7       // угол ↖↘
#define WP_CUR_SIZE_D2 8       // угол ↗↙
#define WP_CUR_COUNT   9

typedef struct {
    uint32_t type;           // WP_CURSOR
    int32_t  win;
    int32_t  shape;          // WP_CUR_*
} wp_cursor_t;

typedef struct {
    uint32_t type;           // WP_SET_TITLE
    int32_t  win;
    char     title[64];
} wp_title_t;

#define WP_MOUSE_DOWN 1
#define WP_MOUSE_UP   2
#define WP_MOUSE_MOVE 3      // движение над окном или с зажатой кнопкой (drag)
#define WP_MOUSE_LEAVE 4     // курсор ушёл с окна

typedef struct {
    uint32_t type;           // WP_EV_MOUSE
    int32_t  win;
    int32_t  kind;           // WP_MOUSE_*
    int32_t  x, y;           // в координатах клиентской области
    int32_t  buttons;        // бит 0 — левая, бит 1 — правая
} wp_mouse_t;

// Захват мыши, как pointer lock в браузере или SDL_SetRelativeMouseMode.
// Композитор даёт его только активному окну и снимает сам по Meta, при смене
// окна или его закрытии — программа узнаёт об этом из WP_EV_GRAB.
typedef struct {
    uint32_t type;           // WP_GRAB (клиент) или WP_EV_GRAB (композитор)
    int32_t  win;
    int32_t  on;
} wp_grab_t;

typedef struct {
    uint32_t type;           // WP_EV_MOTION
    int32_t  win;
    int32_t  dx, dy;         // пиксели мыши, вниз и вправо — плюс
    int32_t  buttons;        // бит 0 — левая, 1 — правая, 2 — средняя
} wp_motion_t;

#define WP_MOD_SHIFT 0x01
#define WP_MOD_CTRL  0x02
#define WP_MOD_ALT   0x04

typedef struct {
    uint32_t type;           // WP_EV_KEY
    int32_t  win;
    int32_t  scancode;       // PS/2 set 1; бит 0x80 — отпускание; 0xE0-клавиши: 0x100 | код
    int32_t  pressed;
    int32_t  mods;           // WP_MOD_*
    char     text[8];        // напечатанный символ UTF-8 с учётом раскладки и Shift ("" — не символ)
} wp_key_t;

typedef struct {
    uint32_t type;           // WP_EV_SCROLL
    int32_t  win;
    int32_t  x, y;
    int32_t  clicks;         // + вниз, - вверх
} wp_scroll_t;

typedef struct {
    uint32_t type;           // WP_EV_FOCUS
    int32_t  win;
    int32_t  focused;
} wp_focus_t;

#define WP_CMD_CREATE_FOLDER 1
#define WP_CMD_CREATE_FILE   2

// Единственный экземпляр программы (как activation в XDG): вместо второго окна
// плеера файловый менеджер просит композитор передать путь уже работающему.
#define WP_OPEN_PATH 160

typedef struct {
    uint32_t type;           // WP_OPEN
    char     app[24];        // кого ищем
    char     path[WP_OPEN_PATH];
} wp_open_t;

typedef struct {
    uint32_t type;           // WP_OPENED
    int32_t  found;          // 1 — путь отдан работающей программе, окно поднято
} wp_opened_t;

typedef struct {
    uint32_t type;           // WP_EV_OPEN
    int32_t  win;
    char     path[WP_OPEN_PATH];
} wp_openfile_t;

typedef struct {
    uint32_t type;           // WP_EV_COMMAND
    int32_t  win;
    int32_t  command;        // WP_CMD_*
} wp_command_t;

// Уведомления (как org.freedesktop.Notifications). Шапку -- значок и имя
// программы -- композитор берёт из исполняемого файла отправителя (ресурсы
// .prg), а не из сообщения: выдать себя за систему или чужую программу нельзя.
#define WP_URGENCY_LOW      0
#define WP_URGENCY_NORMAL   1
#define WP_URGENCY_CRITICAL 2      // не исчезает само
typedef struct {
    uint32_t type;           // WP_NOTIFY
    uint32_t id;             // выбирает программа (не 0): тот же id заменяет прежнее уведомление
    uint16_t urgency;        // WP_URGENCY_*
    uint16_t timeout_s;      // 0 -- по умолчанию (6 с)
    char     icon[24];       // крупный значок справа: имя из /sys/icons, "" -- значок программы
    char     title[64];
    char     body[120];
    char     actions[2][16]; // подписи кнопок, "" -- нет
} wp_notify_t;

typedef struct {
    uint32_t type;           // WP_NOTIFY_CLOSE
    uint32_t id;
} wp_notify_close_t;

typedef struct {
    uint32_t type;           // WP_EV_NOTIFY_ACTION
    uint32_t id;
    int32_t  action;         // 0/1 -- кнопка, -1 -- щелчок по самому уведомлению
} wp_notify_action_t;

// Скан-коды служебных клавиш (для WP_EV_KEY.scancode)
#define WP_KEY_ESC        0x01
#define WP_KEY_BACKSPACE  0x0E
#define WP_KEY_TAB        0x0F
#define WP_KEY_ENTER      0x1C
#define WP_KEY_SPACE      0x39
#define WP_KEY_UP         0x148
#define WP_KEY_DOWN       0x150
#define WP_KEY_LEFT       0x14B
#define WP_KEY_RIGHT      0x14D
#define WP_KEY_HOME       0x147
#define WP_KEY_END        0x14F
#define WP_KEY_DELETE     0x153
#define WP_KEY_PGUP       0x149
#define WP_KEY_PGDN       0x151

#endif
