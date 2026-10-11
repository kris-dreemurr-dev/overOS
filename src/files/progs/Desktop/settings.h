#ifndef SETTINGS_H
#define SETTINGS_H

extern int  g_tz_offset;
extern int  g_layout_ru;
extern int  g_brightness;
extern int  g_volume;        // Громкость 0..100%
extern int  g_audio_output;  // Режим HDA_OUT_*
extern char g_wallpaper_path[64];

void settings_load(void);
void settings_save(void);

#endif