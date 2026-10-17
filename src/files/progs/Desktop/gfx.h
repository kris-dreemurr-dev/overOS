#ifndef GFX_H
#define GFX_H

#include <stdint.h>

extern uint32_t* g_backbuffer;
extern uint32_t  g_screen_w;
extern uint32_t  g_screen_h;
extern uint32_t  g_buf_bytes;
extern int       g_fb_fd;
extern uint32_t* g_wallpaper_buf;
extern int       g_has_wallpaper;

void gfx_load_wallpaper(const char* filepath);
void gfx_draw_wallpaper(void);
void gfx_init(int fb_fd);
void gfx_draw_rect(int rx, int ry, int rw, int rh, uint32_t color);
void gfx_draw_char(int x, int y, uint8_t ch, uint32_t color);
void gfx_draw_string(int x, int y, const char* str, uint32_t color);
int  gfx_text_width(const char* str);        // ширина строки (шрифт переменной ширины)
int  gfx_text_width_lg(const char* str);     // то же для крупного кегля
void gfx_draw_string_px(int x, int y, const char* str, uint32_t color, int px);
int  gfx_text_width_px(const char* str, int px);
void gfx_draw_dec(int x, int y, int val, uint32_t color);
void gfx_draw_dec2(int x, int y, int val, uint32_t color);
void gfx_draw_mono_icon(int x, int y, const uint16_t* mask, uint32_t fg, uint32_t alt_col, int has_alt);
// Значки: "имя" -- /sys/icons/<имя>.svg, "/путь/программа.prg" -- значок из ресурсов
// программы. Растеризуются системой и кэшируются по размеру
int  gfx_icon(const char* name, int size);
void gfx_draw_icon(int x, int y, int idx, uint32_t tint);
void gfx_icon_draw(int x, int y, const char* name, int size, uint32_t tint);
// Курсор заданной формы (WP_CUR_* из оконного протокола)
#define GFX_CUR_HIDDEN (-1)                 // курсор не рисовать (мышь захвачена)
void gfx_draw_cursor(int mx, int my, int shape);
void gfx_present(void);
// Частичный вывод и программный курсор (см. gfx.c)
void gfx_present_rows(int y0, int y1);
void gfx_cursor_forget(void);
void gfx_cursor_restore(void);
void gfx_cursor_rows(int* y0, int* y1);
void gfx_blit(int x, int y, int w, int h, const uint32_t* src);
void gfx_draw_char_scaled(int x, int y, uint8_t ch, uint32_t color, int scale);
void gfx_draw_string_scaled(int x, int y, const char* str, uint32_t color, int scale);
void gfx_darken(int rx, int ry, int rw, int rh, int num);
void gfx_fill_circle(int cx, int cy, int rad, uint32_t color);
void gfx_round_rect(int x, int y, int w, int h, int r, int corners, uint32_t color);
void gfx_blit_round(int x, int y, int w, int h, const uint32_t* src, int r, int corners);
// Скруглённый прямоугольник со сглаженными краями и мягкая тень под ним
void gfx_round_rect_aa(int x, int y, int w, int h, int r, uint32_t color);
void gfx_shadow_round(int x, int y, int w, int h, int r, int spread, int alpha0);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color);

// Маски углов для gfx_round_rect
#define GFX_CORNER_TOP    0x03
#define GFX_CORNER_BOTTOM 0x0C
#define GFX_CORNER_ALL    0x0F

#endif