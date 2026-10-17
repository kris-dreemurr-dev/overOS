#include "users.h"
#include "sha256.h"
#include "sched.h"
#include "../fs/fs.h"
#include "../drivers/mouse.h"

extern void kputs(const char* str, uint32_t color);
extern int  kernel_create_fs_entry(const char* path, int is_dir);

#define MAX_USERS      16
#define MAX_GROUPS     16
#define MAX_PERMS      128
#define ETC_BUF_SIZE   8192

typedef struct {
    char name[USER_NAME_MAX];
    int  uid;
    int  gid;
    char gecos[USER_NAME_MAX];
    char home[USER_PATH_MAX];
    char shell[USER_PATH_MAX];
    char hash[96];            // поле пароля из /etc/shadow
} user_rec_t;

typedef struct {
    char name[USER_NAME_MAX];
    int  gid;
    char members[128];        // дополнительные участники через запятую (wheel:x:10:kris,anna)
} group_rec_t;

typedef struct {
    char path[PERM_PATH_MAX]; // нормализованный
    int  uid, gid, mode;
} perm_rec_t;

static user_rec_t  g_users[MAX_USERS];
static int         g_user_count = 0;
static group_rec_t g_groups[MAX_GROUPS];
static int         g_group_count = 0;
static perm_rec_t  g_perms[MAX_PERMS];
static int         g_perm_count = 0;

static char g_etc_buf[ETC_BUF_SIZE];
static int  g_oobe_done = 0;     // /etc/oobe: "1" — первоначальная настройка пройдена

// Системные настройки (как в Linux): /etc/locale.conf, /etc/timezone, /etc/vconsole.conf
static int  g_sys_lang = SYS_LANG_RU;
static int  g_sys_tz_index = -1;          // индекс в g_timezones, -1 — UTC
static int  g_sys_layout2 = 1;            // вторая раскладка: 0 — только US, 1 — US + RU

// Часовые пояса "Страна/Город" (Region/City, как в tzdata) и смещение от UTC в часах
const tz_entry_t g_timezones[] = {
    { "America/Los_Angeles", "Лос-Анджелес",    -8 },
    { "America/New_York",    "Нью-Йорк",        -5 },
    { "America/Sao_Paulo",   "Сан-Паулу",       -3 },
    { "Europe/London",       "Лондон",           0 },
    { "Europe/Berlin",       "Берлин",           1 },
    { "Europe/Warsaw",       "Варшава",          1 },
    { "Europe/Kaliningrad",  "Калининград",      2 },
    { "Europe/Kyiv",         "Киев",             2 },
    { "Europe/Moscow",       "Москва",           3 },
    { "Europe/Minsk",        "Минск",            3 },
    { "Europe/Istanbul",     "Стамбул",          3 },
    { "Europe/Samara",       "Самара",           4 },
    { "Asia/Yerevan",        "Ереван",           4 },
    { "Asia/Yekaterinburg",  "Екатеринбург",     5 },
    { "Asia/Tashkent",       "Ташкент",          5 },
    { "Asia/Almaty",         "Алматы",           5 },
    { "Asia/Omsk",           "Омск",             6 },
    { "Asia/Novosibirsk",    "Новосибирск",      7 },
    { "Asia/Krasnoyarsk",    "Красноярск",       7 },
    { "Asia/Irkutsk",        "Иркутск",          8 },
    { "Asia/Shanghai",       "Шанхай",           8 },
    { "Asia/Yakutsk",        "Якутск",           9 },
    { "Asia/Tokyo",          "Токио",            9 },
    { "Asia/Vladivostok",    "Владивосток",     10 },
    { "Asia/Magadan",        "Магадан",         11 },
    { "Asia/Kamchatka",      "Петропавловск-Камчатский", 12 },
};
const int g_timezone_count = (int)(sizeof(g_timezones) / sizeof(g_timezones[0]));

// ----------------------------------------------------------------------------
// Строки
// ----------------------------------------------------------------------------
static int u_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static int u_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void u_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void u_append(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void u_append_int(char* buf, int* pos, int max, int v, int base) {
    char tmp[16];
    int n = 0;
    if (v < 0) { u_append(buf, pos, max, "-"); v = -v; }
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v && n < 15);
    while (n > 0 && *pos < max - 1) buf[(*pos)++] = tmp[--n];
    buf[*pos] = '\0';
}

static int u_atoi(const char* s, int base) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    return neg ? -v : v;
}

// Делит строку на поля по sep (на месте). Возвращает число полей.
static int u_split(char* line, char sep, char** fields, int max_fields) {
    int n = 0;
    fields[n++] = line;
    for (char* p = line; *p && n < max_fields; p++) {
        if (*p == sep) { *p = '\0'; fields[n++] = p + 1; }
    }
    return n;
}

// Следующая строка буфера (на месте), NULL — конец
static char* u_next_line(char** cursor) {
    char* s = *cursor;
    if (!s || !*s) return 0;
    char* e = s;
    while (*e && *e != '\n') e++;
    if (*e == '\n') { *e = '\0'; *cursor = e + 1; }
    else *cursor = e;
    if (e > s && e[-1] == '\r') e[-1] = '\0';
    return s;
}

// ----------------------------------------------------------------------------
// Файлы /etc
// ----------------------------------------------------------------------------
// Резервная копия "<файл>-" (как /etc/passwd- в Linux): запись в FAT32 — "удалить и создать",
// и если её прервать (выдернули флешку), файл пропадёт; тогда данные берутся из копии.
static void etc_backup_name(const char* path, char* out, int max) {
    int i = 0;
    while (path[i] && i < max - 2) { out[i] = path[i]; i++; }
    out[i++] = '-';
    out[i] = '\0';
}

static int etc_read_one(const char* path) {
    fs_lock();
    int n = fs_read_file(path, g_etc_buf, ETC_BUF_SIZE - 1);
    fs_unlock();
    if (n < 0) n = 0;
    g_etc_buf[n] = '\0';
    return n;
}

static int etc_read(const char* path) {
    int n = etc_read_one(path);
    if (n > 0) return n;
    char bak[64];
    etc_backup_name(path, bak, sizeof(bak));
    return etc_read_one(bak);
}

static int etc_write(const char* path, const char* data, int len) {
    char bak[64];
    etc_backup_name(path, bak, sizeof(bak));
    fs_lock();
    fs_write_file(bak, data, (uint32_t)len);           // сначала копия, потом сам файл:
    int ok = fs_write_file(path, data, (uint32_t)len); // в любой момент хотя бы один из них цел
    fs_unlock();
    return ok;
}

static void load_passwd(void) {
    g_user_count = 0;
    if (etc_read("/etc/passwd") <= 0) return;
    char* cur = g_etc_buf;
    char* line;
    while ((line = u_next_line(&cur)) && g_user_count < MAX_USERS) {
        if (line[0] == '#' || line[0] == '\0') continue;
        char* f[7];
        if (u_split(line, ':', f, 7) < 7) continue;
        user_rec_t* u = &g_users[g_user_count++];
        u_strcpy(u->name, f[0], USER_NAME_MAX);
        u->uid = u_atoi(f[2], 10);
        u->gid = u_atoi(f[3], 10);
        u_strcpy(u->gecos, f[4], USER_NAME_MAX);
        u_strcpy(u->home, f[5], USER_PATH_MAX);
        u_strcpy(u->shell, f[6], USER_PATH_MAX);
        u->hash[0] = '!';   // пока в shadow нет строки — вход запрещён
        u->hash[1] = '\0';
    }
}

static void load_shadow(void) {
    if (etc_read("/etc/shadow") <= 0) return;
    char* cur = g_etc_buf;
    char* line;
    while ((line = u_next_line(&cur))) {
        if (line[0] == '#' || line[0] == '\0') continue;
        char* f[3];
        if (u_split(line, ':', f, 3) < 2) continue;
        for (int i = 0; i < g_user_count; i++) {
            if (u_streq(g_users[i].name, f[0])) {
                u_strcpy(g_users[i].hash, f[1], sizeof(g_users[i].hash));
                break;
            }
        }
    }
}

// Поле участников группы (4-е), если оно есть
static const char* k_members(char** f, int n) {
    return (n >= 4 && f[3]) ? f[3] : "";
}

static void load_group(void) {
    g_group_count = 0;
    if (etc_read("/etc/group") <= 0) return;
    char* cur = g_etc_buf;
    char* line;
    while ((line = u_next_line(&cur)) && g_group_count < MAX_GROUPS) {
        if (line[0] == '#' || line[0] == '\0') continue;
        char* f[4] = {0, 0, 0, 0};
        int nf = u_split(line, ':', f, 4);
        if (nf < 3) continue;
        if (nf < 4) f[3] = 0;
        u_strcpy(g_groups[g_group_count].name, f[0], USER_NAME_MAX);
        g_groups[g_group_count].gid = u_atoi(f[2], 10);
        u_strcpy(g_groups[g_group_count].members, k_members(f, nf), sizeof(g_groups[0].members));
        g_group_count++;
    }
}

static void load_perms(void) {
    g_perm_count = 0;
    if (etc_read("/etc/perms") <= 0) return;
    char* cur = g_etc_buf;
    char* line;
    while ((line = u_next_line(&cur)) && g_perm_count < MAX_PERMS) {
        while (*line == ' ' || *line == '\t') line++;
        if (line[0] == '#' || line[0] == '\0') continue;
        char* f[4];
        int n = 0;
        // поля через пробелы/табы (несколько подряд)
        char* p = line;
        while (*p && n < 4) {
            while (*p == ' ' || *p == '\t') *p++ = '\0';
            if (!*p) break;
            f[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (n < 4) continue;
        perm_rec_t* r = &g_perms[g_perm_count++];
        perm_normalize(f[0], "/", r->path, PERM_PATH_MAX);
        r->uid  = u_atoi(f[1], 10);
        r->gid  = u_atoi(f[2], 10);
        r->mode = u_atoi(f[3], 8) & 0777;
    }
}

static void save_passwd_shadow_group(void) {
    static char out[ETC_BUF_SIZE];
    int pos;

    pos = 0; out[0] = '\0';
    for (int i = 0; i < g_user_count; i++) {
        user_rec_t* u = &g_users[i];
        u_append(out, &pos, ETC_BUF_SIZE, u->name);  u_append(out, &pos, ETC_BUF_SIZE, ":x:");
        u_append_int(out, &pos, ETC_BUF_SIZE, u->uid, 10); u_append(out, &pos, ETC_BUF_SIZE, ":");
        u_append_int(out, &pos, ETC_BUF_SIZE, u->gid, 10); u_append(out, &pos, ETC_BUF_SIZE, ":");
        u_append(out, &pos, ETC_BUF_SIZE, u->gecos); u_append(out, &pos, ETC_BUF_SIZE, ":");
        u_append(out, &pos, ETC_BUF_SIZE, u->home);  u_append(out, &pos, ETC_BUF_SIZE, ":");
        u_append(out, &pos, ETC_BUF_SIZE, u->shell); u_append(out, &pos, ETC_BUF_SIZE, "\n");
    }
    etc_write("/etc/passwd", out, pos);

    pos = 0; out[0] = '\0';
    for (int i = 0; i < g_user_count; i++) {
        u_append(out, &pos, ETC_BUF_SIZE, g_users[i].name); u_append(out, &pos, ETC_BUF_SIZE, ":");
        u_append(out, &pos, ETC_BUF_SIZE, g_users[i].hash); u_append(out, &pos, ETC_BUF_SIZE, ":\n");
    }
    etc_write("/etc/shadow", out, pos);

    pos = 0; out[0] = '\0';
    for (int i = 0; i < g_group_count; i++) {
        u_append(out, &pos, ETC_BUF_SIZE, g_groups[i].name); u_append(out, &pos, ETC_BUF_SIZE, ":x:");
        u_append_int(out, &pos, ETC_BUF_SIZE, g_groups[i].gid, 10); u_append(out, &pos, ETC_BUF_SIZE, ":");
        u_append(out, &pos, ETC_BUF_SIZE, g_groups[i].members); u_append(out, &pos, ETC_BUF_SIZE, "\n");
    }
    etc_write("/etc/group", out, pos);
}

static void save_perms(void) {
    static char out[ETC_BUF_SIZE];
    int pos = 0;
    out[0] = '\0';
    u_append(out, &pos, ETC_BUF_SIZE, "# path uid gid mode\n");
    for (int i = 0; i < g_perm_count; i++) {
        perm_rec_t* r = &g_perms[i];
        u_append(out, &pos, ETC_BUF_SIZE, r->path); u_append(out, &pos, ETC_BUF_SIZE, " ");
        u_append_int(out, &pos, ETC_BUF_SIZE, r->uid, 10); u_append(out, &pos, ETC_BUF_SIZE, " ");
        u_append_int(out, &pos, ETC_BUF_SIZE, r->gid, 10); u_append(out, &pos, ETC_BUF_SIZE, " 0");
        u_append_int(out, &pos, ETC_BUF_SIZE, r->mode, 8); u_append(out, &pos, ETC_BUF_SIZE, "\n");
    }
    etc_write("/etc/perms", out, pos);
}

// "KEY=value" из g_etc_buf (после etc_read): копия value или 0
static int etc_get(const char* key, char* out, int max) {
    int klen = u_strlen(key);
    char* cur = g_etc_buf;
    char* line;
    while ((line = u_next_line(&cur))) {
        int i = 0;
        while (i < klen && line[i] == key[i]) i++;
        if (i == klen && line[i] == '=') { u_strcpy(out, line + i + 1, max); return 1; }
    }
    return 0;
}

static int tz_find(const char* name) {
    for (int i = 0; i < g_timezone_count; i++)
        if (u_streq(g_timezones[i].name, name)) return i;
    return -1;
}

static void load_system_settings(void) {
    char v[64];
    if (etc_read("/etc/locale.conf") > 0 && etc_get("LANG", v, sizeof(v)))
        g_sys_lang = (v[0] == 'e' && v[1] == 'n') ? SYS_LANG_EN : SYS_LANG_RU;
    if (etc_read("/etc/timezone") > 0) {
        char* cur = g_etc_buf;
        char* line = u_next_line(&cur);
        if (line) g_sys_tz_index = tz_find(line);
    }
    if (etc_read("/etc/vconsole.conf") > 0 && etc_get("XKBLAYOUT", v, sizeof(v)))
        g_sys_layout2 = (v[0] == 'u' && v[1] == 's' && v[2] == ',');

    // Указатель: /etc/mouse.conf, по образцу остальных файлов в /etc
    if (etc_read("/etc/mouse.conf") > 0) {
        int speed = 100, accel = 1;
        if (etc_get("SPEED", v, sizeof(v))) {
            int n = 0;
            for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
            if (n >= 25 && n <= 400) speed = n;
        }
        if (etc_get("ACCEL", v, sizeof(v))) accel = (v[0] == '1');
        mouse_set_speed(speed, accel);
    }
}

// Скорость указателя: в памяти её держит драйвер, на диске -- /etc/mouse.conf
void users_set_mouse(int speed, int accel) {
    mouse_set_speed(speed, accel);
    char buf[64];
    int pos = 0;
    buf[0] = '\0';
    u_append(buf, &pos, sizeof(buf), "SPEED=");
    char num[8];
    int n = mouse_get_speed(), d = 0;
    char tmp[8];
    if (n == 0) tmp[d++] = '0';
    while (n > 0) { tmp[d++] = (char)('0' + n % 10); n /= 10; }
    int k = 0;
    while (d > 0) num[k++] = tmp[--d];
    num[k] = '\0';
    u_append(buf, &pos, sizeof(buf), num);
    u_append(buf, &pos, sizeof(buf), "\nACCEL=");
    u_append(buf, &pos, sizeof(buf), mouse_get_accel() ? "1" : "0");
    u_append(buf, &pos, sizeof(buf), "\n");
    etc_write("/etc/mouse.conf", buf, pos);
}

int users_sys_lang(void)     { return g_sys_lang; }
int users_sys_tz_index(void) { return g_sys_tz_index; }
int users_sys_tz_offset(void){ return g_sys_tz_index >= 0 ? g_timezones[g_sys_tz_index].offset : 0; }
int users_sys_layout2(void)  { return g_sys_layout2; }

void users_set_system(int lang, int tz_index, int layout2) {
    if (lang == SYS_LANG_RU || lang == SYS_LANG_EN) {
        g_sys_lang = lang;
        const char* s = (lang == SYS_LANG_EN) ? "LANG=en_US.UTF-8\n" : "LANG=ru_RU.UTF-8\n";
        etc_write("/etc/locale.conf", s, u_strlen(s));
    }
    if (tz_index >= 0 && tz_index < g_timezone_count) {
        g_sys_tz_index = tz_index;
        char buf[64];
        int pos = 0;
        buf[0] = '\0';
        u_append(buf, &pos, sizeof(buf), g_timezones[tz_index].name);
        u_append(buf, &pos, sizeof(buf), "\n");
        etc_write("/etc/timezone", buf, pos);
    }
    if (layout2 == 0 || layout2 == 1) {
        g_sys_layout2 = layout2;
        const char* s = layout2 ? "KEYMAP=us\nXKBLAYOUT=us,ru\n" : "KEYMAP=us\nXKBLAYOUT=us\n";
        etc_write("/etc/vconsole.conf", s, u_strlen(s));
    }
}

void users_init(void) {
    g_oobe_done = (etc_read("/etc/oobe") > 0 && g_etc_buf[0] == '1');
    load_system_settings();
    load_passwd();
    load_shadow();
    load_group();
    load_perms();

    if (g_user_count == 0) {
        // /etc/passwd нет (старый образ или файл потерян): root заблокирован, а не "без пароля" —
        // иначе удаление одного файла давало бы вход root без пароля. Доступ вернёт
        // первоначальная настройка: она создаст администратора.
        user_rec_t* r = &g_users[0];
        u_strcpy(r->name, "root", USER_NAME_MAX);
        r->uid = 0; r->gid = 0;
        u_strcpy(r->gecos, "root", USER_NAME_MAX);
        u_strcpy(r->home, "/root", USER_PATH_MAX);
        u_strcpy(r->shell, "tty", USER_PATH_MAX);
        r->hash[0] = '!';
        r->hash[1] = '\0';
        g_user_count = 1;
        g_oobe_done = 0;
        kputs("[users] /etc/passwd not found: root locked, starting first-time setup\n", 0xFFFF55);
    }
    if (g_group_count == 0) {
        u_strcpy(g_groups[0].name, "root", USER_NAME_MAX);
        g_groups[0].gid = 0;
        g_group_count = 1;
    }
}

// ----------------------------------------------------------------------------
// Пароли
// ----------------------------------------------------------------------------
static void hash_password(const char* salt, const char* pass, char hex[65]) {
    sha256_ctx_t ctx;
    uint8_t h[32];
    static const char digits[] = "0123456789abcdef";
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t*)salt, (size_t)u_strlen(salt));
    sha256_update(&ctx, (const uint8_t*)pass, (size_t)u_strlen(pass));
    sha256_final(&ctx, h);
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = digits[h[i] >> 4];
        hex[i * 2 + 1] = digits[h[i] & 0xF];
    }
    hex[64] = '\0';
}

// Сравнение за постоянное время — не выдаём по таймингу, сколько символов совпало
static int ct_equal(const char* a, const char* b) {
    int la = u_strlen(a), lb = u_strlen(b);
    int diff = la ^ lb;
    for (int i = 0; i < la && i < lb; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static int check_hash(const char* stored, const char* pass) {
    if (stored[0] == '\0') return 1;                 // пустое поле — без пароля
    if (stored[0] != '$') return 0;                  // '!' / '*' — заблокирован
    // $ov$<salt>$<hex>
    if (!(stored[1] == 'o' && stored[2] == 'v' && stored[3] == '$')) return 0;
    char salt[32];
    int i = 0;
    const char* s = stored + 4;
    while (s[i] && s[i] != '$' && i < 31) { salt[i] = s[i]; i++; }
    salt[i] = '\0';
    if (s[i] != '$') return 0;
    char hex[65];
    hash_password(salt, pass, hex);
    return ct_equal(hex, s + i + 1);
}

static void make_hash(const char* pass, char* out, int max) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t r = ((uint64_t)hi << 32) | lo;
    char salt[17];
    for (int i = 0; i < 16; i++) {
        salt[i] = "0123456789abcdef"[(r >> ((i * 4) % 64)) & 0xF];
        if (i == 7) r = r * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    salt[16] = '\0';
    char hex[65];
    hash_password(salt, pass, hex);
    int pos = 0;
    out[0] = '\0';
    u_append(out, &pos, max, "$ov$");
    u_append(out, &pos, max, salt);
    u_append(out, &pos, max, "$");
    u_append(out, &pos, max, hex);
}

static user_rec_t* find_user(const char* name) {
    for (int i = 0; i < g_user_count; i++)
        if (u_streq(g_users[i].name, name)) return &g_users[i];
    return 0;
}

static void fill_info(const user_rec_t* u, user_info_t* out) {
    if (!out) return;
    u_strcpy(out->name, u->name, USER_NAME_MAX);
    out->uid = u->uid;
    out->gid = u->gid;
    u_strcpy(out->home, u->home, USER_PATH_MAX);
}

int users_auth(const char* name, const char* pass) {
    user_rec_t* u = find_user(name);
    if (!u) {
        // Тратим столько же времени, сколько на настоящую проверку
        char hex[65];
        hash_password("0000000000000000", pass, hex);
        return -1;
    }
    return check_hash(u->hash, pass) ? u->uid : -1;
}

int users_find_by_name(const char* name, user_info_t* out) {
    user_rec_t* u = find_user(name);
    if (!u) return 0;
    fill_info(u, out);
    return 1;
}

int users_find_by_uid(int uid, user_info_t* out) {
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) { fill_info(&g_users[i], out); return 1; }
    }
    return 0;
}

int users_group_name(int gid, char* out, int max) {
    for (int i = 0; i < g_group_count; i++) {
        if (g_groups[i].gid == gid) { u_strcpy(out, g_groups[i].name, max); return 1; }
    }
    int pos = 0;
    out[0] = '\0';
    u_append_int(out, &pos, max, gid, 10);
    return 0;
}

int users_set_password(const char* name, const char* pass) {
    user_rec_t* u = find_user(name);
    if (!u) return 0;
    make_hash(pass, u->hash, sizeof(u->hash));
    save_passwd_shadow_group();
    return 1;
}

int users_valid_name(const char* n) {
    int len = u_strlen(n);
    if (len == 0 || len >= USER_NAME_MAX - 1) return 0;
    for (int i = 0; i < len; i++) {
        char c = n[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || (i == 0 && c == '-')) return 0;
    }
    return 1;
}

// Есть ли имя в списке "a,b,c"
static int list_has(const char* list, const char* name) {
    int n = u_strlen(name);
    const char* p = list;
    while (*p) {
        int k = 0;
        while (k < n && p[k] == name[k]) k++;
        if (k == n && (p[k] == ',' || p[k] == '\0')) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

int users_in_group(int uid, const char* group) {
    user_rec_t* u = 0;
    for (int i = 0; i < g_user_count; i++) if (g_users[i].uid == uid) { u = &g_users[i]; break; }
    if (!u) return 0;
    for (int i = 0; i < g_group_count; i++) {
        if (!u_streq(g_groups[i].name, group)) continue;
        return u->gid == g_groups[i].gid || list_has(g_groups[i].members, u->name);
    }
    return 0;
}

int users_is_admin(int uid) {
    return uid == UID_ROOT || users_in_group(uid, "wheel");
}

int users_lock(const char* name) {
    user_rec_t* u = find_user(name);
    if (!u) return 0;
    u->hash[0] = '!';
    u->hash[1] = '\0';
    save_passwd_shadow_group();
    return 1;
}

int users_add(const char* name, const char* pass, int admin) {
    if (!users_valid_name(name) || find_user(name)) return -1;
    if (g_user_count >= MAX_USERS || g_group_count >= MAX_GROUPS) return -1;

    int uid = 1000;
    for (int i = 0; i < g_user_count; i++)
        if (g_users[i].uid >= uid) uid = g_users[i].uid + 1;

    user_rec_t* u = &g_users[g_user_count++];
    u_strcpy(u->name, name, USER_NAME_MAX);
    u->uid = uid;
    u->gid = uid;                          // личная группа пользователя, как в Linux
    u_strcpy(u->gecos, name, USER_NAME_MAX);
    int pos = 0;
    u->home[0] = '\0';
    u_append(u->home, &pos, USER_PATH_MAX, "/home/");
    u_append(u->home, &pos, USER_PATH_MAX, name);
    u_strcpy(u->shell, "tty", USER_PATH_MAX);
    make_hash(pass, u->hash, sizeof(u->hash));

    u_strcpy(g_groups[g_group_count].name, name, USER_NAME_MAX);
    g_groups[g_group_count].gid = uid;
    g_groups[g_group_count].members[0] = '\0';
    g_group_count++;

    // Администратор: в группу wheel (sudo), как первый пользователь в установщиках Linux
    if (admin) {
        group_rec_t* wheel = 0;
        for (int i = 0; i < g_group_count; i++) if (u_streq(g_groups[i].name, "wheel")) wheel = &g_groups[i];
        if (!wheel && g_group_count < MAX_GROUPS) {
            wheel = &g_groups[g_group_count++];
            u_strcpy(wheel->name, "wheel", USER_NAME_MAX);
            wheel->gid = 10;
            wheel->members[0] = '\0';
        }
        if (wheel) {
            int pos = u_strlen(wheel->members);
            if (pos > 0) u_append(wheel->members, &pos, sizeof(wheel->members), ",");
            u_append(wheel->members, &pos, sizeof(wheel->members), name);
        }
    }

    save_passwd_shadow_group();

    kernel_create_fs_entry("/home", 1);    // нет — создастся, есть — ничего не случится
    kernel_create_fs_entry(u->home, 1);
    perm_set(u->home, "/", uid, uid, 0700);
    return uid;
}

int users_needs_oobe(void) {
    if (g_oobe_done) return 0;
    for (int i = 0; i < g_user_count; i++)
        if (g_users[i].uid >= 1000) return 0;
    return 1;
}

void users_finish_oobe(void) {
    g_oobe_done = 1;
    etc_write("/etc/oobe", "1\n", 2);
}

// ----------------------------------------------------------------------------
// Пути и права
// ----------------------------------------------------------------------------
void perm_normalize(const char* path, const char* base, char* out, int max) {
    char tmp[PERM_PATH_MAX * 2];
    int pos = 0;
    tmp[0] = '\0';
    if (!path) path = "";
    if (path[0] != '/') {
        u_append(tmp, &pos, sizeof(tmp), base && base[0] ? base : "/");
        u_append(tmp, &pos, sizeof(tmp), "/");
    }
    u_append(tmp, &pos, sizeof(tmp), path);

    // Разбор по компонентам со стеком
    int o = 0;
    out[o++] = '/';
    out[o] = '\0';
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            // out хранится без завершающего '/': отрезаем последний компонент
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;
            out[o] = '\0';
            continue;
        }
        if (o > 1 && o < max - 1) out[o++] = '/';
        for (int i = 0; i < len && o < max - 1; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c += 32;         // FAT не различает регистр
            out[o++] = c;
        }
        out[o] = '\0';
    }
    out[o] = '\0';
}

static int perm_find_exact(const char* npath) {
    for (int i = 0; i < g_perm_count; i++)
        if (u_streq(g_perms[i].path, npath)) return i;
    return -1;
}

// Съёмный накопитель в /media/<имя>: у FAT нет владельцев и прав, и Linux в
// таком случае монтирует vfat с uid вошедшего пользователя. Мы проще: всё
// внутри тома открыто на чтение и запись (0777), а сам /media -- 0755. В
// /etc/perms корневой флешки такие пути не пишем: через минуту флешку вынут.
static int perm_is_removable(const char* npath) {
    const char* m = "/media/";
    int i = 0;
    while (m[i] && npath[i] == m[i]) i++;
    return !m[i] && npath[i];
}

void perm_get(const char* npath, perm_info_t* out) {
    if (perm_is_removable(npath)) {
        out->uid = 0; out->gid = 0; out->mode = 0777;
        return;
    }
    char cur[PERM_PATH_MAX];
    u_strcpy(cur, npath, PERM_PATH_MAX);
    while (1) {
        int idx = perm_find_exact(cur);
        if (idx >= 0) {
            out->uid  = g_perms[idx].uid;
            out->gid  = g_perms[idx].gid;
            out->mode = g_perms[idx].mode;
            return;
        }
        int len = u_strlen(cur);
        if (len <= 1) break;
        while (len > 1 && cur[len - 1] != '/') len--;
        if (len > 1) len--;          // убираем '/'
        cur[len] = '\0';
    }
    // Ничего не нашлось даже для "/": корень принадлежит root, rwxr-xr-x
    out->uid = 0;
    out->gid = 0;
    out->mode = 0755;
}

static int perm_bits(int uid, int gid, const perm_info_t* p) {
    if (uid == p->uid) return (p->mode >> 6) & 7;
    if (gid == p->gid) return (p->mode >> 3) & 7;
    return p->mode & 7;
}

// Право X на все каталоги-предки npath (как path lookup в Linux)
static int perm_walk_ok(int uid, int gid, const char* npath) {
    char cur[PERM_PATH_MAX];
    int len = u_strlen(npath);
    for (int i = 1; i < len; i++) {
        if (npath[i] != '/') continue;
        for (int k = 0; k < i; k++) cur[k] = npath[k];
        cur[i] = '\0';
        perm_info_t p;
        perm_get(cur, &p);
        if (!(perm_bits(uid, gid, &p) & PERM_X)) return 0;
    }
    perm_info_t root;
    perm_get("/", &root);
    return (perm_bits(uid, gid, &root) & PERM_X) != 0;
}

int perm_check(int uid, int gid, const char* path, const char* base, int want) {
    if (uid == UID_ROOT) return 1;
    if (uid < 0) return 0;
    char n[PERM_PATH_MAX];
    perm_normalize(path, base, n, PERM_PATH_MAX);
    if (!perm_walk_ok(uid, gid, n)) return 0;
    perm_info_t p;
    perm_get(n, &p);
    return (perm_bits(uid, gid, &p) & want) == want;
}

int perm_check_parent(int uid, int gid, const char* path, const char* base, int want) {
    if (uid == UID_ROOT) return 1;
    if (uid < 0) return 0;
    char n[PERM_PATH_MAX];
    perm_normalize(path, base, n, PERM_PATH_MAX);
    int len = u_strlen(n);
    while (len > 1 && n[len - 1] != '/') len--;
    if (len > 1) len--;
    n[len] = '\0';
    return perm_check(uid, gid, n, "/", want | PERM_X);
}

int perm_set(const char* path, const char* base, int uid, int gid, int mode) {
    char n[PERM_PATH_MAX];
    perm_normalize(path, base, n, PERM_PATH_MAX);
    if (perm_is_removable(n)) return 0;          // у FAT на флешке прав нет
    int idx = perm_find_exact(n);
    if (idx < 0) {
        if (g_perm_count >= MAX_PERMS) return 0;
        idx = g_perm_count++;
        u_strcpy(g_perms[idx].path, n, PERM_PATH_MAX);
    }
    g_perms[idx].uid  = uid;
    g_perms[idx].gid  = gid;
    g_perms[idx].mode = mode & 0777;
    save_perms();
    return 1;
}

void perm_on_create(int uid, int gid, const char* path, const char* base, int is_dir) {
    char n[PERM_PATH_MAX];
    perm_normalize(path, base, n, PERM_PATH_MAX);
    if (perm_is_removable(n)) return;
    perm_info_t inh;
    perm_get(n, &inh);
    // Владелец по наследству тот же — отдельная строка не нужна
    if (inh.uid == uid && inh.gid == gid) return;
    if (uid < 0) return;
    perm_set(n, "/", uid, gid, is_dir ? 0755 : 0644);
}

void perm_on_remove(const char* path, const char* base) {
    char n[PERM_PATH_MAX];
    perm_normalize(path, base, n, PERM_PATH_MAX);
    if (perm_is_removable(n)) return;
    int len = u_strlen(n);
    int changed = 0;
    for (int i = 0; i < g_perm_count; ) {
        const char* p = g_perms[i].path;
        int match = u_streq(p, n);
        if (!match) {   // содержимое удалённого каталога
            int k = 0;
            while (k < len && p[k] == n[k]) k++;
            match = (k == len && p[len] == '/');
        }
        if (match) {
            g_perms[i] = g_perms[--g_perm_count];
            changed = 1;
        } else {
            i++;
        }
    }
    if (changed) save_perms();
}

void perm_format_mode(int mode, int is_dir, char out[11]) {
    const char* rwx = "rwxrwxrwx";
    out[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; i++) out[i + 1] = (mode & (0400 >> i)) ? rwx[i] : '-';
    out[10] = '\0';
}

int cred_uid(void) {
    task_t* t = sched_get_current_task();
    return t ? t->uid : 0;
}

int cred_gid(void) {
    task_t* t = sched_get_current_task();
    return t ? t->gid : 0;
}
