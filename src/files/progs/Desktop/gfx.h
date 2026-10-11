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
void gfx_draw_dec(int x, int y, int val, uint32_t color);
void gfx_draw_dec2(int x, int y, int val, uint32_t color);
void gfx_draw_mono_icon(int x, int y, const uint16_t* mask, uint32_t fg, uint32_t alt_col, int has_alt);
void gfx_draw_cursor(int mx, int my);
void gfx_present(void);
void gfx_blit(int x, int y, int w, int h, const uint32_t* src);

#endif