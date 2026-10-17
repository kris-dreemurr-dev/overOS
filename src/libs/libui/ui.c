#include "ui.h"
#include "libgfx/uifont.h"
#include "libgfx/svg.h"
#include "prgres.h"

// =============================================================================
// libui: подключение к композитору, буфер окна в разделяемой памяти, ввод, виджеты
// =============================================================================

#define UI_MAX_WINDOWS 4
#define UI_MAX_KEYS    32

struct ui_win {
    int       used;
    int       server;          // pid композитора
    int       id;              // id окна у композитора
    int       shm_id;
    uint32_t* px;        // половина, в которую рисуем сейчас
    uint32_t* base;      // начало общего буфера (он вдвое больше окна)
    int       flip;      // какая половина сейчас наша: 0 или 1
    int       w, h;
    int       want_w, want_h;  // композитор попросил другой размер (разворачивание)

    int       frame_pending;   // кадр отправлен, ждём WP_FRAME_DONE (рисовать в буфер нельзя)
    int       want_frame;      // ui_request_frame
    int       redraw_all;      // ui_redraw_all: прошлый кадр не переносить
    int       closed;
    int       focused;

    // Состояние ввода
    int       mx, my, buttons;
    int       prev_buttons;
    int       mods;
    uint64_t  last_click_ms;
    int       last_click_x, last_click_y;

    // Ввод за кадр
    int       has_input;       // в этом кадре уже пришёл ввод — не ждать следующего
    int       clicked, dbl;
    int       command;         // WP_EV_COMMAND за кадр (WP_CMD_*)
    char      open_path[WP_OPEN_PATH];   // WP_EV_OPEN за кадр: открыть ещё один файл
    int       notify_act;      // WP_EV_NOTIFY_ACTION за кадр: кнопка, -2 -- не было
    uint32_t  notify_id;
    int       scroll;
    int       grabbed;         // мышь захвачена (WP_EV_GRAB)
    int       mdx, mdy;        // смещения захваченной мыши за кадр
    int       keys[UI_MAX_KEYS];
    int       nkeys;
    int       raw[UI_MAX_KEYS];   // и нажатия, и отпускания (для псевдотерминала)
    int       nraw;
    char      typed[64];
    int       typed_len;

    int       cursor;          // форма курсора, о которой композитор уже знает
    int       cursor_req;      // что запросили виджеты в этом кадре

    // Виджеты
    int       next_id;         // авто-id по порядку вызовов в кадре
    int       active_id;       // виджет, захвативший мышь (ползунок)
    int       focus_id;        // поле ввода с фокусом
};

static struct ui_win g_wins[UI_MAX_WINDOWS];

// Класс программы для единственного экземпляра: ui_app_id("ovplayer") до ui_open
static char g_app_id[24];

void ui_app_id(const char* id) { ov_strlcpy(g_app_id, id ? id : "", sizeof(g_app_id)); }

// -----------------------------------------------------------------------------
// Протокол
// -----------------------------------------------------------------------------
static void ui_handle(struct ui_win* w, const ov_msg_t* m) {
    if (m->sender_pid != w->server || m->len < 8) return;   // только от композитора
    const uint32_t type = *(const uint32_t*)m->data;
    // Кнопка уведомления: оно не принадлежит окну, отдаём первому окну программы
    if (type == WP_EV_NOTIFY_ACTION) {
        if (m->len >= sizeof(wp_notify_action_t)) {
            const wp_notify_action_t* a = (const wp_notify_action_t*)m->data;
            w->notify_act = a->action;
            w->notify_id = a->id;
            w->has_input = 1;
        }
        return;
    }
    const int32_t  win  = *(const int32_t*)(m->data + 4);
    if (win != w->id) return;

    if (type != WP_FRAME_DONE) w->has_input = 1;
    switch (type) {
        case WP_FRAME_DONE:
            w->frame_pending = 0;
            break;
        case WP_EV_CLOSE:
            w->closed = 1;
            break;
        case WP_EV_COMMAND:
            if (m->len >= sizeof(wp_command_t)) w->command = ((const wp_command_t*)m->data)->command;
            break;
        case WP_EV_OPEN:
            if (m->len >= sizeof(wp_openfile_t))
                ov_strlcpy(w->open_path, ((const wp_openfile_t*)m->data)->path, sizeof(w->open_path));
            break;
        case WP_EV_FOCUS:
            w->focused = ((const wp_focus_t*)m->data)->focused;
            break;
        case WP_EV_RESIZE: {
            // Новый буфер выделяем не здесь, а в начале кадра: сейчас в старый
            // буфер ещё может рисовать вызывающий код
            const wp_resize_t* e = (const wp_resize_t*)m->data;
            if (m->len >= sizeof(wp_resize_t) && e->w > 0 && e->h > 0) {
                w->want_w = e->w;
                w->want_h = e->h;
            }
            break;
        }
        case WP_EV_GRAB:
            if (m->len >= sizeof(wp_grab_t)) w->grabbed = ((const wp_grab_t*)m->data)->on;
            break;
        case WP_EV_MOTION: {
            const wp_motion_t* e = (const wp_motion_t*)m->data;
            if (m->len >= sizeof(wp_motion_t)) {
                w->mdx += e->dx;
                w->mdy += e->dy;
                if ((e->buttons & 1) && !(w->buttons & 1)) w->clicked = 1;
                w->buttons = e->buttons;
            }
            break;
        }
        case WP_EV_SCROLL: {
            const wp_scroll_t* e = (const wp_scroll_t*)m->data;
            w->scroll += e->clicks;
            w->mx = e->x; w->my = e->y;
            break;
        }
        case WP_EV_MOUSE: {
            const wp_mouse_t* e = (const wp_mouse_t*)m->data;
            w->mx = e->x; w->my = e->y;
            if (e->kind == WP_MOUSE_DOWN) {
                w->buttons |= 1;
                w->clicked = 1;
                uint64_t now = ov_ms();
                int dx = e->x - w->last_click_x, dy = e->y - w->last_click_y;
                if (now - w->last_click_ms < 400 && dx * dx + dy * dy < 36) w->dbl = 1;
                w->last_click_ms = now;
                w->last_click_x = e->x;
                w->last_click_y = e->y;
            } else if (e->kind == WP_MOUSE_UP) {
                w->buttons &= ~1;
            } else if (e->kind == WP_MOUSE_LEAVE) {
                w->mx = -1; w->my = -1;
            } else {
                w->buttons = e->buttons;
            }
            break;
        }
        case WP_EV_KEY: {
            const wp_key_t* e = (const wp_key_t*)m->data;
            w->mods = e->mods;
            if (w->nraw < UI_MAX_KEYS)
                w->raw[w->nraw++] = e->pressed ? e->scancode : (e->scancode | 0x80);
            if (!e->pressed) break;
            if (w->nkeys < UI_MAX_KEYS) w->keys[w->nkeys++] = e->scancode;
            for (int i = 0; e->text[i] && w->typed_len < (int)sizeof(w->typed) - 1; i++)
                w->typed[w->typed_len++] = e->text[i];
            w->typed[w->typed_len] = '\0';
            break;
        }
        default:
            break;
    }
}

static void ui_send(struct ui_win* w, const void* msg, uint32_t len) {
    // Очередь композитора может быть полна — пробуем ещё немного
    for (int tries = 0; tries < 50; tries++) {
        int r = ov_send(w->server, msg, len);
        if (r != -2) return;
        ov_sleep(2);
    }
}

ui_win_t* ui_open(const char* title, int ww, int hh, const uint16_t* icon16, uint32_t flags) {
    struct ui_win* w = 0;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) if (!g_wins[i].used) { w = &g_wins[i]; break; }
    if (!w || ww <= 0 || hh <= 0) return 0;
    ov_memset(w, 0, sizeof(*w));

    w->server = ov_lookup(WP_SERVICE_NAME);
    if (w->server < 0) return 0;

    uif_init();      // /sys/fonts/ui.ttf — один раз на процесс

    // Буфер вдвое больше окна: рисуем в одну половину, показываем другую.
    // Композитор перерисовывает экран не только по нашему COMMIT (например,
    // пока окно тащат мышью), и, читая ту же половину, он ловил окно в
    // середине отрисовки -- видео в плеере от этого мигало.
    w->shm_id = ov_shm_create((uint64_t)ww * (uint64_t)hh * 8);
    if (w->shm_id < 0) return 0;
    ov_shm_grant(w->shm_id, w->server);
    w->base = (uint32_t*)ov_shm_map(w->shm_id);
    if (!w->base) { ov_shm_destroy(w->shm_id); return 0; }
    w->flip = 0;
    w->px = w->base;
    w->w = ww;
    w->h = hh;

    wp_create_t c;
    ov_memset(&c, 0, sizeof(c));
    c.type = WP_CREATE;
    c.shm_id = w->shm_id;
    c.w = ww;
    c.h = hh;
    c.flags = flags;
    if (icon16) for (int i = 0; i < 16; i++) c.icon[i] = icon16[i];
    ov_strlcpy(c.title, title ? title : "", sizeof(c.title));
    ov_strlcpy(c.app, g_app_id, sizeof(c.app));
    w->id = -1;
    w->used = 1;
    ui_send(w, &c, sizeof(c));

    // Ждём WP_CREATED (до 3 с)
    uint64_t deadline = ov_ms() + 3000;
    ov_msg_t m;
    while (w->id < 0 && ov_ms() < deadline) {
        if (!ov_recv(&m, 100)) continue;
        if (m.sender_pid != w->server || m.len < sizeof(wp_created_t)) continue;
        const wp_created_t* r = (const wp_created_t*)m.data;
        if (r->type == WP_CREATED) {
            if (r->win < 0) break;
            w->id = r->win;
        }
    }
    if (w->id < 0) {
        ov_shm_unmap(w->shm_id);
        ov_shm_destroy(w->shm_id);
        w->used = 0;
        return 0;
    }
    w->mx = w->my = -1;
    w->focused = 1;
    w->want_frame = 1;
    return w;
}

void ui_close(ui_win_t* w) {
    if (!w || !w->used) return;
    wp_simple_t d = { WP_DESTROY, w->id };
    ui_send(w, &d, sizeof(d));
    ov_shm_unmap(w->shm_id);
    ov_shm_destroy(w->shm_id);
    w->used = 0;
}

void ui_set_title(ui_win_t* w, const char* title) {
    wp_title_t t;
    ov_memset(&t, 0, sizeof(t));
    t.type = WP_SET_TITLE;
    t.win = w->id;
    ov_strlcpy(t.title, title, sizeof(t.title));
    ui_send(w, &t, sizeof(t));
}

int       ui_width(ui_win_t* w)  { return w->w; }
int       ui_height(ui_win_t* w) { return w->h; }
uint32_t* ui_pixels(ui_win_t* w) { return w->px; }
void      ui_request_frame(ui_win_t* w) { w->want_frame = 1; }
void      ui_redraw_all(ui_win_t* w) { if (w) w->redraw_all = 1; }

void ui_grab(ui_win_t* w, int on) {
    if (!w) return;
    wp_grab_t g = { WP_GRAB, w->id, on ? 1 : 0 };
    ui_send(w, &g, sizeof(g));
    if (!on) w->grabbed = 0;     // включение подтвердит композитор (WP_EV_GRAB)
}
int  ui_grabbed(ui_win_t* w) { return w ? w->grabbed : 0; }
void ui_motion(ui_win_t* w, int* dx, int* dy) {
    if (dx) *dx = w ? w->mdx : 0;
    if (dy) *dy = w ? w->mdy : 0;
}
int  ui_buttons(ui_win_t* w) { return w ? w->buttons : 0; }

int ui_begin(ui_win_t* w, uint32_t timeout_ms) {
    if (!w || !w->used) return 0;

    // Новый кадр — сбрасываем ввод прошлого
    w->prev_buttons = w->buttons;
    w->has_input = 0;
    w->command = 0;
    w->open_path[0] = '\0';
    w->notify_act = -2;
    w->clicked = 0;
    w->dbl = 0;
    w->scroll = 0;
    w->mdx = w->mdy = 0;
    w->nkeys = 0;
    w->nraw = 0;
    w->typed_len = 0;
    w->typed[0] = '\0';
    w->next_id = 0;
    w->cursor_req = WP_CUR_ARROW;    // форму курсора каждый кадр просят заново

    ov_msg_t m;
    // 0. Композитор попросил другой размер: выделяем новый буфер и отдаём ему
    if (w->want_w > 0 && w->want_h > 0 && (w->want_w != w->w || w->want_h != w->h)) {
        int nw = w->want_w, nh = w->want_h;
        w->want_w = w->want_h = 0;
        int nid = ov_shm_create((uint64_t)nw * (uint64_t)nh * 8);
        if (nid >= 0) {
            ov_shm_grant(nid, w->server);
            uint32_t* npx = (uint32_t*)ov_shm_map(nid);
            if (npx) {
                int old_id = w->shm_id;
                w->shm_id = nid;
                w->base = npx;
                w->flip = 0;
                w->px = npx;
                w->w = nw;
                w->h = nh;
                wp_newbuf_t nb = { WP_RESIZE, w->id, nid, nw, nh };
                ui_send(w, &nb, sizeof(nb));
                ov_shm_unmap(old_id);
                ov_shm_destroy(old_id);
                w->frame_pending = 0;
                w->has_input = 1;        // перерисовать окно под новый размер
            } else {
                ov_shm_destroy(nid);
            }
        }
    }

    // 1. Пока композитор не вывел прошлый кадр, буфер трогать нельзя
    uint64_t deadline = ov_ms() + 200;
    while (w->frame_pending && !w->closed && ov_ms() < deadline) {
        if (ov_recv(&m, 50)) ui_handle(w, &m);
    }
    w->frame_pending = 0;

    // 2. Ждём событие (или таймаут анимации), затем забираем всё, что накопилось
    if (!w->closed && !w->want_frame && !w->has_input) {
        if (ov_recv(&m, timeout_ms)) ui_handle(w, &m);
    }
    while (ov_recv(&m, 0)) ui_handle(w, &m);
    w->want_frame = 0;

    return !w->closed;
}

void ui_end(ui_win_t* w) {
    if (!w || !w->used || w->closed) return;
    if (w->cursor_req != w->cursor) {          // форма курсора сменилась за кадр
        w->cursor = w->cursor_req;
        wp_cursor_t cur = { WP_CURSOR, w->id, w->cursor };
        ui_send(w, &cur, sizeof(cur));
    }
    uint32_t half = (uint32_t)w->w * (uint32_t)w->h;
    wp_commit_t c = { WP_COMMIT, w->id, (uint32_t)w->flip * half };
    ui_send(w, &c, sizeof(c));
    w->frame_pending = 1;

    // Переходим на вторую половину и переносим туда только что показанный кадр:
    // многие окна дорисовывают поверх прежнего, а не чистят его целиком.
    uint32_t* done = w->px;
    w->flip ^= 1;
    w->px = w->base + (uint32_t)w->flip * half;
    if (w->redraw_all) return;
    uint64_t q = half / 2;
    void* d = w->px;
    const void* src = done;
    __asm__ volatile ("rep movsq" : "+D"(d), "+S"(src), "+c"(q) : : "memory");
    if (half & 1) w->px[half - 1] = done[half - 1];
}

// -----------------------------------------------------------------------------
// Рисование
// -----------------------------------------------------------------------------
void ui_fill(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color) {
    if (x < 0) { ww += x; x = 0; }
    if (y < 0) { hh += y; y = 0; }
    if (x + ww > w->w) ww = w->w - x;
    if (y + hh > w->h) hh = w->h - y;
    if (ww <= 0 || hh <= 0) return;
    for (int r = 0; r < hh; r++) {
        uint32_t* row = w->px + (y + r) * w->w + x;
        for (int c = 0; c < ww; c++) row[c] = color;
    }
}

void ui_clear(ui_win_t* w) { ui_fill(w, 0, 0, w->w, w->h, UI_BG); }

void ui_hline(ui_win_t* w, int x, int y, int len, uint32_t color) { ui_fill(w, x, y, len, 1, color); }

void ui_frame(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color) {
    ui_fill(w, x, y, ww, 1, color);
    ui_fill(w, x, y + hh - 1, ww, 1, color);
    ui_fill(w, x, y, 1, hh, color);
    ui_fill(w, x + ww - 1, y, 1, hh, color);
}

// Глиф шрифта в буфер окна: y — верх строки, базовая линия на uif_baseline()
static void draw_glyph(ui_win_t* w, int x, int y, const ttf_glyph_t* g, int px, uint32_t color) {
    const uint8_t* cov = uif_cov(g);
    int gx = x + g->bx, gy = y + uif_baseline(px) - g->by;
    for (int r = 0; r < g->h; r++) {
        int py = gy + r;
        if (py < 0 || py >= w->h) continue;
        uint32_t* row = &w->px[py * w->w];
        for (int c = 0; c < g->w; c++) {
            uint32_t a = cov[r * g->w + c];
            int pxx = gx + c;
            if (!a || pxx < 0 || pxx >= w->w) continue;
            row[pxx] = uif_blend(row[pxx], color, a);
        }
    }
}

// Запасной растровый шрифт (шрифт не загрузился) и моноширинный вывод
static void draw_mono_glyph(ui_win_t* w, int x, int y, uint32_t cp, uint32_t color) {
    const uint8_t* rows = uif_mono_rows(cp);
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= w->h) continue;
        uint8_t bits = rows[r];
        for (int c = 0; c < 8; c++) {
            int pxx = x + c;
            if ((bits & (0x80 >> c)) && pxx >= 0 && pxx < w->w) w->px[py * w->w + pxx] = color;
        }
    }
}

void ui_text(ui_win_t* w, int x, int y, const char* utf8, uint32_t color) {
    const uint8_t* s = (const uint8_t*)utf8;
    while (*s) {
        uint32_t cp = uif_utf8_next(&s);
        if (!uif_ok()) { draw_mono_glyph(w, x, y, cp, color); x += UIF_MONO_W; continue; }
        const ttf_glyph_t* g = uif_glyph(cp, UIF_PX);
        if (!g) continue;
        draw_glyph(w, x, y, g, UIF_PX, color);
        x += g->adv;
    }
}

// Моноширинный текст: редактор и всё, где колонки считаются по символам —
// пропорциональный шрифт сдвигал бы там курсор
void ui_text_mono(ui_win_t* w, int x, int y, const char* utf8, uint32_t color) {
    const uint8_t* s = (const uint8_t*)utf8;
    while (*s) {
        uint32_t cp = uif_utf8_next(&s);
        draw_mono_glyph(w, x, y, cp, color);
        x += UI_MONO_W;
    }
}

int ui_text_width(const char* utf8) { return uif_text_width(utf8); }

void ui_text_center(ui_win_t* w, int x, int y, int ww, const char* utf8, uint32_t color) {
    ui_text(w, x + (ww - ui_text_width(utf8)) / 2, y, utf8, color);
}

void ui_icon16(ui_win_t* w, int x, int y, const uint16_t* mask, uint32_t color) {
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++)
            if (mask[r] & (0x8000 >> c)) ui_fill(w, x + c, y + r, 1, 1, color);
}

void ui_blit(ui_win_t* w, int x, int y, int ww, int hh, const uint32_t* src, int stride) {
    for (int r = 0; r < hh; r++) {
        int py = y + r;
        if (py < 0 || py >= w->h) continue;
        for (int c = 0; c < ww; c++) {
            int px = x + c;
            if (px >= 0 && px < w->w) w->px[py * w->w + px] = src[r * stride + c];
        }
    }
}

// -----------------------------------------------------------------------------
// Ввод
// -----------------------------------------------------------------------------
int ui_mouse_x(ui_win_t* w)       { return w->mx; }
int ui_mouse_y(ui_win_t* w)       { return w->my; }
int ui_mouse_down(ui_win_t* w)    { return w->buttons & 1; }
int ui_mouse_clicked(ui_win_t* w) { return w->clicked; }
int ui_double_clicked(ui_win_t* w){ return w->dbl; }
int ui_scroll(ui_win_t* w)        { return w->scroll; }
int ui_mods(ui_win_t* w)          { return w->mods; }
int ui_focused(ui_win_t* w)       { return w->focused; }
int ui_command(ui_win_t* w)       { return w->command; }
const char* ui_open_request(ui_win_t* w) { return w->open_path; }

int ui_notify_action(ui_win_t* w, uint32_t* id) {
    if (id) *id = w->notify_id;
    return w->notify_act;
}

// -----------------------------------------------------------------------------
// Значки: SVG из /sys/icons или из ресурсов программы, растеризованные один раз
// -----------------------------------------------------------------------------
#define UI_ICON_SLOTS 24
#define UI_ICON_ARENA (24 * 1024)          // пикселей на все значки процесса
#define UI_ICON_FILE  8192

typedef struct {
    char      name[96];
    int       size;
    int       ok;
    uint32_t* px;
} ui_icon_slot_t;

static ui_icon_slot_t s_icons[UI_ICON_SLOTS];
static uint32_t       s_icon_arena[UI_ICON_ARENA];
static int            s_icon_n = 0, s_icon_used = 0;

static const ui_icon_slot_t* ui_icon_get(const char* name, int size) {
    for (int i = 0; i < s_icon_n; i++)
        if (s_icons[i].size == size && ov_streq(s_icons[i].name, name)) return s_icons[i].ok ? &s_icons[i] : 0;
    if (s_icon_n >= UI_ICON_SLOTS || s_icon_used + size * size > UI_ICON_ARENA) return 0;
    ui_icon_slot_t* sl = &s_icons[s_icon_n++];
    ov_strlcpy(sl->name, name, sizeof(sl->name));
    sl->size = size;
    sl->ok = 0;

    static uint8_t file[UI_ICON_FILE];
    int len = 0;
    if (name[0] == '/') {
        len = prg_resource(name, "ICON", file, sizeof(file));
    } else {
        char path[64];
        ov_strlcpy(path, "/sys/icons/", sizeof(path));
        int p = 0; while (path[p]) p++;
        for (int i = 0; name[i] && p < 58; i++) path[p++] = name[i];
        path[p++] = '.'; path[p++] = 's'; path[p++] = 'v'; path[p++] = 'g'; path[p] = '\0';
        int fd = ov_open(path, 0);
        if (fd >= 0) {
            int64_t r;
            while (len < (int)sizeof(file) && (r = ov_read(fd, file + len, sizeof(file) - len)) > 0) len += (int)r;
            ov_close(fd);
        }
    }
    if (len <= 0) return 0;
    sl->px = &s_icon_arena[s_icon_used];
    if (svg_render(file, (uint32_t)len, sl->px, size, size, 0xFFFFFF) != 0) return 0;
    s_icon_used += size * size;
    sl->ok = 1;
    return sl;
}

int ui_icon(ui_win_t* w, int x, int y, int size, const char* name, uint32_t tint) {
    if (!w || !name || !name[0] || size <= 0 || size > 128) return 0;
    const ui_icon_slot_t* ic = ui_icon_get(name, size);
    if (!ic) return 0;
    for (int r = 0; r < size; r++) {
        int py = y + r;
        if (py < 0 || py >= w->h) continue;
        uint32_t* row = w->px + py * w->w;
        for (int c = 0; c < size; c++) {
            int px = x + c;
            if (px < 0 || px >= w->w) continue;
            uint32_t p = ic->px[r * size + c];
            uint32_t a = p >> 24;
            if (!a) continue;
            uint32_t cr = ((p >> 16) & 0xFF) * ((tint >> 16) & 0xFF) / 255;
            uint32_t cg = ((p >> 8) & 0xFF) * ((tint >> 8) & 0xFF) / 255;
            uint32_t cb = (p & 0xFF) * (tint & 0xFF) / 255;
            row[px] = uif_blend(row[px], (cr << 16) | (cg << 8) | cb, a);
        }
    }
    return 1;
}

// Попросить композитор отдать файл уже работающей программе того же класса.
// 1 — отдали (своё окно открывать не нужно), 0 — такой программы нет.
int ui_hand_over(const char* app_id, const char* path) {
    int server = ov_lookup(WP_SERVICE_NAME);
    if (server < 0 || !app_id || !app_id[0] || !path) return 0;

    wp_open_t o;
    ov_memset(&o, 0, sizeof(o));
    o.type = WP_OPEN;
    ov_strlcpy(o.app, app_id, sizeof(o.app));
    ov_strlcpy(o.path, path, sizeof(o.path));
    for (int tries = 0; tries < 50; tries++) {
        int r = ov_send(server, &o, sizeof(o));
        if (r != -2) break;
        ov_sleep(2);
    }

    // Ждём ответ, не теряя событий собственных окон
    uint64_t deadline = ov_ms() + 1000;
    ov_msg_t m;
    while (ov_ms() < deadline) {
        if (!ov_recv(&m, 50)) continue;
        if (m.sender_pid == server && m.len >= sizeof(wp_opened_t) &&
            *(const uint32_t*)m.data == WP_OPENED)
            return ((const wp_opened_t*)m.data)->found;
        for (int i = 0; i < UI_MAX_WINDOWS; i++)
            if (g_wins[i].used) ui_handle(&g_wins[i], &m);
    }
    return 0;
}
const char* ui_typed(ui_win_t* w) { return w->typed; }

int ui_raw_keys(ui_win_t* w, int* out, int max) {
    int n = w->nraw < max ? w->nraw : max;
    for (int i = 0; i < n; i++) out[i] = w->raw[i];
    return n;
}

int ui_key_pressed(ui_win_t* w, int sc) {
    for (int i = 0; i < w->nkeys; i++) if (w->keys[i] == sc) return 1;
    return 0;
}

int ui_hit(ui_win_t* w, int x, int y, int ww, int hh) {
    return w->mx >= x && w->mx < x + ww && w->my >= y && w->my < y + hh;
}

// -----------------------------------------------------------------------------
// Виджеты
// -----------------------------------------------------------------------------
void ui_label(ui_win_t* w, int x, int y, const char* s)       { ui_text(w, x, y, s, UI_TEXT); }
void ui_label_muted(ui_win_t* w, int x, int y, const char* s) { ui_text(w, x, y, s, UI_TEXT_MUTED); }

static int button_impl(ui_win_t* w, int x, int y, int ww, int hh, const char* label, int accent) {
    w->next_id++;
    int hot = ui_hit(w, x, y, ww, hh);
    int pressed = hot && ui_mouse_down(w);
    uint32_t bg = accent ? (hot ? UI_ACCENT_HOVER : UI_ACCENT) : (pressed ? UI_BORDER : hot ? UI_HOVER : UI_PANEL);
    ui_fill(w, x, y, ww, hh, bg);
    if (!accent) ui_frame(w, x, y, ww, hh, hot ? UI_ACCENT : UI_BORDER);
    ui_text_center(w, x, y + (hh - UI_FONT_H) / 2, ww, label, UI_TEXT);
    return hot && w->clicked;
}

int ui_button(ui_win_t* w, int x, int y, int ww, int hh, const char* label) {
    return button_impl(w, x, y, ww, hh, label, 0);
}

int ui_button_accent(ui_win_t* w, int x, int y, int ww, int hh, const char* label) {
    return button_impl(w, x, y, ww, hh, label, 1);
}

int ui_checkbox(ui_win_t* w, int x, int y, const char* label, int* value) {
    w->next_id++;
    int bw = 16 + 8 + ui_text_width(label);
    int hot = ui_hit(w, x, y, bw, 16);
    ui_fill(w, x, y, 16, 16, *value ? UI_ACCENT : UI_BG_DARK);
    ui_frame(w, x, y, 16, 16, hot ? UI_ACCENT : UI_BORDER);
    if (*value) {
        for (int i = 0; i < 4; i++) ui_fill(w, x + 3 + i, y + 7 + i, 2, 2, UI_TEXT);
        for (int i = 0; i < 7; i++) ui_fill(w, x + 6 + i, y + 10 - i, 2, 2, UI_TEXT);
    }
    ui_text(w, x + 24, y, label, UI_TEXT);
    if (hot && w->clicked) { *value = !*value; return 1; }
    return 0;
}

int ui_slider(ui_win_t* w, int x, int y, int ww, int* value, int min, int max) {
    int id = ++w->next_id;
    int track_y = y + 6, track_h = 6;
    int range = max - min;
    if (range <= 0) range = 1;

    int state = UI_SLIDER_IDLE;
    if (w->clicked && ui_hit(w, x - 6, y - 4, ww + 12, 22)) w->active_id = id;   // зона шире дорожки
    if (w->active_id == id) {
        int rel = w->mx - x;
        if (rel < 0) rel = 0;
        if (rel > ww) rel = ww;
        *value = min + (int)((int64_t)range * rel / ww);
        if (ui_mouse_down(w)) {
            state = UI_SLIDER_DRAGGING;
        } else {
            state = UI_RELEASED;
            w->active_id = 0;
        }
    }

    int v = *value;
    if (v < min) v = min;
    if (v > max) v = max;
    int fill = (int)((int64_t)ww * (v - min) / range);
    ui_fill(w, x, track_y, ww, track_h, UI_BG_DARK);
    ui_fill(w, x, track_y, fill, track_h, UI_ACCENT);
    int kx = x + fill - 5;
    if (kx < x) kx = x;
    if (kx > x + ww - 10) kx = x + ww - 10;
    int hot = w->active_id == id || ui_hit(w, x - 6, y - 4, ww + 12, 22);
    ui_fill(w, kx, y, 10, 18, w->active_id == id ? UI_ACCENT : (hot ? UI_TEXT : 0xD0D4D8));
    return state;
}

void ui_progress(ui_win_t* w, int x, int y, int ww, int hh, int value, int max) {
    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    ui_fill(w, x, y, ww, hh, UI_BG_DARK);
    ui_fill(w, x, y, (int)((int64_t)ww * value / max), hh, UI_ACCENT);
}

static int utf8_prev(const char* buf, int len) {
    int i = len - 1;
    while (i > 0 && (((uint8_t)buf[i]) & 0xC0) == 0x80) i--;
    return i < 0 ? 0 : i;
}

static int textbox_impl(ui_win_t* w, int x, int y, int ww, char* buf, int cap, int masked) {
    int id = ++w->next_id;
    int hh = 26;
    if (ui_hit(w, x, y, ww, hh)) ui_cursor(w, WP_CUR_TEXT);   // двутавр над полем
    if (w->clicked) {
        if (ui_hit(w, x, y, ww, hh)) w->focus_id = id;
        else if (w->focus_id == id) w->focus_id = 0;
    }
    int focused = (w->focus_id == id);
    int entered = 0;
    int len = ov_strlen(buf);

    if (focused) {
        for (int k = 0; k < w->nkeys; k++) {
            if (w->keys[k] == WP_KEY_BACKSPACE && len > 0) { len = utf8_prev(buf, len); buf[len] = '\0'; }
            if (w->keys[k] == WP_KEY_ENTER) entered = 1;
        }
        for (int i = 0; w->typed[i]; i++) {
            char c = w->typed[i];
            if ((uint8_t)c < 0x20) continue;
            if (len < cap - 1) { buf[len++] = c; buf[len] = '\0'; }
        }
    }

    ui_fill(w, x, y, ww, hh, UI_BG_DARK);
    ui_frame(w, x, y, ww, hh, focused ? UI_ACCENT : UI_BORDER);
    // Длинный текст — показываем хвост. Шрифт переменной ширины, поэтому режем
    // по ширине в пикселях, а не по числу символов
    const char* shown = buf;
    while (ui_text_width(shown) > ww - 16 && *shown) {
        int l = 1;
        while ((((uint8_t)shown[l]) & 0xC0) == 0x80) l++;
        shown += l;
    }
    int caret_x;
    if (masked) {
        int chars = 0;
        for (const uint8_t* p = (const uint8_t*)shown; *p; p++) if ((*p & 0xC0) != 0x80) chars++;
        for (int i = 0; i < chars; i++) ui_fill(w, x + 10 + i * 8, y + 11, 4, 4, UI_TEXT);
        caret_x = x + 10 + chars * 8;
    } else {
        ui_text(w, x + 8, y + 5, shown, UI_TEXT);
        caret_x = x + 8 + ui_text_width(shown) + 1;
    }
    if (focused) ui_fill(w, caret_x, y + 5, 2, 16, UI_TEXT);
    return entered;
}

void ui_cursor(ui_win_t* w, int shape) {
    if (!w || shape < 0 || shape >= WP_CUR_COUNT) return;
    w->cursor_req = shape;         // отправится в ui_end, если форма сменилась
}

int ui_textbox(ui_win_t* w, int x, int y, int ww, char* buf, int cap) {
    return textbox_impl(w, x, y, ww, buf, cap, 0);
}

int ui_password(ui_win_t* w, int x, int y, int ww, char* buf, int cap) {
    return textbox_impl(w, x, y, ww, buf, cap, 1);
}

void ui_focus_next_textbox(ui_win_t* w) { w->focus_id = w->next_id + 1; }

int ui_list(ui_win_t* w, int x, int y, int ww, int hh, const char* const* items, int count,
            int* selected, int* scroll) {
    w->next_id++;
    const int row_h = 22;
    int rows = hh / row_h;
    int activated = 0;
    int hot = ui_hit(w, x, y, ww, hh);

    if (hot && w->scroll) *scroll += w->scroll * 3;
    if (ui_key_pressed(w, WP_KEY_DOWN) && *selected < count - 1) (*selected)++;
    if (ui_key_pressed(w, WP_KEY_UP) && *selected > 0) (*selected)--;
    if (ui_key_pressed(w, WP_KEY_ENTER) && *selected >= 0 && *selected < count) activated = 1;
    if (*selected >= 0) {
        if (*selected < *scroll) *scroll = *selected;
        if (*selected >= *scroll + rows) *scroll = *selected - rows + 1;
    }
    if (*scroll > count - rows) *scroll = count - rows;
    if (*scroll < 0) *scroll = 0;

    ui_fill(w, x, y, ww, hh, UI_BG_DARK);
    ui_frame(w, x, y, ww, hh, UI_BORDER);
    for (int r = 0; r < rows && *scroll + r < count; r++) {
        int i = *scroll + r;
        int ry = y + r * row_h;
        int row_hot = ui_hit(w, x, ry, ww, row_h);
        if (i == *selected) ui_fill(w, x + 1, ry + 1, ww - 2, row_h - 2, UI_ACCENT);
        else if (row_hot) ui_fill(w, x + 1, ry + 1, ww - 2, row_h - 2, UI_HOVER);
        ui_text(w, x + 8, ry + 3, items[i], UI_TEXT);
        if (row_hot && w->clicked) {
            if (w->dbl && *selected == i) activated = 1;
            *selected = i;
        }
    }
    return activated;
}

// -----------------------------------------------------------------------------
// Утилиты
// -----------------------------------------------------------------------------
void ui_itoa(int v, char* out) {
    char tmp[12];
    int n = 0, p = 0;
    if (v < 0) { out[p++] = '-'; v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) out[p++] = tmp[--n];
    out[p] = '\0';
}

void ui_fmt_time(uint32_t ms, char* out) {
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    s %= 60;
    out[0] = (char)('0' + (m / 10) % 10);
    out[1] = (char)('0' + m % 10);
    out[2] = ':';
    out[3] = (char)('0' + s / 10);
    out[4] = (char)('0' + s % 10);
    out[5] = '\0';
}
