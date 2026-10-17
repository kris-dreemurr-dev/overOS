#ifndef SETTINGS_H
#define SETTINGS_H

extern int  g_tz_offset;
extern int  g_tz_index;    // индекс пояса в списке ядра, -1 — UTC
extern int  g_layout_ru;
extern int  g_brightness;
extern int  g_volume;        // Громкость 0..100%
extern int  g_audio_output;  // Режим HDA_OUT_*
extern char g_wallpaper_path[64];
extern char g_pinned[224];    // закреплённые программы панели, пути через ';'
extern int  g_sys_lang;       // язык системы (/etc/locale.conf): SYS_LANG_RU / SYS_LANG_EN
extern int  g_has_layout2;    // вторая раскладка (RU) включена в /etc/vconsole.conf

void settings_load(void);
void settings_set_user(int uid, int gid);   // конфиг сеанса: ~/.config/desktop.json
void settings_save(void);

#endif