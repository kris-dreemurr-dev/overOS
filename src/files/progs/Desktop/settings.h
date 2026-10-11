#ifndef SETTINGS_H
#define SETTINGS_H

extern int  g_tz_offset;
extern int  g_layout_ru;
extern int  g_brightness;
extern char g_wallpaper_path[64];

void settings_load(void);
void settings_save(void);

#endif