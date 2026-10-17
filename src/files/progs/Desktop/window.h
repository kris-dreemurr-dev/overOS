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
    char app[24];       // класс программы из WP_CREATE ("ovplayer"): для единственного экземпляра
    char exe[96];       // исполняемый файл владельца: из него значок и имя программы
    int  is_open;       // 1 = окно открыто, 0 = закрыто (нет на экране и в таскбаре)
    int  is_active;     // Активно ли сейчас
    int  is_minimized;  // Свернуто ли в таскбар
    int  has_icon;
    const uint16_t* icon;
    uint16_t icon_data[16];   // собственная копия иконки внешнего окна

    void (*render_content)(struct window* win);
    void (*on_click)(struct window* win, int local_x, int local_y);
    void (*on_key)(struct window* win, uint8_t scancode);

    // Внешние окна: буфер кадра клиента в разделяемой памяти (отображён к нам)
    uint32_t* surf;
    uint32_t  surf_cap;   // его размер в байтах
    uint32_t  surf_off;   // с какого пикселя лежит показываемый кадр (двойная буферизация)

    // Окно оконного протокола (IPC + разделяемая память, src/lib/wproto.h)
    int       proto;          // 1 — окно клиента (libui/wproto), 0 — встроенное окно desktop
    int       shm_id;         // буфер клиента, отображён к нам: surf указывает прямо в него
    int       committed;      // клиент прислал WP_COMMIT — после вывода ответить WP_FRAME_DONE
    uint32_t  flags;          // WP_FLAG_*
    int       cursor;         // форма курсора внутри окна (WP_CUR_*, просит клиент)
    int       closing;        // нажали [X]: ждём, пока клиент закроется сам
    uint64_t  close_deadline; // ... не закрылся к этому моменту — завершаем принудительно

    // Разворачивание: буфер кадра принадлежит клиенту, поэтому геометрия меняется
    // не сразу, а когда клиент пришлёт новый буфер (WP_RESIZE)
    int       maximized;
    int       want_w, want_h;         // размер, который мы попросили (0 — не ждём)
    int       want_x, want_y;         // куда поставить окно, когда буфер приедет
    int       want_max;               // каким будет maximized, когда приедет новый буфер
    int       rest_x, rest_y;         // геометрия до разворачивания
    int       rest_w, rest_h;
} window_t;

void      window_manager_init(void);
window_t* window_create(const char* title, int x, int y, int w, int h,
                        const uint16_t* icon,
                        void (*render_cb)(window_t*),
                        void (*click_cb)(window_t*, int, int));

void      window_render_all(int mouse_x, int mouse_y);
int       window_blit_committed(int* y0, int* y1);
int       window_covers_desktop(void);
void      window_set_grab(window_t* w, int on);
int       window_grab_active(void);
void      window_grab_release(void);
void      window_grab_feed(int* cx, int* cy);   // смещения захватившему окну, курсор — в его центр   // развёрнутое окно закрывает обои целиком   // быстрый кадр: только клиентские области
void      window_toggle_max(window_t* w);     // развернуть/вернуть окно
void      window_request_close(window_t* w);  // попросить клиента закрыться ([X], меню панели)
void      window_draw_resize_hint(void);     // контур окна, пока тянут за край
int       window_btn_at(const window_t* w, int mx, int my);   // кнопка шапки под точкой
void      window_apply_resize(window_t* w, int shm_id, int nw, int nh);   // клиент прислал новый буфер
int       window_handle_mouse(int mouse_x, int mouse_y, int left_pressed, int prev_left_pressed);
window_t* window_get_by_id(int id);
window_t* window_get_list(int* out_count);

// Делает окно активным и поднимает его поверх остальных
void      window_activate(window_t* w);
// Пересылает скан-код активному внешнему окну
void      window_post_key(uint8_t scancode);
extern int g_caps_on;                       // Caps Lock (ведётся в window_post_key)
// Пересылает щелчки колеса окну под курсором (WP_EV_SCROLL)
void      window_post_scroll(int mx, int my, int clicks);
// Окно под точкой (сверху вниз по слоям), 0 — пусто
window_t* window_at(int x, int y);
// Нужна ли полная перерисовка при движении курсора в эту точку (см. window.c)
int window_hover_dirty(int mx, int my);
// Команда окну (WP_EV_COMMAND), например "создать папку" из контекстного меню
void      window_send_command(int win_id, int wp_command);
// Свободный слот под новое окно (wserver.c)
window_t* window_slot_alloc(void);
void      window_place_cascade(window_t* w);
// Какой курсор показывать в этой точке экрана (WP_CUR_*)
int       window_cursor_at(int mx, int my);
void      window_release(window_t* w);

#endif