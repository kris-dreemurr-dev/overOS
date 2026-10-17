#ifndef TASKBAR_H
#define TASKBAR_H

#include "syscalls.h"

enum PopupType {
    POPUP_NONE = 0,
    POPUP_SOUND,
    POPUP_NET,
    POPUP_BRIGHTNESS,
    POPUP_CLOCK
};

#define MAX_MENU_APPS 16
#define CTX_TYPE_NONE      0
#define CTX_TYPE_DESKTOP   1
#define CTX_TYPE_EXPLORER  2
#define CTX_TYPE_TASKBAR   3      // меню значка в панели задач (закрепить/открепить)

typedef struct {
    char name[64];     // UTF-8: русское название занимает по два байта на букву
    char path[96];     // /desprogs/x.prg или /desprogs/Игра/x.prg
} menu_app_t;

typedef struct {
    int active;
    int type;
    int x, y;
    int w, h;
    int target_win_id;
    int dock_item;        // CTX_TYPE_TASKBAR: индекс значка в доке
} desktop_ctx_menu_t;

extern int        g_active_popup;
extern uint32_t   g_popup_timer;
extern int        g_menu_open;
extern int        g_brightness_dragging;
extern menu_app_t g_menu_apps[MAX_MENU_APPS];
extern int        g_menu_app_count;
extern int        g_power_menu_open;
extern int        g_logout_requested;   // "Выйти из сеанса" — обработка в главном цикле desktop.c
extern sys_net_info_t g_net_info;
extern sys_hda_outputs_t g_audio_outputs;
extern int               g_audio_status;
extern int g_net_driver_ok;
extern int g_volume_dragging;
extern desktop_ctx_menu_t g_ctx_menu;

void desktop_ctx_open(int mx, int my, int type, int win_id);
int  taskbar_ctx_at(int mx, int my);   // ПКМ по панели: открыть меню значка, 1 — открыли
void desktop_ctx_close(void);
void desktop_ctx_render(int mouse_x, int mouse_y);
int  desktop_ctx_handle_click(int mx, int my);
void taskbar_refresh_menu_apps(void);
void taskbar_render(int mouse_x, int mouse_y, const rtc_time_t* t);
int  taskbar_handle_click(int mouse_x, int mouse_y);
int taskbar_is_inside_menu(int mx, int my);
int taskbar_is_inside_popup(int mx, int my);
int taskbar_popup_scroll(int mx, int my, int wheel);
int taskbar_popup_drag(int my, int pressed);
int launch_file_safe(const char* filepath);
void taskbar_report_child_exit(int pid, int code);
void show_error_dialog(const char* msg);

#endif