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

#define MAX_MENU_APPS 12

typedef struct {
    char name[32];
    char path[48];
} menu_app_t;

extern int        g_active_popup;
extern uint32_t   g_popup_timer;
extern int        g_menu_open;
extern int        g_brightness_dragging;
extern menu_app_t g_menu_apps[MAX_MENU_APPS];
extern int        g_menu_app_count;
extern int        g_power_menu_open;
extern sys_net_info_t g_net_info;
extern sys_hda_outputs_t g_audio_outputs;
extern int               g_audio_status;
extern int g_net_driver_ok;
extern int g_volume_dragging;

void taskbar_refresh_menu_apps(void);
void taskbar_render(int mouse_x, int mouse_y, const rtc_time_t* t);
int  taskbar_handle_click(int mouse_x, int mouse_y);
int taskbar_is_inside_menu(int mx, int my);
int taskbar_is_inside_popup(int mx, int my);
int launch_file_safe(const char* filepath);
void taskbar_report_child_exit(int pid, int code);
void show_error_dialog(const char* msg);

#endif