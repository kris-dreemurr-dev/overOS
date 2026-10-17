#include "window.h"
#include "gfx.h"
#include "theme.h"
#include "icons.h"
#include "syscalls.h"
#include "settings.h"
#include "notifier.h"
#include "ovos.h"
#include "wproto.h"
#include "common/keymap.h"

int             g_caps_on = 0;          // Caps Lock включён (индикатор в трее)

static window_t g_windows[MAX_WINDOWS];
static int      g_win_count = 0;
static int      g_z[MAX_WINDOWS];      // индексы слотов снизу вверх (порядок слоёв)

static window_t* g_dragging_win = 0;
static int       g_drag_off_x   = 0;
static int       g_drag_off_y   = 0;
static window_t* g_press_win    = 0;   // внешнее окно, в которое нажали ЛКМ (ждём отпускания)
static window_t* g_grab_win     = 0;   // окно, захватившее мышь (WP_GRAB)
static int       g_grab_buttons = 0;
static int       g_opened_total = 0;   // для каскадного размещения новых окон
static window_t* g_hover_win    = 0;   // окно протокола под курсором (для WP_MOUSE_LEAVE)
static uint64_t  g_title_click_ms = 0;  // двойной щелчок по шапке = развернуть
// Изменение размера мышью за край окна (контур тянется, размер применяется по отпусканию)
static window_t* g_resize_win = 0;
static int       g_resize_edge = 0;      // WIN_EDGE_*
static int       g_resize_x, g_resize_y, g_resize_w, g_resize_h;
static int       g_resize_mx, g_resize_my;   // где была мышь в начале
static int       g_resize_ox, g_resize_oy, g_resize_ow, g_resize_oh;
static int       g_title_click_win = -1;
static int       g_kbd_shift = 0, g_kbd_ctrl = 0, g_kbd_alt = 0, g_kbd_e0 = 0;

// Событие клиенту оконного протокола (очередь полна — событие теряется, как и в X11/Wayland
// при зависшем клиенте; композитор из-за клиента не блокируется)
static void proto_send(window_t* w, const void* msg, uint32_t len) {
    if (w && w->proto && w->pid > 0) ov_send(w->pid, msg, len);
}

static void proto_mouse(window_t* w, int kind, int mx, int my, int buttons) {
    wp_mouse_t e = { WP_EV_MOUSE, w->id, kind, mx - w->x, my - (w->y + TITLE_BAR_HEIGHT), buttons };
    proto_send(w, &e, sizeof(e));
}

static void proto_focus(window_t* w, int focused) {
    wp_focus_t e = { WP_EV_FOCUS, w->id, focused };
    proto_send(w, &e, sizeof(e));
}

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
        g_windows[i].surf_off = 0;
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
    if (g_grab_win && g_grab_win != w) window_set_grab(g_grab_win, 0);
    int idx = (int)(w - g_windows);
    for (int k = 0; k < g_win_count; k++) {
        if (g_windows[k].is_active && &g_windows[k] != w) proto_focus(&g_windows[k], 0);
        g_windows[k].is_active = 0;
    }
    if (!w->is_active) proto_focus(w, 1);
    w->is_active = 1;
    z_raise(idx);
}

// Свободный слот: сначала переиспользуем освобождённый, иначе добавляем новый
static window_t* slot_alloc(void) {
    window_t* w = 0;
    for (int j = 0; j < g_win_count && !w; j++) {
        if (g_windows[j].id == 0 && g_windows[j].pid == 0 && !g_windows[j].is_open)
            w = &g_windows[j];
    }
    if (!w) {
        if (g_win_count >= MAX_WINDOWS) return 0;
        g_z[g_win_count] = g_win_count;
        w = &g_windows[g_win_count++];
    }
    // Слот — с чистого листа. Раньше сбрасывались только несколько полей, и новое
    // окно наследовало от умершего в этом слоте maximized/want_*/rest_*: окно
    // игры, упавшей развёрнутой, открывалось с прямыми углами и не тянулось.
    ov_memset(w, 0, sizeof(*w));
    return w;
}

window_t* window_slot_alloc(void) { return slot_alloc(); }

// Каскад для нового окна: каждое следующее чуть ниже и правее, не вылезая за экран
void window_place_cascade(window_t* w) {
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
}

// Окно больше не нужно: слот освобождается, буфер клиента отключается от нас
void window_release(window_t* w) {
    if (!w) return;
    if (g_dragging_win == w) g_dragging_win = 0;
    if (g_press_win == w) g_press_win = 0;
    if (g_hover_win == w) g_hover_win = 0;
    if (g_grab_win == w) g_grab_win = 0;      // окна нет — и сообщать некому
    if (w->proto) {
        if (w->shm_id > 0) ov_shm_unmap(w->shm_id);
        w->surf = 0;
        w->surf_cap = 0;
        w->surf_off = 0;
    }
    w->is_open = 0; w->is_active = 0; w->pid = 0; w->id = 0;
    w->proto = 0; w->shm_id = 0; w->committed = 0; w->closing = 0;
}

window_t* window_at(int x, int y) {
    for (int zi = g_win_count - 1; zi >= 0; zi--) {
        window_t* w = &g_windows[g_z[zi]];
        if (!w->is_open || w->is_minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + TITLE_BAR_HEIGHT + w->h) return w;
    }
    return 0;
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

// --- Изменение размера за край ----------------------------------------------
#define WIN_EDGE_L 1
#define WIN_EDGE_R 2
#define WIN_EDGE_T 4
#define WIN_EDGE_B 8
#define WIN_RESIZE_GRIP 5        // полоса захвата по краю окна, пикселей
#define WIN_MIN_W 220
#define WIN_MIN_H 120

// Какой край окна под курсором (0 — не край). Тянуть можно только окна,
// которые умеют менять размер, и только в обычном (не развёрнутом) виде.
static int window_edge_at(const window_t* w, int mx, int my) {
    if (!w->proto || !(w->flags & WP_FLAG_RESIZABLE) || w->maximized) return 0;
    int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h + TITLE_BAR_HEIGHT;
    if (mx < x0 - WIN_RESIZE_GRIP || mx > x1 + WIN_RESIZE_GRIP ||
        my < y0 - WIN_RESIZE_GRIP || my > y1 + WIN_RESIZE_GRIP) return 0;
    int e = 0;
    if (mx <= x0 + WIN_RESIZE_GRIP) e |= WIN_EDGE_L;
    if (mx >= x1 - WIN_RESIZE_GRIP) e |= WIN_EDGE_R;
    if (my <= y0 + WIN_RESIZE_GRIP) e |= WIN_EDGE_T;
    if (my >= y1 - WIN_RESIZE_GRIP) e |= WIN_EDGE_B;
    return e;
}

// Край окна -> форма курсора
static int edge_cursor(int e) {
    if ((e & WIN_EDGE_L) && (e & WIN_EDGE_T)) return WP_CUR_SIZE_D1;
    if ((e & WIN_EDGE_R) && (e & WIN_EDGE_B)) return WP_CUR_SIZE_D1;
    if ((e & WIN_EDGE_R) && (e & WIN_EDGE_T)) return WP_CUR_SIZE_D2;
    if ((e & WIN_EDGE_L) && (e & WIN_EDGE_B)) return WP_CUR_SIZE_D2;
    if (e & (WIN_EDGE_L | WIN_EDGE_R)) return WP_CUR_SIZE_H;
    if (e & (WIN_EDGE_T | WIN_EDGE_B)) return WP_CUR_SIZE_V;
    return WP_CUR_ARROW;
}

// Меняет ли движение курсора в эту точку то, что рисует сам композитор?
// Шапка окна (подсветка кнопок) и края (форма курсора) — да; клиентская часть
// окна и пустой рабочий стол — нет, там достаточно подвинуть сам курсор.
int window_hover_dirty(int mx, int my) {
    if (g_dragging_win || g_resize_edge) return 1;

    int count = 0;
    window_t* list = window_get_list(&count);
    for (int layer = count - 1; layer >= 0; layer--) {
        window_t* w = &list[g_z[layer]];
        if (!w->is_open || w->is_minimized) continue;
        if (window_edge_at(w, mx, my)) return 1;
        int top = w->y, cy = w->y + TITLE_BAR_HEIGHT;
        if (mx < w->x || mx >= w->x + w->w || my < top || my >= cy + w->h) continue;
        return (my < cy);                       // шапка — перерисовать, внутри окна — нет
    }
    return 0;
}

// Какой курсор показывать в этой точке экрана.
//
// Края и углы окон — забота композитора: он один знает рамку. Внутри окна форму
// выбирает сама программа (WP_CURSOR, поле w->cursor), а пока окно тянут за
// край — курсор не меняется, иначе он прыгал бы при каждом движении мыши.
int window_cursor_at(int mx, int my) {
    if (window_grab_active()) return GFX_CUR_HIDDEN;
    if (g_resize_edge) return edge_cursor(g_resize_edge);
    if (g_dragging_win) return WP_CUR_MOVE;

    int count = 0;
    window_t* list = window_get_list(&count);
    for (int layer = count - 1; layer >= 0; layer--) {
        window_t* w = &list[g_z[layer]];
        if (!w->is_open || w->is_minimized) continue;
        int e = window_edge_at(w, mx, my);
        if (e) return edge_cursor(e);
        int top = w->y, cy = w->y + TITLE_BAR_HEIGHT;
        if (mx < w->x || mx >= w->x + w->w || my < top || my >= cy + w->h) continue;
        if (my < cy) return WP_CUR_ARROW;            // шапка — обычная стрелка
        return w->cursor;
    }
    return WP_CUR_ARROW;
}

// --- Кнопки шапки -----------------------------------------------------------
// Как в Breeze: справа кнопки «свернуть», «развернуть» (только у окон, умеющих
// менять размер) и «закрыть»; под курсором у кнопки появляется подложка.
#define WIN_BTN_W 28

static int win_btn_count(const window_t* w) { return (w->flags & WP_FLAG_RESIZABLE) ? 3 : 2; }

// Левый край i-й кнопки (0 — самая левая из них)
static int win_btn_x(const window_t* w, int i) {
    return w->x + w->w - (win_btn_count(w) - i) * WIN_BTN_W - 4;
}

// Какая кнопка под точкой: 0 свернуть, 1 развернуть, 2 закрыть, -1 — мимо
int window_btn_at(const window_t* w, int mx, int my) {
    if (my < w->y + 3 || my >= w->y + TITLE_BAR_HEIGHT - 3) return -1;
    int n = win_btn_count(w);
    for (int i = 0; i < n; i++) {
        int bx = win_btn_x(w, i);
        if (mx >= bx && mx < bx + WIN_BTN_W) return (n == 2 && i == 1) ? 2 : i;
    }
    return -1;
}

static void draw_win_buttons(window_t* w, int mx, int my) {
    int n = win_btn_count(w);
    int hot = window_btn_at(w, mx, my);
    uint32_t fg = w->is_active ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED;
    for (int i = 0; i < n; i++) {
        int kind = (n == 2 && i == 1) ? 2 : i;        // без «развернуть» вторая кнопка — закрыть
        int bx = win_btn_x(w, i), by = w->y + 3;
        int cx = bx + WIN_BTN_W / 2, cy = by + (TITLE_BAR_HEIGHT - 6) / 2;
        if (hot == kind) {
            gfx_round_rect(bx + 2, by, WIN_BTN_W - 4, TITLE_BAR_HEIGHT - 6, 4, GFX_CORNER_ALL,
                           kind == 2 ? COLOR_CLOSE_BTN : COLOR_WIN_BTN_HOVER);
            fg = COLOR_TEXT_MAIN;
        } else {
            fg = w->is_active ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED;
        }
        if (kind == 0) {                                   // свернуть
            gfx_draw_rect(cx - 5, cy + 3, 10, 1, fg);
        } else if (kind == 1) {                            // развернуть / вернуть
            if (w->maximized) {
                gfx_draw_rect(cx - 5, cy - 2, 8, 1, fg);
                gfx_draw_rect(cx - 5, cy - 2, 1, 7, fg);
                gfx_draw_rect(cx - 5, cy + 5, 8, 1, fg);
                gfx_draw_rect(cx + 2, cy - 2, 1, 8, fg);
            } else {
                gfx_draw_rect(cx - 5, cy - 4, 10, 1, fg);
                gfx_draw_rect(cx - 5, cy + 4, 10, 1, fg);
                gfx_draw_rect(cx - 5, cy - 4, 1, 9, fg);
                gfx_draw_rect(cx + 4, cy - 4, 1, 9, fg);
            }
        } else {                                           // закрыть
            gfx_line(cx - 4, cy - 4, cx + 4, cy + 4, fg);
            gfx_line(cx + 4, cy - 4, cx - 4, cy + 4, fg);
        }
    }
}

// ---------------------------------------------------------------------------
// Захват мыши (WP_GRAB): игра получает смещения, курсор спрятан. Даётся только
// активному окну; снимается по просьбе окна, по Meta, при смене или закрытии
// окна. Окно узнаёт о снятии из WP_EV_GRAB.
// ---------------------------------------------------------------------------
void window_set_grab(window_t* w, int on) {
    if (!w || !w->proto) return;
    if (on) {
        if (!w->is_active || w->is_minimized) on = 0;   // захват только у окна в фокусе
        else {
            if (g_grab_win && g_grab_win != w) window_set_grab(g_grab_win, 0);
            g_grab_win = w;
            g_grab_buttons = 0;
            int dx, dy; uint8_t b;
            sys_get_mouse_delta(&dx, &dy, &b);           // старое накопленное — не игре
        }
    } else if (g_grab_win == w) {
        g_grab_win = 0;
    } else {
        return;
    }
    wp_grab_t e = { WP_EV_GRAB, w->id, on };
    proto_send(w, &e, sizeof(e));
}

int window_grab_active(void) {
    window_t* w = g_grab_win;
    if (!w) return 0;
    if (!w->is_open || !w->proto || !w->is_active || w->is_minimized) {
        window_set_grab(w, 0);
        return 0;
    }
    return 1;
}

void window_grab_release(void) { if (g_grab_win) window_set_grab(g_grab_win, 0); }

// Раз за проход цикла: смещение с прошлого раза и кнопки — окну. Логический
// курсор стоит в центре окна: колесо и наведение не уезжают в соседей.
void window_grab_feed(int* cx, int* cy) {
    window_t* w = g_grab_win;
    int dx, dy; uint8_t b;
    sys_get_mouse_delta(&dx, &dy, &b);
    int btn = b & 7;
    if (dx || dy || btn != g_grab_buttons) {
        wp_motion_t m = { WP_EV_MOTION, w->id, dx, dy, btn };
        proto_send(w, &m, sizeof(m));
        g_grab_buttons = btn;
    }
    *cx = w->x + w->w / 2;
    *cy = w->y + TITLE_BAR_HEIGHT + w->h / 2;
}

// ---------------------------------------------------------------------------
// Отрисовка (снизу вверх по порядку слоёв)
// ---------------------------------------------------------------------------
// Нижний слой, с которого есть смысл рисовать: развёрнутое окно закрывает
// весь стол, и всё, что под ним, рисовать незачем — ни обои, ни окна
static int first_visible_layer(void) {
    for (int zi = g_win_count - 1; zi >= 0; zi--) {
        const window_t* w = &g_windows[g_z[zi]];
        if (!w->is_open || w->is_minimized || !w->maximized) continue;
        if (w->x <= 0 && w->y <= 0 && w->x + w->w >= (int)g_screen_w &&
            w->y + TITLE_BAR_HEIGHT + w->h >= (int)g_screen_h - TASKBAR_HEIGHT) return zi;
    }
    return -1;
}

int window_covers_desktop(void) { return first_visible_layer() >= 0; }

void window_render_all(int mouse_x, int mouse_y) {
    int first = first_visible_layer();
    for (int zi = first < 0 ? 0 : first; zi < g_win_count; zi++) {
        window_t* w = &g_windows[g_z[zi]];
        if (!w->is_open || w->is_minimized) continue;

        int full_h = w->h + TITLE_BAR_HEIGHT;
        // Развёрнутое окно занимает весь экран, и скругления ему только мешают —
        // как в любой среде, в этом режиме углы прямые
        int rad = w->maximized ? 0 : WIN_CORNER_R;

        // Рамка и шапка: скругление со всех сторон
        gfx_round_rect(w->x - 1, w->y - 1, w->w + 2, full_h + 2, rad ? rad + 1 : 0,
                       GFX_CORNER_ALL, COLOR_WIN_BORDER);
        gfx_round_rect(w->x, w->y, w->w, TITLE_BAR_HEIGHT, rad, GFX_CORNER_TOP,
                       w->is_active ? COLOR_WIN_TITLE_ACT : COLOR_WIN_TITLE_IN);
        // Тонкая синяя полоса под шапкой — признак активного окна
        if (w->is_active) gfx_draw_rect(w->x, w->y + TITLE_BAR_HEIGHT - 1, w->w, 1, COLOR_ACCENT_BLUE);

        // Иконка слева, заголовок по центру (как в Breeze). Значок -- из
        // ресурсов исполняемого файла программы, иначе -- тот, что прислало окно.
        uint32_t ico_col = w->is_active ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED;
        const char* svg = w->exe[0] ? w->exe : 0;     // значок из ресурсов программы
        if (svg && gfx_icon(svg, 18) >= 0) {
            gfx_icon_draw(w->x + 7, w->y + (TITLE_BAR_HEIGHT - 18) / 2, svg, 18, ico_col);
        } else {
            const uint16_t* ico = (w->has_icon && w->icon) ? w->icon : icon_placeholder;
            gfx_draw_mono_icon(w->x + 8, w->y + (TITLE_BAR_HEIGHT - 16) / 2, ico, ico_col, 0, 0);
        }
        int tw = gfx_text_width(w->title);
        int tx = w->x + (w->w - tw) / 2;
        int t_left = w->x + 30;
        int t_right = win_btn_x(w, 0) - 8;
        if (tx < t_left) tx = t_left;
        if (tx + tw > t_right) tx = t_right - tw;
        if (tx < t_left) tx = t_left;
        gfx_draw_string(tx, w->y + (TITLE_BAR_HEIGHT - 16) / 2, w->title,
                        w->is_active ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);

        draw_win_buttons(w, mouse_x, mouse_y);

        // Клиентская область окна. Под кадром клиента фон не заливаем: кадр
        // закрывает ровно ту же скруглённую область, и заливка была бы
        // напрасной записью всей площади окна на каждом кадре.
        if (w->render_content || !(w->pid > 0 && w->surf))
            gfx_round_rect(w->x, w->y + TITLE_BAR_HEIGHT, w->w, w->h, rad, GFX_CORNER_BOTTOM, COLOR_WIN_BG);

        if (w->render_content) {
            // Встроенное окно: рисует само
            w->render_content(w);
        } else if (w->pid > 0 && w->surf) {
            // Внешнее окно: забираем свежий кадр клиента (если он изменился) и блитим
            // Кадр прямо из буфера клиента в разделяемой памяти (без копий через ядро)
            gfx_blit_round(w->x, w->y + TITLE_BAR_HEIGHT, w->w, w->h, w->surf + w->surf_off, rad, GFX_CORNER_BOTTOM);
        }
    }
}

// Быстрый путь кадра: сменилось только содержимое окон (пришли WP_COMMIT), а
// всё остальное на экране прежнее. Тогда незачем заново класть обои, шапки и
// панель — достаточно переложить клиентскую область этих окон в задний буфер.
// Годится, только если поверх области ничего не лежит: ни окно выше по слоям,
// ни панель, ни карточка уведомления. Возвращает 0, если хоть одно окно так
// обновить нельзя (тогда нужен полный кадр), иначе 1 и строки [*y0, *y1).
int window_blit_committed(int* y0, int* y1) {
    int lo = 1 << 30, hi = 0;
    if (g_resize_win) return 0;
    for (int pass = 0; pass < 2; pass++) {          // 0 — проверить всё, 1 — рисовать
        for (int zi = 0; zi < g_win_count; zi++) {
            window_t* w = &g_windows[g_z[zi]];
            if (!w->proto || !w->committed || !w->surf) continue;
            if (!w->is_open || w->is_minimized) continue;
            int cx = w->x, cy = w->y + TITLE_BAR_HEIGHT;
            if (pass == 0) {
                if (cy + w->h > (int)g_screen_h - TASKBAR_HEIGHT) return 0;
                if (notifier_overlaps(cx, cy, w->w, w->h)) return 0;
                for (int zj = zi + 1; zj < g_win_count; zj++) {
                    const window_t* o = &g_windows[g_z[zj]];
                    if (!o->is_open || o->is_minimized) continue;
                    int ox = o->x - 1, oy = o->y - 1;
                    int ow = o->w + 2, oh = o->h + TITLE_BAR_HEIGHT + 2;
                    if (ox < cx + w->w && cx < ox + ow && oy < cy + w->h && cy < oy + oh) return 0;
                }
                continue;
            }
            int rad = w->maximized ? 0 : WIN_CORNER_R;
            gfx_blit_round(cx, cy, w->w, w->h, w->surf + w->surf_off, rad, GFX_CORNER_BOTTOM);
            if (cy < lo) lo = cy;
            if (cy + w->h > hi) hi = cy + w->h;
        }
    }
    *y0 = (hi > lo) ? lo : 0;
    *y1 = (hi > lo) ? hi : 0;
    return 1;
}

// Пока тянут за край, само окно не трогаем: буфер кадра принадлежит клиенту, и
// пересоздавать его на каждое движение мыши дорого. Рисуем контур, как делали
// оконные менеджеры X11 в режиме wireframe, а размер применяем по отпусканию.
void window_draw_resize_hint(void) {
    if (!g_resize_win) return;
    int x = g_resize_x, y = g_resize_y;
    int w = g_resize_w, h = g_resize_h + TITLE_BAR_HEIGHT;
    gfx_draw_rect(x, y, w, 1, COLOR_ACCENT_BLUE);
    gfx_draw_rect(x, y + h - 1, w, 1, COLOR_ACCENT_BLUE);
    gfx_draw_rect(x, y, 1, h, COLOR_ACCENT_BLUE);
    gfx_draw_rect(x + w - 1, y, 1, h, COLOR_ACCENT_BLUE);
}

// Развернуть окно на весь рабочий стол и обратно. Размер буфера задаёт клиент,
// поэтому сразу меняем только «пожелание»: геометрия переключится, когда придёт
// новый буфер (window_apply_resize).
// Закрытие окна: как в Wayland/X11 просим клиента закрыться сам (он успеет
// сохранить данные), а если за 2 секунды не закрылся — его завершает wserver_tick
void window_request_close(window_t* w) {
    if (!w || !w->is_open) return;
    if (w->proto) {
        wp_simple_t e = { WP_EV_CLOSE, w->id };
        proto_send(w, &e, sizeof(e));
        w->closing = 1;
        w->close_deadline = ov_ms() + 2000;
    }
    w->is_open = 0;
    w->is_minimized = 0;
    w->is_active = 0;
}

void window_toggle_max(window_t* w) {
    if (!w || !w->proto || !(w->flags & WP_FLAG_RESIZABLE)) return;
    int nw, nh, nx, ny;
    if (w->maximized) {
        nx = w->rest_x; ny = w->rest_y; nw = w->rest_w; nh = w->rest_h;
    } else {
        w->rest_x = w->x; w->rest_y = w->y; w->rest_w = w->w; w->rest_h = w->h;
        nx = 0; ny = 0;
        nw = (int)g_screen_w;
        nh = (int)g_screen_h - TASKBAR_HEIGHT - TITLE_BAR_HEIGHT;
    }
    w->want_w = nw; w->want_h = nh;
    w->want_x = nx; w->want_y = ny;
    w->want_max = !w->maximized;
    wp_resize_t e = { WP_EV_RESIZE, w->id, nw, nh };
    proto_send(w, &e, sizeof(e));
}

// Клиент прислал буфер нового размера — переключаемся на него
void window_apply_resize(window_t* w, int shm_id, int nw, int nh) {
    if (!w || nw <= 0 || nh <= 0) return;
    uint64_t need = (uint64_t)nw * (uint64_t)nh * 4;
    uint64_t have = (uint64_t)ov_sc(111, (uint64_t)shm_id, 0, 0);
    if (have < need) return;                                   // буфер меньше окна — не берём
    uint32_t* surf = (uint32_t*)ov_shm_map(shm_id);
    if (!surf) return;
    if (w->surf && w->shm_id != shm_id) ov_shm_unmap(w->shm_id);
    w->shm_id = shm_id;
    w->surf = surf;
    w->surf_cap = (uint32_t)have;
    w->surf_off = 0;
    w->w = nw;
    w->h = nh;
    if (w->want_w == nw && w->want_h == nh) {
        w->x = w->want_x;
        w->y = w->want_y;
        // Развёрнутость задаёт тот, кто просил размер: кнопка «развернуть» её
        // меняет, а растягивание за край — нет. Раньше здесь стоял переворот
        // флага, и окно после любого растягивания считалось развёрнутым: углы
        // становились прямыми, а край больше не тянулся.
        w->maximized = w->want_max;
        w->want_w = w->want_h = 0;
    }
}

// ---------------------------------------------------------------------------
// Мышь: перетаскивание, закрытие, клики, пересылка событий внешним окнам
// ---------------------------------------------------------------------------
int window_handle_mouse(int mx, int my, int left_pressed, int prev_left_pressed) {
    int new_click = (left_pressed && !prev_left_pressed);

    // 0. Кнопка зажата над внешним окном и мышь двигается — клиенту нужен drag (ползунки)
    static int s_last_mx = -1, s_last_my = -1;
    int moved = (mx != s_last_mx || my != s_last_my);
    if (g_press_win && left_pressed && moved) {
        proto_mouse(g_press_win, WP_MOUSE_MOVE, mx, my, 1);
    }

    // Наведение без кнопки: окнам протокола — для подсветки кнопок под курсором
    if (!left_pressed && moved && !g_dragging_win) {
        window_t* under = window_at(mx, my);
        if (under && (!under->proto || my < under->y + TITLE_BAR_HEIGHT)) under = 0;   // встроенное окно / шапка
        if (g_hover_win && g_hover_win != under) proto_mouse(g_hover_win, WP_MOUSE_LEAVE, mx, my, 0);
        if (under) proto_mouse(under, WP_MOUSE_MOVE, mx, my, 0);
        g_hover_win = under;
    }
    s_last_mx = mx;
    s_last_my = my;

    // 1. Отпускание кнопки над внешним окном — сообщаем клиенту
    if (g_press_win && !left_pressed) {
        proto_mouse(g_press_win, WP_MOUSE_UP, mx, my, 0);
        g_press_win = 0;
    }

    // 0.5. Тянем край окна: двигаем только контур, настоящий размер — по отпусканию
    if (g_resize_win) {
        if (left_pressed) {
            int dx = mx - g_resize_mx, dy = my - g_resize_my;
            int nx = g_resize_ox, ny = g_resize_oy, nw = g_resize_ow, nh = g_resize_oh;
            if (g_resize_edge & WIN_EDGE_L) { nx += dx; nw -= dx; }
            if (g_resize_edge & WIN_EDGE_R) { nw += dx; }
            if (g_resize_edge & WIN_EDGE_T) { ny += dy; nh -= dy; }
            if (g_resize_edge & WIN_EDGE_B) { nh += dy; }
            if (nw < WIN_MIN_W) { if (g_resize_edge & WIN_EDGE_L) nx -= WIN_MIN_W - nw; nw = WIN_MIN_W; }
            if (nh < WIN_MIN_H) { if (g_resize_edge & WIN_EDGE_T) ny -= WIN_MIN_H - nh; nh = WIN_MIN_H; }
            if (ny < 0) { nh += ny; ny = 0; }
            g_resize_x = nx; g_resize_y = ny; g_resize_w = nw; g_resize_h = nh;
            return 1;
        }
        window_t* rw = g_resize_win;
        g_resize_win = 0;
        g_resize_edge = 0;              // иначе курсор так и остался бы стрелками размера
        if (g_resize_w != rw->w || g_resize_h != rw->h) {
            rw->want_w = g_resize_w; rw->want_h = g_resize_h;
            rw->want_x = g_resize_x; rw->want_y = g_resize_y;
            rw->want_max = 0;           // растянули за край — окно уже не «развёрнуто»
            wp_resize_t e = { WP_EV_RESIZE, rw->id, g_resize_w, g_resize_h };
            proto_send(rw, &e, sizeof(e));
        } else {
            rw->x = g_resize_x;
            rw->y = g_resize_y;
        }
        return 1;
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

            // Край окна: начинаем тянуть размер. Идём сверху вниз по слоям, так
            // что до нижнего окна очередь дойдёт, только если точка не на верхнем.
            // Кнопки шапки важнее: их верхняя кромка попадает в полосу захвата.
            int edge = window_edge_at(w, mx, my);
            if (edge && window_btn_at(w, mx, my) < 0) {
                window_activate(w);
                g_resize_win = w;
                g_resize_edge = edge;
                g_resize_mx = mx; g_resize_my = my;
                g_resize_ox = g_resize_x = w->x;
                g_resize_oy = g_resize_y = w->y;
                g_resize_ow = g_resize_w = w->w;
                g_resize_oh = g_resize_h = w->h;
                return 1;
            }

            // Кнопки шапки: свернуть / развернуть / закрыть
            int btn = window_btn_at(w, mx, my);
            if (btn == 0) {
                w->is_minimized = 1;
                w->is_active = 0;
                return 1;
            }
            if (btn == 1) {
                window_activate(w);
                window_toggle_max(w);
                return 1;
            }
            if (btn == 2) {
                window_request_close(w);
                return 1;
            }

            // Захват за шапку заголовка (развёрнутое окно не таскаем)
            if (mx >= w->x && mx <= w->x + w->w &&
                my >= w->y && my <= w->y + TITLE_BAR_HEIGHT) {
                window_activate(w);          // активное окно + наверх по слоям

                // Двойной щелчок по шапке разворачивает окно, как в любой среде
                uint64_t now = ov_ms();
                if (now - g_title_click_ms < 400 && g_title_click_win == w->id) {
                    g_title_click_ms = 0;
                    window_toggle_max(w);
                    return 1;
                }
                g_title_click_ms = now;
                g_title_click_win = w->id;

                if (!w->maximized) {
                    g_dragging_win = w;
                    g_drag_off_x = mx - w->x;
                    g_drag_off_y = my - w->y;
                }
                return 1;
            }

            // Клик внутри тела окна
            if (mx >= w->x && mx <= w->x + w->w &&
                my >= w->y + TITLE_BAR_HEIGHT && my <= w->y + TITLE_BAR_HEIGHT + w->h) {
                window_activate(w);

                if (w->on_click) {
                    w->on_click(w, mx - w->x, my - (w->y + TITLE_BAR_HEIGHT));
                } else if (w->proto) {
                    proto_mouse(w, WP_MOUSE_DOWN, mx, my, 1);
                    g_press_win = w;
                }
                return 1;
            }
        }
    }

    return 0;
}

// Колесо мыши: окну под курсором (если это внешнее окно)
void window_post_scroll(int mx, int my, int clicks) {
    for (int zi = g_win_count - 1; zi >= 0; zi--) {
        window_t* w = &g_windows[g_z[zi]];
        if (!w->is_open || w->is_minimized) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + TITLE_BAR_HEIGHT + w->h) {
            if (w->proto) {
                wp_scroll_t e = { WP_EV_SCROLL, w->id, mx - w->x, my - (w->y + TITLE_BAR_HEIGHT), clicks };
                proto_send(w, &e, sizeof(e));
            }
            return;
        }
    }
}

// Клавиатура: активному внешнему окну
void window_post_key(uint8_t sc) {
    // Модификаторы и префикс 0xE0 (стрелки и т.п.) — для окон протокола
    int e0 = g_kbd_e0;
    g_kbd_e0 = (sc == 0xE0);
    // Caps Lock переключается по нажатию (0x3A), отпускание (0xBA) игнорируем.
    // Состояние глобальное: по нему панель задач рисует индикатор, как в KDE.
    if (sc == 0x3A) g_caps_on = !g_caps_on;
    if (sc == 0x2A || sc == 0x36) g_kbd_shift = 1;
    if (sc == 0xAA || sc == 0xB6) g_kbd_shift = 0;
    if (sc == 0x1D) g_kbd_ctrl = 1;
    if (sc == 0x9D) g_kbd_ctrl = 0;
    if (sc == 0x38) g_kbd_alt = 1;
    if (sc == 0xB8) g_kbd_alt = 0;

    for (int i = 0; i < g_win_count; i++) {
        window_t* w = &g_windows[i];
        if (!(w->is_open && !w->is_minimized && w->is_active && w->proto)) continue;

        {
            if (sc == 0xE0) return;
            wp_key_t k;
            ov_memset(&k, 0, sizeof(k));
            k.type = WP_EV_KEY;
            k.win = w->id;
            k.pressed = !(sc & 0x80);
            k.scancode = (sc & 0x7F) | (e0 ? 0x100 : 0);
            k.mods = (g_kbd_shift ? WP_MOD_SHIFT : 0) | (g_kbd_ctrl ? WP_MOD_CTRL : 0) | (g_kbd_alt ? WP_MOD_ALT : 0);
            if (k.pressed && !e0 && !g_kbd_ctrl && !g_kbd_alt)
                km_translate(sc & 0x7F, g_kbd_shift, g_caps_on, g_layout_ru, k.text);
            proto_send(w, &k, sizeof(k));
        }
        return;
    }
}

void window_send_command(int win_id, int wp_command) {
    window_t* w = window_get_by_id(win_id);
    if (w && w->proto) {
        wp_command_t c = { WP_EV_COMMAND, w->id, wp_command };
        proto_send(w, &c, sizeof(c));
    }
}
