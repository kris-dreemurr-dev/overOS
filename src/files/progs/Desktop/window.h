#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define MAX_WINDOWS 16

struct window;

typedef struct window {
    int  id;
    int  pid;           // > 0 — окно внешнего процесса (оконный сервер), 0 — встроенное окно DE
    int  x, y, w, h;
    char title[64];
    int  is_open;       // 1 = окно открыто, 0 = закрыто (нет на экране и в таскбаре)
    int  is_active;     // Активно ли сейчас
    int  is_minimized;  // Свернуто ли в таскбар
    int  has_icon;
    const uint16_t* icon;
    uint16_t icon_data[16];   // собственная копия иконки внешнего окна

    void (*render_content)(struct window* win);
    void (*on_click)(struct window* win, int local_x, int local_y);
    void (*on_key)(struct window* win, uint8_t scancode);

    // Внешние окна: локальная копия поверхности, которую рисует клиент
    uint32_t* surf;       // буфер-копия поверхности внешнего окна
    uint32_t  surf_cap;   // его размер в байтах
    uint32_t  surf_seq;   // последний принятый seq
} window_t;

void      window_manager_init(void);
window_t* window_create(const char* title, int x, int y, int w, int h,
                        const uint16_t* icon,
                        void (*render_cb)(window_t*),
                        void (*click_cb)(window_t*, int, int));

void      window_render_all(void);
void      window_sync_from_kernel(void);
int       window_handle_mouse(int mouse_x, int mouse_y, int left_pressed, int prev_left_pressed);
window_t* window_get_by_id(int id);
window_t* window_get_list(int* out_count);

// Делает окно активным и поднимает его поверх остальных
void      window_activate(window_t* w);
// Пересылает скан-код активному внешнему окну
void      window_post_key(uint8_t scancode);

#endif