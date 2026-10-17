#include "wserver.h"
#include "window.h"
#include "gfx.h"
#include "theme.h"
#include "icons.h"
#include "ovos.h"
#include "wproto.h"
#include "notifier.h"

// =============================================================================
// Серверная часть оконного протокола (src/libs/common/wproto.h): desktop — композитор.
// Клиенты присылают запросы сообщениями IPC, кадры лежат в их разделяемой памяти.
// =============================================================================

#define PROTO_ID_BASE 1000            // id окон протокола не пересекаются с id окон ядра

static int      g_next_proto_id = PROTO_ID_BASE;
static uint64_t g_last_alive_check = 0;

int wserver_init(void) {
    return ov_register(WP_SERVICE_NAME);
}

static window_t* find_proto(int id, int owner_pid) {
    int n = 0;
    window_t* list = window_get_list(&n);
    for (int i = 0; i < n; i++) {
        window_t* w = &list[i];
        if (w->proto && w->id == id && w->pid == owner_pid) return w;
    }
    return 0;
}

static void reply(int pid, const void* msg, uint32_t len) {
    ov_send(pid, msg, len);
}

static int handle_create(const ov_msg_t* m) {
    const wp_create_t* c = (const wp_create_t*)m->data;
    wp_created_t r = { WP_CREATED, -1 };
    if (m->len < sizeof(wp_create_t) || c->w <= 0 || c->h <= 0 || c->w > 1920 || c->h > 1080) {
        reply(m->sender_pid, &r, sizeof(r));
        return 0;
    }

    // Буфер клиента: должен быть не меньше окна, иначе мы читали бы за его пределами
    uint64_t need = (uint64_t)c->w * (uint64_t)c->h * 4;
    uint64_t have = (uint64_t)ov_sc(111, (uint64_t)c->shm_id, 0, 0);
    uint32_t* surf = 0;
    if (have >= need) surf = (uint32_t*)ov_shm_map(c->shm_id);
    window_t* w = surf ? window_slot_alloc() : 0;
    if (!w) {
        if (surf) ov_shm_unmap(c->shm_id);
        reply(m->sender_pid, &r, sizeof(r));
        return 0;
    }

    w->id = g_next_proto_id++;
    w->pid = m->sender_pid;
    w->proto = 1;
    w->shm_id = c->shm_id;
    w->flags = c->flags;
    w->w = c->w;
    w->h = c->h;
    w->surf = surf;
    w->surf_cap = (uint32_t)have;
    w->surf_off = 0;
    w->committed = 0;
    ov_strlcpy(w->title, c->title, sizeof(w->title));
    ov_strlcpy(w->app, c->app, sizeof(w->app));
    // Значок окна -- из ресурсов исполняемого файла клиента (как _NET_WM_PID +
    // .desktop в X11). Путь даёт ядро, а не клиент.
    if (ov_proc_exe(m->sender_pid, w->exe, sizeof(w->exe)) != 0) w->exe[0] = '\0';
    int has_icon = 0;
    for (int i = 0; i < 16; i++) { w->icon_data[i] = c->icon[i]; if (c->icon[i]) has_icon = 1; }
    w->has_icon = has_icon;
    w->icon = has_icon ? w->icon_data : icon_placeholder;
    w->render_content = 0;
    w->on_click = 0;
    w->on_key = 0;
    w->is_minimized = 0;
    w->is_open = 1;
    window_place_cascade(w);
    window_activate(w);

    r.win = w->id;
    reply(m->sender_pid, &r, sizeof(r));
    return 1;
}

// WP_OPEN: файловый менеджер просит отдать файл уже работающей программе того же
// класса (как activation в XDG). Нашли — поднимаем её окно и шлём ей WP_EV_OPEN.
static int handle_open(const ov_msg_t* m) {
    const wp_open_t* o = (const wp_open_t*)m->data;
    wp_opened_t r = { WP_OPENED, 0 };
    if (m->len < sizeof(wp_open_t) || !o->app[0]) { reply(m->sender_pid, &r, sizeof(r)); return 0; }

    int n = 0;
    window_t* list = window_get_list(&n);
    window_t* found = 0;
    for (int i = 0; i < n; i++) {
        window_t* w = &list[i];
        if (!w->proto || !w->is_open || w->pid <= 0 || w->pid == m->sender_pid) continue;
        if (!ov_streq(w->app, o->app)) continue;
        found = w;
        break;
    }
    if (found) {
        wp_openfile_t ev;
        ov_memset(&ev, 0, sizeof(ev));
        ev.type = WP_EV_OPEN;
        ev.win = found->id;
        ov_strlcpy(ev.path, o->path, sizeof(ev.path));
        ov_send(found->pid, &ev, sizeof(ev));
        found->is_minimized = 0;
        window_activate(found);
        r.found = 1;
    }
    reply(m->sender_pid, &r, sizeof(r));
    return r.found;
}

// Отдать путь уже открытой программе этого класса (WP_EV_OPEN) -- то же, что
// делает WP_OPEN, только по просьбе самого композитора (кнопка "Открыть" в
// уведомлении). 1 -- нашлась.
int wserver_hand_over(const char* app, const char* path) {
    int n = 0;
    window_t* list = window_get_list(&n);
    for (int i = 0; i < n; i++) {
        window_t* w = &list[i];
        if (!w->proto || !w->is_open || w->pid <= 0 || !ov_streq(w->app, app)) continue;
        wp_openfile_t ev;
        ov_memset(&ev, 0, sizeof(ev));
        ev.type = WP_EV_OPEN;
        ev.win = w->id;
        ov_strlcpy(ev.path, path, sizeof(ev.path));
        ov_send(w->pid, &ev, sizeof(ev));
        w->is_minimized = 0;
        window_activate(w);
        return 1;
    }
    return 0;
}

int wserver_poll(uint32_t wait_ms) {
    int changed = 0;
    ov_msg_t m;
    int got = ov_recv(&m, wait_ms);
    while (got) {
        if (m.len >= 4) {
            uint32_t type = *(const uint32_t*)m.data;
            const wp_simple_t* s = (const wp_simple_t*)m.data;
            window_t* w;
            switch (type) {
                case WP_CREATE:
                    changed |= handle_create(&m);
                    break;
                case WP_COMMIT:
                    if (m.len >= sizeof(wp_commit_t) && (w = find_proto(s->win, m.sender_pid))) {
                        const wp_commit_t* cm = (const wp_commit_t*)m.data;
                        // Берём показываемую половину буфера, но только если
                        // она целиком внутри того, что клиент нам выделил.
                        uint64_t frame = (uint64_t)w->w * (uint64_t)w->h * 4;
                        if ((uint64_t)cm->off * 4 + frame <= (uint64_t)w->surf_cap)
                            w->surf_off = cm->off;
                        else
                            w->surf_off = 0;
                        // Не «changed»: новый кадр окна не требует перерисовки всего
                        // экрана, цикл видит его по флагу (wserver_has_commits)
                        w->committed = 1;
                    }
                    break;
                case WP_DESTROY:
                    if (m.len >= sizeof(wp_simple_t) && (w = find_proto(s->win, m.sender_pid))) {
                        window_release(w);
                        changed = 1;
                    }
                    break;
                case WP_OPEN:
                    changed |= handle_open(&m);
                    break;
                case WP_RESIZE:
                    if (m.len >= sizeof(wp_newbuf_t) && (w = find_proto(s->win, m.sender_pid))) {
                        const wp_newbuf_t* nb = (const wp_newbuf_t*)m.data;
                        window_apply_resize(w, nb->shm_id, nb->w, nb->h);
                        changed = 1;
                    }
                    break;
                case WP_CURSOR:
                    // Форму курсора внутри своего окна выбирает программа
                    if (m.len >= sizeof(wp_cursor_t) && (w = find_proto(s->win, m.sender_pid))) {
                        int sh = ((const wp_cursor_t*)m.data)->shape;
                        if (sh >= 0 && sh < WP_CUR_COUNT && w->cursor != sh) {
                            w->cursor = sh;
                            changed = 1;
                        }
                    }
                    break;
                case WP_GRAB:
                    if (m.len >= sizeof(wp_grab_t) && (w = find_proto(s->win, m.sender_pid))) {
                        window_set_grab(w, ((const wp_grab_t*)m.data)->on);
                        changed = 1;
                    }
                    break;
                case WP_NOTIFY:
                    // Уведомление: окна для этого не нужно (как notify-send)
                    if (m.len >= sizeof(wp_notify_t)) {
                        notifier_post_app((const wp_notify_t*)m.data, m.sender_pid);
                        changed = 1;
                    }
                    break;
                case WP_NOTIFY_CLOSE:
                    if (m.len >= sizeof(wp_notify_close_t)) {
                        notifier_close_app(((const wp_notify_close_t*)m.data)->id, m.sender_pid);
                        changed = 1;
                    }
                    break;
                case WP_SET_TITLE:
                    if (m.len >= sizeof(wp_title_t) && (w = find_proto(s->win, m.sender_pid))) {
                        ov_strlcpy(w->title, ((const wp_title_t*)m.data)->title, sizeof(w->title));
                        changed = 1;
                    }
                    break;
                default:
                    break;
            }
        }
        got = ov_recv(&m, 0);   // забираем всё, что накопилось, не ожидая
    }
    return changed;
}

int wserver_has_commits(void) {
    int n = 0;
    window_t* list = window_get_list(&n);
    for (int i = 0; i < n; i++)
        if (list[i].proto && list[i].committed) return 1;
    return 0;
}

void wserver_frame_done(void) {
    int n = 0;
    window_t* list = window_get_list(&n);
    for (int i = 0; i < n; i++) {
        window_t* w = &list[i];
        if (!w->proto || !w->committed) continue;
        w->committed = 0;
        wp_simple_t d = { WP_FRAME_DONE, w->id };
        ov_send(w->pid, &d, sizeof(d));
    }
}

int wserver_tick(void) {
    uint64_t now = ov_ms();
    int changed = 0;
    int n = 0;
    window_t* list = window_get_list(&n);

    for (int i = 0; i < n; i++) {
        window_t* w = &list[i];
        if (!w->proto || w->pid <= 0) continue;
        // Закрыли [X], а клиент не завершился сам — принудительно
        if (w->closing && now >= w->close_deadline) {
            ov_kill(w->pid, 9);
            window_release(w);
            changed = 1;
        }
    }

    // Клиент завершился (в т.ч. упал) — убираем его окна. Проверяем раз в 300 мс.
    if (now - g_last_alive_check >= 300) {
        g_last_alive_check = now;
        for (int i = 0; i < n; i++) {
            window_t* w = &list[i];
            if (w->proto && w->pid > 0 && !ov_alive(w->pid)) {
                window_release(w);
                changed = 1;
            }
        }
    }
    return changed;
}

void wserver_close_all(void) {
    int n = 0;
    window_t* list = window_get_list(&n);
    for (int i = 0; i < n; i++) {
        window_t* w = &list[i];
        if (w->pid > 0) ov_kill(w->pid, 9);
        if (w->proto) window_release(w);
    }
}
