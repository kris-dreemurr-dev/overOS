#include "settings.h"
#include "syscalls.h"
#include "greeter.h"

int  g_tz_offset = 3;
int  g_tz_index  = -1;
int  g_layout_ru = 1;
int  g_sys_lang = 0;
int  g_has_layout2 = 1;
int  g_brightness = 100;
int  g_volume = 100;
int  g_audio_output = 0; // 0 = HDA_OUT_AUTO
char g_wallpaper_path[64] = "/sys/wallpaper.bmp";
// Закреплённые в панели программы: пути через ';' (как ярлыки на панели в KDE)
char g_pinned[224] = "/desprogs/ovfiles.prg;/desprogs/ovterm.prg;/desprogs/notepad.prg;/desprogs/ovimage.prg;/desprogs/ovplayer.prg;/desprogs/ovsettings.prg";

// Путь к конфигу текущего сеанса. До входа (greeter) — общесистемный, после входа
// ~/.config/desktop.json, чтобы обои и громкость у пользователей не были общими.
#define SYS_CFG "/configs/desktop.json"
static char s_cfg_path[96] = SYS_CFG;
static int  s_cfg_uid = -1;
static int  s_cfg_gid = -1;

static void cfg_append(char* dst, int* pos, int max, const char* src) {
    while (*src && *pos < max - 1) dst[(*pos)++] = *src++;
    dst[*pos] = '\0';
}

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
            int v = 0, neg = 0;
            if (*p2 == '-') { neg = 1; p2++; }   // часовые пояса западнее UTC
            while (*p2 >= '0' && *p2 <= '9') {
                v = v * 10 + (*p2 - '0');
                p2++;
            }
            return neg ? -v : v;
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

// Создание ~/.config для конфига сеанса (desktop работает как root, поэтому
// после создания отдаём каталог и файл владельцу, иначе пользователь их не прочитает)
static void settings_make_user_dir(void) {
    if (s_cfg_uid < 0) return;
    char dir[96];
    int pos = 0;
    dir[0] = '\0';
    cfg_append(dir, &pos, sizeof(dir), "/home/");
    cfg_append(dir, &pos, sizeof(dir), g_session_name);
    sys_mkdir(dir);
    sys_chown(dir, s_cfg_uid, s_cfg_gid);
    cfg_append(dir, &pos, sizeof(dir), "/.config");
    sys_mkdir(dir);
    sys_chown(dir, s_cfg_uid, s_cfg_gid);
    sys_chmod(dir, 0755);
}

void settings_load(void) {
    // Системные язык и раскладки (/etc) — общие для всех пользователей
    sys_settings_t sys;
    if (sys_get_system(&sys) == 0) {
        g_sys_lang = sys.lang;
        g_has_layout2 = sys.layout2;
        g_tz_index = sys.tz_index;
        if (sys.tz_index >= 0) g_tz_offset = sys.tz_offset;
    }

    int fd = sys_open(s_cfg_path, 0);
    if (fd < 0) return;

    char buf[1024];
    int rd = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (rd <= 0) return;
    buf[rd] = '\0';

    // пояс больше не читаем из json: это системная настройка (/etc/timezone)
    g_layout_ru    = json_get_int(buf, "\"langlayout\"", 1);
    if (!g_has_layout2) g_layout_ru = 0;   // второй раскладки нет — только английская
    g_brightness   = json_get_int(buf, "\"brightness\"", 100);
    g_volume       = json_get_int(buf, "\"volume\"", 100);
    g_audio_output = json_get_int(buf, "\"audio_output\"", 0);
    json_get_string(buf, "\"wallpaper\"", g_wallpaper_path, sizeof(g_wallpaper_path), "/sys/wallpaper.bmp");
    json_get_string(buf, "\"pinned\"", g_pinned, sizeof(g_pinned), g_pinned);
}

void settings_save(void) {
    int fd = sys_open(s_cfg_path, 1);
    if (fd < 0) {
        // Первый запуск сеанса: ~/.config могло ещё не быть
        settings_make_user_dir();
        fd = sys_open(s_cfg_path, 1);
    }
    if (fd < 0) return;

    char buf[1024];
    int p = 0;
    const char* s2 = "{\n  \"langlayout\": "; while (*s2) buf[p++] = *s2++;
    buf[p++] = '0' + (g_layout_ru ? 1 : 0);
    const char* s3 = ",\n  \"brightness\": "; while (*s3) buf[p++] = *s3++;

    if (g_brightness == 100) {
        buf[p++] = '1'; buf[p++] = '0'; buf[p++] = '0';
    } else {
        if (g_brightness >= 10) buf[p++] = '0' + (g_brightness / 10);
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
    const char* s6 = "\",\n  \"pinned\": \""; while (*s6) buf[p++] = *s6++;
    int pp = 0; while (g_pinned[pp] && p < (int)sizeof(buf) - 8) buf[p++] = g_pinned[pp++];
    const char* s5 = "\"\n}\n"; while (*s5) buf[p++] = *s5++;
    buf[p] = '\0';

    sys_write(fd, buf, p);
    sys_close(fd);
}

// Переключение на конфиг пользователя (после входа) или на общесистемный (greeter)
void settings_set_user(int uid, int gid) {
    int pos = 0;
    s_cfg_path[0] = '\0';
    if (uid >= 0 && g_session_name[0]) {
        s_cfg_uid = uid;
        s_cfg_gid = gid;
        cfg_append(s_cfg_path, &pos, sizeof(s_cfg_path), "/home/");
        cfg_append(s_cfg_path, &pos, sizeof(s_cfg_path), g_session_name);
        cfg_append(s_cfg_path, &pos, sizeof(s_cfg_path), "/.config/desktop.json");
    } else {
        s_cfg_uid = -1;
        s_cfg_gid = -1;
        cfg_append(s_cfg_path, &pos, sizeof(s_cfg_path), SYS_CFG);
    }
    settings_load();
}
