#ifndef LIBUI_LL_H
#define LIBUI_LL_H
// libui_ll.h — подключение тулкита из /lib/libui.ll вместо статической сборки.
//
// Программа включает ui.h (там типы и прототипы), затем этот заголовок и зовёт
// libui_bind() один раз в начале main. Дальше вызовы ui_* идут через таблицу
// указателей: имена подменяются макросами, поэтому исходный код приложений
// остаётся прежним.
//
//     #include "libui/ui.h"
//     #include "libui_ll.h"
//     int main(void) { if (libui_bind() < 0) ov_exit(1); ... }

#include "ovos.h"
#include "libui/ui.h"

#define LIBUI_PATH "/lib/libui.ll"

typedef struct {
    int lib;
    ui_win_t* (*open)(const char* title, int w, int h, const uint16_t* icon16, uint32_t flags);
    void (*close)(ui_win_t* w);
    void (*set_title)(ui_win_t* w, const char* title);
    int (*width)(ui_win_t* w);
    int (*height)(ui_win_t* w);
    uint32_t* (*pixels)(ui_win_t* w);
    int (*begin)(ui_win_t* w, uint32_t timeout_ms);
    void (*end)(ui_win_t* w);
    void (*request_frame)(ui_win_t* w);
    void (*redraw_all)(ui_win_t* w);
    void (*grab)(ui_win_t* w, int on);
    int (*grabbed)(ui_win_t* w);
    void (*motion)(ui_win_t* w, int* dx, int* dy);
    int (*buttons)(ui_win_t* w);
    void (*clear)(ui_win_t* w);
    void (*fill)(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color);
    void (*frame)(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color);
    void (*hline)(ui_win_t* w, int x, int y, int len, uint32_t color);
    void (*text)(ui_win_t* w, int x, int y, const char* utf8, uint32_t color);
    void (*text_mono)(ui_win_t* w, int x, int y, const char* utf8, uint32_t color);
    int (*text_width)(const char* utf8);
    void (*text_center)(ui_win_t* w, int x, int y, int ww, const char* utf8, uint32_t color);
    void (*icon16)(ui_win_t* w, int x, int y, const uint16_t* mask, uint32_t color);
    void (*blit)(ui_win_t* w, int x, int y, int ww, int hh, const uint32_t* px, int src_stride);
    void (*label)(ui_win_t* w, int x, int y, const char* utf8);
    void (*label_muted)(ui_win_t* w, int x, int y, const char* utf8);
    int (*button)(ui_win_t* w, int x, int y, int ww, int hh, const char* label);
    int (*button_accent)(ui_win_t* w, int x, int y, int ww, int hh, const char* label);
    int (*checkbox)(ui_win_t* w, int x, int y, const char* label, int* value);
    int (*slider)(ui_win_t* w, int x, int y, int ww, int* value, int min, int max);
    void (*progress)(ui_win_t* w, int x, int y, int ww, int hh, int value, int max);
    int (*textbox)(ui_win_t* w, int x, int y, int ww, char* buf, int cap);
    int (*password)(ui_win_t* w, int x, int y, int ww, char* buf, int cap);
    void (*focus_next_textbox)(ui_win_t* w);
    int (*list)(ui_win_t* w, int x, int y, int ww, int hh, const char* const* items, int count, int* selected, int* scroll);
    int (*mouse_x)(ui_win_t* w);
    int (*mouse_y)(ui_win_t* w);
    int (*mouse_down)(ui_win_t* w);
    int (*mouse_clicked)(ui_win_t* w);
    int (*double_clicked)(ui_win_t* w);
    int (*scroll)(ui_win_t* w);
    int (*key_pressed)(ui_win_t* w, int scancode);
    int (*raw_keys)(ui_win_t* w, int* out, int max);
    int (*mods)(ui_win_t* w);
    const char* (*typed)(ui_win_t* w);
    int (*hit)(ui_win_t* w, int x, int y, int ww, int hh);
    int (*focused)(ui_win_t* w);
    int (*command)(ui_win_t* w);
    void (*app_id)(const char* id);
    const char* (*open_request)(ui_win_t* w);
    int (*hand_over)(const char* app_id, const char* path);
    void (*cursor)(ui_win_t* w, int shape);
    void (*itoa)(int v, char* out);
    void (*fmt_time)(uint32_t ms, char* out);
    int (*icon)(ui_win_t* w, int x, int y, int size, const char* name, uint32_t tint);
    int (*notify_action)(ui_win_t* w, uint32_t* id);
} libui_api_t;

// Одна таблица на всю программу: слабое определение в каждом файле, который
// включил этот заголовок, компоновщик сливает в одно. Со static у программы
// из нескольких .c/.cpp таблицу заполнял бы libui_bind только в main, а
// остальные файлы звали бы по нулям.
__attribute__((weak)) libui_api_t g_ui;

// 0 — тулкит загружен, -1 — нет файла, -2 — нет символа
static inline int libui_bind(void) {
    for (unsigned i = 0; i < sizeof(g_ui); i++) ((char*)&g_ui)[i] = 0;
    int h = ov_lib_open(LIBUI_PATH);
    if (h < 0) return -1;
    g_ui.lib = h;
    OV_BIND(h, g_ui.open, "ui_open");
    OV_BIND(h, g_ui.close, "ui_close");
    OV_BIND(h, g_ui.set_title, "ui_set_title");
    OV_BIND(h, g_ui.width, "ui_width");
    OV_BIND(h, g_ui.height, "ui_height");
    OV_BIND(h, g_ui.pixels, "ui_pixels");
    OV_BIND(h, g_ui.begin, "ui_begin");
    OV_BIND(h, g_ui.end, "ui_end");
    OV_BIND(h, g_ui.request_frame, "ui_request_frame");
    OV_BIND(h, g_ui.redraw_all, "ui_redraw_all");
    OV_BIND(h, g_ui.grab, "ui_grab");
    OV_BIND(h, g_ui.grabbed, "ui_grabbed");
    OV_BIND(h, g_ui.motion, "ui_motion");
    OV_BIND(h, g_ui.buttons, "ui_buttons");
    OV_BIND(h, g_ui.clear, "ui_clear");
    OV_BIND(h, g_ui.fill, "ui_fill");
    OV_BIND(h, g_ui.frame, "ui_frame");
    OV_BIND(h, g_ui.hline, "ui_hline");
    OV_BIND(h, g_ui.text, "ui_text");
    OV_BIND(h, g_ui.text_mono, "ui_text_mono");
    OV_BIND(h, g_ui.text_width, "ui_text_width");
    OV_BIND(h, g_ui.text_center, "ui_text_center");
    OV_BIND(h, g_ui.icon16, "ui_icon16");
    OV_BIND(h, g_ui.blit, "ui_blit");
    OV_BIND(h, g_ui.label, "ui_label");
    OV_BIND(h, g_ui.label_muted, "ui_label_muted");
    OV_BIND(h, g_ui.button, "ui_button");
    OV_BIND(h, g_ui.button_accent, "ui_button_accent");
    OV_BIND(h, g_ui.checkbox, "ui_checkbox");
    OV_BIND(h, g_ui.slider, "ui_slider");
    OV_BIND(h, g_ui.progress, "ui_progress");
    OV_BIND(h, g_ui.textbox, "ui_textbox");
    OV_BIND(h, g_ui.password, "ui_password");
    OV_BIND(h, g_ui.focus_next_textbox, "ui_focus_next_textbox");
    OV_BIND(h, g_ui.list, "ui_list");
    OV_BIND(h, g_ui.mouse_x, "ui_mouse_x");
    OV_BIND(h, g_ui.mouse_y, "ui_mouse_y");
    OV_BIND(h, g_ui.mouse_down, "ui_mouse_down");
    OV_BIND(h, g_ui.mouse_clicked, "ui_mouse_clicked");
    OV_BIND(h, g_ui.double_clicked, "ui_double_clicked");
    OV_BIND(h, g_ui.scroll, "ui_scroll");
    OV_BIND(h, g_ui.key_pressed, "ui_key_pressed");
    OV_BIND(h, g_ui.mods, "ui_mods");
    OV_BIND(h, g_ui.typed, "ui_typed");
    OV_BIND(h, g_ui.hit, "ui_hit");
    OV_BIND(h, g_ui.focused, "ui_focused");
    OV_BIND(h, g_ui.command, "ui_command");
    OV_BIND(h, g_ui.raw_keys, "ui_raw_keys");
    OV_BIND(h, g_ui.app_id, "ui_app_id");
    OV_BIND(h, g_ui.open_request, "ui_open_request");
    OV_BIND(h, g_ui.hand_over, "ui_hand_over");
    OV_BIND(h, g_ui.cursor, "ui_cursor");
    OV_BIND(h, g_ui.itoa, "ui_itoa");
    OV_BIND(h, g_ui.fmt_time, "ui_fmt_time");
    OV_BIND(h, g_ui.icon, "ui_icon");
    OV_BIND(h, g_ui.notify_action, "ui_notify_action");
    const void* const* p = (const void* const*)&g_ui;
    for (unsigned i = 1; i < sizeof(g_ui) / sizeof(void*); i++)
        if (!p[i]) return -2;
    return 0;
}

#define ui_open g_ui.open
#define ui_close g_ui.close
#define ui_set_title g_ui.set_title
#define ui_width g_ui.width
#define ui_height g_ui.height
#define ui_pixels g_ui.pixels
#define ui_begin g_ui.begin
#define ui_end g_ui.end
#define ui_request_frame g_ui.request_frame
#define ui_redraw_all g_ui.redraw_all
#define ui_grab g_ui.grab
#define ui_grabbed g_ui.grabbed
#define ui_motion g_ui.motion
#define ui_buttons g_ui.buttons
#define ui_clear g_ui.clear
#define ui_fill g_ui.fill
#define ui_frame g_ui.frame
#define ui_hline g_ui.hline
#define ui_text g_ui.text
#define ui_text_mono g_ui.text_mono
#define ui_text_width g_ui.text_width
#define ui_text_center g_ui.text_center
#define ui_icon16 g_ui.icon16
#define ui_blit g_ui.blit
#define ui_label g_ui.label
#define ui_label_muted g_ui.label_muted
#define ui_button g_ui.button
#define ui_button_accent g_ui.button_accent
#define ui_checkbox g_ui.checkbox
#define ui_slider g_ui.slider
#define ui_progress g_ui.progress
#define ui_textbox g_ui.textbox
#define ui_password g_ui.password
#define ui_focus_next_textbox g_ui.focus_next_textbox
#define ui_list g_ui.list
#define ui_mouse_x g_ui.mouse_x
#define ui_mouse_y g_ui.mouse_y
#define ui_mouse_down g_ui.mouse_down
#define ui_mouse_clicked g_ui.mouse_clicked
#define ui_double_clicked g_ui.double_clicked
#define ui_scroll g_ui.scroll
#define ui_key_pressed g_ui.key_pressed
#define ui_mods g_ui.mods
#define ui_typed g_ui.typed
#define ui_hit g_ui.hit
#define ui_focused g_ui.focused
#define ui_command g_ui.command
#define ui_raw_keys           g_ui.raw_keys
#define ui_app_id g_ui.app_id
#define ui_open_request g_ui.open_request
#define ui_hand_over g_ui.hand_over
#define ui_cursor             g_ui.cursor
#define ui_itoa g_ui.itoa
#define ui_fmt_time g_ui.fmt_time
#define ui_icon g_ui.icon
#define ui_notify_action g_ui.notify_action

#endif
