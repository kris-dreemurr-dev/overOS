#ifndef WIN_PROTO_H
#define WIN_PROTO_H

#include <stdint.h>

#define MAX_SYS_WINDOWS 16
#define DEFAULT_WIN_SIZE 320 // Стандартный квадратный размер по умолчанию

// Структура, которую приложение передает в ядро при регистрации
typedef struct {
    const char*     title;  // ОБЯЗАТЕЛЬНО: название окна
    int             w;      // 0 или < 0 -> стандартный квадрат 320x320
    int             h;      // 0 или < 0 -> стандартный квадрат 320x320
    const uint16_t* icon;   // Маска 16x16 (NULL -> плейсхолдер)
} __attribute__((packed)) win_desc_t;

// Структура окна, хранимая в ядре и читаемая DE
typedef struct {
    int      id;
    int      pid;
    char     title[64];
    int      x, y;
    int      w, h;
    int      has_icon;
    uint16_t icon[16];      // Монохромная маска 16x16
    int      is_minimized;
    int      is_active;
} __attribute__((packed)) kernel_window_t;

#endif