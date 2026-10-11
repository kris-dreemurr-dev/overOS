#include "settings.h"
#include "syscalls.h"

int  g_tz_offset = 3;
int  g_layout_ru = 1;
int  g_brightness = 100;
int  g_volume = 100;
int  g_audio_output = 0; // 0 = HDA_OUT_AUTO
char g_wallpaper_path[64] = "/sys/wallpaper.bmp";

static int json_get_int(const char* json, const char* key, int default_val) {
    const char* p = json;
    while (*p) {
        const char* p2 = p;
        const char* k2 = key;
        while (*p2 && *k2 && *p2 == *k2) { p2++; k2++; }
        if (!*k2) {
            while (*p2 && *p2 != ':') p2++;
            if (!*p2) return default_val;
            p2++;
            while (*p2 == ' ' || *p2 == '\t' || *p2 == '\r' || *p2 == '\n') p2++;
            int v = 0;
            while (*p2 >= '0' && *p2 <= '9') {
                v = v * 10 + (*p2 - '0');
                p2++;
            }
            return v;
        }
        p++;
    }
    return default_val;
}

static void json_get_string(const char* json, const char* key, char* out, int max_len, const char* default_val) {
    const char* p = json;
    while (*p) {
        const char* p2 = p;
        const char* k2 = key;
        while (*p2 && *k2 && *p2 == *k2) { p2++; k2++; }
        if (!*k2) {
            while (*p2 && *p2 != ':') p2++;
            if (!*p2) break;
            p2++;
            while (*p2 && *p2 != '\"') p2++;
            if (*p2 == '\"') p2++;
            int idx = 0;
            while (*p2 && *p2 != '\"' && idx < max_len - 1) {
                out[idx++] = *p2++;
            }
            out[idx] = '\0';
            return;
        }
        p++;
    }
    int idx = 0;
    while (default_val[idx] && idx < max_len - 1) {
        out[idx] = default_val[idx];
        idx++;
    }
    out[idx] = '\0';
}

void settings_load(void) {
    int fd = sys_open("/configs/desktop.json", 0);
    if (fd < 0) return;

    char buf[512];
    int rd = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (rd <= 0) return;
    buf[rd] = '\0';

    g_tz_offset    = json_get_int(buf, "\"timezone\"", 3);
    g_layout_ru    = json_get_int(buf, "\"langlayout\"", 1);
    g_brightness   = json_get_int(buf, "\"brightness\"", 100);
    g_volume       = json_get_int(buf, "\"volume\"", 100);
    g_audio_output = json_get_int(buf, "\"audio_output\"", 0);
    json_get_string(buf, "\"wallpaper\"", g_wallpaper_path, sizeof(g_wallpaper_path), "/sys/wallpaper.bmp");
}

void settings_save(void) {
    int fd = sys_open("/configs/desktop.json", 1);
    if (fd < 0) return;

    char buf[512];
    int p = 0;
    const char* s1 = "{\n  \"timezone\": "; while (*s1) buf[p++] = *s1++;
    buf[p++] = '0' + (g_tz_offset % 10);
    const char* s2 = ",\n  \"langlayout\": "; while (*s2) buf[p++] = *s2++;
    buf[p++] = '0' + (g_layout_ru ? 1 : 0);
    const char* s3 = ",\n  \"brightness\": "; while (*s3) buf[p++] = *s3++;

    if (g_brightness == 100) {
        buf[p++] = '1'; buf[p++] = '0'; buf[p++] = '0';
    } else {
        buf[p++] = '0' + (g_brightness / 10);
        buf[p++] = '0' + (g_brightness % 10);
    }

    const char* s_vol = ",\n  \"volume\": "; while (*s_vol) buf[p++] = *s_vol++;
    if (g_volume == 100) {
        buf[p++] = '1'; buf[p++] = '0'; buf[p++] = '0';
    } else {
        buf[p++] = '0' + (g_volume / 10);
        buf[p++] = '0' + (g_volume % 10);
    }

    const char* s_out = ",\n  \"audio_output\": "; while (*s_out) buf[p++] = *s_out++;
    buf[p++] = '0' + (g_audio_output % 10);

    const char* s4 = ",\n  \"wallpaper\": \""; while (*s4) buf[p++] = *s4++;
    int wp = 0; while (g_wallpaper_path[wp]) buf[p++] = g_wallpaper_path[wp++];
    const char* s5 = "\"\n}\n"; while (*s5) buf[p++] = *s5++;
    buf[p] = '\0';

    sys_write(fd, buf, p);
    sys_close(fd);
}