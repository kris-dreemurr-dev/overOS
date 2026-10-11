#include "window.h"
#include "gfx.h"
#include "theme.h"
#include "icons.h"
#include "syscalls.h"

static window_t g_windows[MAX_WINDOWS];
static int      g_win_count = 0;
static int      g_z[MAX_WINDOWS];      // индексы слотов снизу вверх (порядок слоёв)

static window_t* g_dragging_win = 0;
static int       g_drag_off_x   = 0;
static int       g_drag_off_y   = 0;
static window_t* g_press_win    = 0;   // внешнее окно, в которое нажали ЛКМ (ждём отпускания)
static int       g_opened_total = 0;   // для каскадного размещения новых окон

void window_manager_init(void) {
    g_win_count = 0;
    g_dragging_win = 0;
    g_press_win = 0;
    g_opened_total = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        g_windows[i].id = 0;
        g_windows[i].pid = 0;
        g_windows[i].is_open = 0;
        g_windows[i].is_minimized = 1;
        g_windows[i].is_active = 0;
        g_windows[i].surf = 0;
        g_windows[i].surf_cap = 0;
        g_windows[i].surf_seq = 0;
        g_z[i] = i;
    }
}

// ---------------------------------------------------------------------------
// Слои
// ---------------------------------------------------------------------------
static void z_raise(int idx) {
    int pos = -1;
    for (int i = 0; i < g_win_count; i++) if (g_z[i] == idx) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < g_win_count - 1; i++) g_z[i] = g_z[i + 1];
    g_z[g_win_count - 1] = idx;
}

void window_activate(window_t* w) {
    if (!w) return;
    int idx = (int)(w - g_windows);
    for (int k = 0; k < g_win_count; k++) g_windows[k].is_active = 0;
    w->is_active = 1;
    z_raise(idx);
}

// Свободный слот: сначала переиспользуем освобождённый, иначе добавляем новый
static window_t* slot_alloc(void) {
    for (int j = 0; j < g_win_count; j++) {
        if (g_windows[j].id == 0 && g_windows[j].pid == 0 && !g_windows[j].is_open)
            return &g_windows[j];
    }
    if (g_win_count >= MAX_WINDOWS) return 0;
    g_z[g_win_count] = g_win_count;
    return &g_windows[g_win_count++];
}

// ---------------------------------------------------------------------------
// Встроенные окна DE (osver, диалоги)
// ---------------------------------------------------------------------------
window_t* window_create(const char* title, int x, int y, int w, int h,
                        const uint16_t* icon,
                        void (*render_cb)(window_t*),
                        void (*click_cb)(window_t*, int, int)) {
    window_t* win = slot_alloc();
    if (!win) return 0;

    win->id = (int)(win - g_windows) + 1;
    win->pid = 0;
    win->x = x;
    win->y = y;
    win->w = (w > 0) ? w : 320;
    win->h = (h > 0) ? h : 240;
    win->is_open = 0;
    win->is_minimized = 0;
    win->is_active = 0;

    int p = 0;
    while (title[p] && p < 63) { win->title[p] = title[p]; p++; }
    win->title[p] = '\0';

    if (icon) {
        win->has_icon = 1;
        win->icon = icon;
    } else {
        win->has_icon = 0;
        win->icon = icon_placeholder;
    }

    win->render_content = render_cb;
    win->on_click = click_cb;
    win->on_key = 0;
    return win;
}

window_t* window_get_by_id(int id) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == id) return &g_windows[i];
    }
    return 0;
}

window_t* window_get_list(int* out_count) {
    if (out_count) *out_count = g_win_count;
    return g_windows;
}

// ---------------------------------------------------------------------------
// Отрисовка (снизу вверх по порядку слоёв)
// ---------------------------------------------------------------------------
void window_render_all(void) {
    for (int zi = 0; zi < g_win_count; zi++) {
        window_t* w = &g_windows[g_z[zi]];
        if (!w->is_open || w->is_minimized) continue;

        // Рамка окна Breeze
        gfx_draw_rect(w->x - 1, w->y - 1, w->w + 2, w->h + TITLE_BAR_HEIGHT + 2, COLOR_WIN_BORDER);

        // Шапка (заголовок)
        uint32_t t_color = w->is_active ? COLOR_WIN_TITLE_ACT : COLOR_WIN_TITLE_IN;
        gfx_draw_rect(w->x, w->y, w->w, TITLE_BAR_HEIGHT, t_color);

        // Иконка
        const uint16_t* ico = (w->has_icon && w->icon) ? w->icon : icon_placeholder;
        gfx_draw_mono_icon(w->x + 6, w->y + 5, ico, COLOR_TEXT_MAIN, 0, 0);

        // Текст заголовка
        gfx_draw_string(w->x + 28, w->y + 5, w->title, COLOR_TEXT_MAIN);

        // Кнопка [X]
        gfx_draw_rect(w->x + w->w - 22, w->y + 5, 16, 16, COLOR_CLOSE_BTN);
        gfx_draw_string(w->x + w->w - 18, w->y + 5, "X", COLOR_TEXT_MAIN);

        // Клиентская область окна
        gfx_draw_rect(w->x, w->y + TITLE_BAR_HEIGHT, w->w, w->h, COLOR_WIN_BG);

        if (w->render_content) {
            // Встроенное окно: рисует само
            w->render_content(w);
        } else if (w->pid > 0 && w->surf) {
            // Внешнее окно: забираем свежий кадр клиента (если он изменился) и блитим
            sys_win_read(w->id, w->surf, &w->surf_seq);
            gfx_blit(w->x, w->y + TITLE_BAR_HEIGHT, w->w, w->h, w->surf);
        }
    }
}

// ---------------------------------------------------------------------------
// Мышь: перетаскивание, закрытие, клики, пересылка событий внешним окнам
// ---------------------------------------------------------------------------
int window_handle_mouse(int mx, int my, int left_pressed, int prev_left_pressed) {
    int new_click = (left_pressed && !prev_left_pressed);

    // 0. Отпускание кнопки над внешним окном — сообщаем клиенту
    if (g_press_win && !left_pressed) {
        win_event_t e = { WEV_MOUSE_UP,
                          mx - g_press_win->x,
                          my - (g_press_win->y + TITLE_BAR_HEIGHT), 0, 0 };
        sys_win_push_event(g_press_win->id, &e);
        g_press_win = 0;
    }

    // 1. Если окно уже захвачено мышью — плавно передвигаем его
    if (g_dragging_win) {
        if (left_pressed) {
            g_dragging_win->x = mx - g_drag_off_x;
            g_dragging_win->y = my - g_drag_off_y;

            // Ограничение по экрану
            if (g_dragging_win->y < 0) g_dragging_win->y = 0;
            if (g_dragging_win->y > (int)g_screen_h - TASKBAR_HEIGHT - TITLE_BAR_HEIGHT)
                g_dragging_win->y = (int)g_screen_h - TASKBAR_HEIGHT - TITLE_BAR_HEIGHT;
            return 1;
        } else {
            g_dragging_win = 0; // Кнопку отпустили
        }
    }

    // 2. Обработка нового клика ЛКМ: идём по окнам сверху вниз (по порядку слоёв g_z)
    if (new_click) {
        for (int zi = g_win_count - 1; zi >= 0; zi--) {
            window_t* w = &g_windows[g_z[zi]];
            if (!w->is_open || w->is_minimized) continue;

            // Клик по крестику [X]
            if (mx >= w->x + w->w - 22 && mx <= w->x + w->w - 6 &&
                my >= w->y + 5 && my <= w->y + 21) {

                // Завершаем процесс окна (kernel/tty1/desktop не трогаем)
                if (w->pid > 2) {
                    sys_kill(w->pid, 9);
                }

                w->is_open = 0;
                w->is_minimized = 0;
                w->is_active = 0;
                return 1;
            }

            // Захват за шапку заголовка
            if (mx >= w->x && mx <= w->x + w->w &&
                my >= w->y && my <= w->y + TITLE_BAR_HEIGHT) {
                window_activate(w);          // активное окно + наверх по слоям

                g_dragging_win = w;
                g_drag_off_x = mx - w->x;
                g_drag_off_y = my - w->y;
                return 1;
            }

            // Клик внутри тела окна
            if (mx >= w->x && mx <= w->x + w->w &&
                my >= w->y + TITLE_BAR_HEIGHT && my <= w->y + TITLE_BAR_HEIGHT + w->h) {
                window_activate(w);

                if (w->on_click) {
                    w->on_click(w, mx - w->x, my - (w->y + TITLE_BAR_HEIGHT));
                } else if (w->pid > 0) {
                    win_event_t e = { WEV_MOUSE_DOWN,
                                      mx - w->x, my - (w->y + TITLE_BAR_HEIGHT), 1, 0 };
                    sys_win_push_event(w->id, &e);
                    g_press_win = w;
                }
                return 1;
            }
        }
    }

    return 0;
}

// Клавиатура: активному внешнему окну
void window_post_key(uint8_t sc) {
    for (int i = 0; i < g_win_count; i++) {
        window_t* w = &g_windows[i];
        if (w->is_open && !w->is_minimized && w->is_active && w->pid > 0 && !w->on_click) {
            win_event_t e = { WEV_KEY, 0, 0, 0, sc };
            sys_win_push_event(w->id, &e);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Синхронизация с реестром окон ядра
// ---------------------------------------------------------------------------
void window_sync_from_kernel(void) {
    kernel_window_t k_wins[MAX_SYS_WINDOWS];
    int count = sys_get_windows(k_wins, MAX_SYS_WINDOWS);
    if (count < 0) count = 0;

    // 1. Освобождаем слоты внешних окон, которых больше нет в ядре
    for (int j = 0; j < g_win_count; j++) {
        window_t* w = &g_windows[j];
        if (w->pid <= 0) continue;               // встроенное окно
        int alive = 0;
        for (int i = 0; i < count; i++)
            if (k_wins[i].id == w->id) { alive = 1; break; }
        if (!alive) {
            if (g_dragging_win == w) g_dragging_win = 0;
            if (g_press_win == w) g_press_win = 0;
            w->is_open = 0; w->is_active = 0; w->pid = 0; w->id = 0;
        }
    }

    // 2. Добавляем новые (сравниваем id ТОЛЬКО среди внешних окон)
    for (int i = 0; i < count; i++) {
        int exists = 0;
        for (int j = 0; j < g_win_count; j++)
            if (g_windows[j].pid > 0 && g_windows[j].id == k_wins[i].id) { exists = 1; break; }
        if (exists) continue;

        window_t* w = slot_alloc();
        if (!w) continue;

        w->id  = k_wins[i].id;
        w->pid = k_wins[i].pid > 0 ? k_wins[i].pid : 1;   // pid > 0 — признак внешнего окна
        w->w = (k_wins[i].w > 0) ? k_wins[i].w : 320;
        w->h = (k_wins[i].h > 0) ? k_wins[i].h : 240;

        // Каскад: ядерные x/y растут с каждым id и уезжают за экран — считаем свои
        int step = g_opened_total++ % 8;
        int nx = 60 + step * 30;
        int ny = 40 + step * 26;
        if (nx + w->w > (int)g_screen_w) nx = (int)g_screen_w - w->w;
        if (ny + w->h + TITLE_BAR_HEIGHT > (int)g_screen_h - TASKBAR_HEIGHT)
            ny = (int)g_screen_h - TASKBAR_HEIGHT - TITLE_BAR_HEIGHT - w->h;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        w->x = nx;
        w->y = ny;

        w->is_open = 1;
        w->is_minimized = 0;

        int p = 0;
        while (k_wins[i].title[p] && p < 63) { w->title[p] = k_wins[i].title[p]; p++; }
        w->title[p] = '\0';

        w->has_icon = k_wins[i].has_icon;
        if (w->has_icon) {
            for (int k = 0; k < 16; k++) w->icon_data[k] = k_wins[i].icon[k];
            w->icon = w->icon_data;
        } else {
            w->icon = icon_placeholder;
        }

        w->render_content = 0;      // внешний клиент: содержимое приходит через поверхность
        w->on_click = 0;
        w->on_key = 0;

        // Локальный буфер под кадр клиента (куча только растёт — слоты переиспользуем)
        uint32_t need = (uint32_t)w->w * (uint32_t)w->h * 4;
        if (w->surf_cap < need) {
            void* b = sys_sbrk((int64_t)need);
            if (b == (void*)(uintptr_t)-1) { w->surf = 0; w->surf_cap = 0; }
            else { w->surf = (uint32_t*)b; w->surf_cap = need; }
        }
        w->surf_seq = 0xFFFFFFFFu;  // принудительно прочитать первый кадр

        window_activate(w);
    }
}