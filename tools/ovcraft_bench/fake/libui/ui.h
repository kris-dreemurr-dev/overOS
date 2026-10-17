// Заглушка libui для стенда: окна нет, рисование — в буфер в памяти
#pragma once
#include <stdint.h>
typedef struct ui_win { uint32_t* px; int w, h; } ui_win_t;
#define UI_FONT_H 16
static inline uint32_t* ui_pixels(ui_win_t* w) { return w->px; }
static inline int ui_width(ui_win_t* w) { return w->w; }
static inline int ui_height(ui_win_t* w) { return w->h; }
static inline void ui_fill(ui_win_t* w, int x, int y, int ww, int hh, uint32_t c) {
    for (int j = y; j < y + hh; j++) for (int i = x; i < x + ww; i++)
        if (i >= 0 && j >= 0 && i < w->w && j < w->h) w->px[j * w->w + i] = c;
}
static inline void ui_frame(ui_win_t* w, int x, int y, int ww, int hh, uint32_t c) {
    ui_fill(w, x, y, ww, 1, c); ui_fill(w, x, y + hh - 1, ww, 1, c); ui_fill(w, x, y, 1, hh, c); ui_fill(w, x + ww - 1, y, 1, hh, c);
}
static inline void ui_text(ui_win_t*, int, int, const char*, uint32_t) {}
static inline int ui_text_width(const char* s) { int n = 0; while (*s++) n += 8; return n; }
static inline void ui_itoa(int v, char* o) { char t[16]; int n = 0; bool neg = v < 0; if (neg) v = -v; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); int k = 0; if (neg) o[k++] = '-'; while (n) o[k++] = t[--n]; o[k] = 0; }
