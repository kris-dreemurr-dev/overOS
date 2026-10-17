#ifndef USERS_H
#define USERS_H

#include <stdint.h>

// =============================================================================
// Пользователи и права доступа (DAC), UNIX-way
//   /etc/passwd  name:x:uid:gid:gecos:home:shell
//   /etc/group   name:x:gid:
//   /etc/shadow  name:$ov$<salt>$<sha256(salt+password) hex>:   ('' = без пароля, '!' = заблокирован)
//   /etc/perms   <path> <uid> <gid> <octal mode>   — владельцы и права (в FAT32 их хранить негде)
// Права файла без своей строки в /etc/perms наследуются от ближайшего родителя.
// =============================================================================

#define UID_ROOT     0
#define UID_NOBODY   (-1)    // TTY до входа в систему

#define PERM_R 4
#define PERM_W 2
#define PERM_X 1

#define USER_NAME_MAX 32
#define USER_PATH_MAX 64
#define PERM_PATH_MAX 128

typedef struct {
    char name[USER_NAME_MAX];
    int  uid;
    int  gid;
    char home[USER_PATH_MAX];
} user_info_t;

typedef struct {
    int uid;
    int gid;
    int mode;    // 0..0777
} perm_info_t;

// Загрузка /etc/* (после монтирования ФС). Нет /etc/passwd — встроенный root без пароля.
void users_init(void);

// Проверка пароля: uid или -1
int  users_auth(const char* name, const char* pass);
int  users_find_by_name(const char* name, user_info_t* out);
int  users_find_by_uid(int uid, user_info_t* out);
int  users_group_name(int gid, char* out, int max);
int  users_set_password(const char* name, const char* pass);
int  users_add(const char* name, const char* pass, int admin);   // uid или -1; admin — в группу wheel
int  users_in_group(int uid, const char* group);
int  users_is_admin(int uid);                          // root или участник wheel (может sudo)
int  users_lock(const char* name);                     // '!' в shadow: вход по паролю запрещён
int  users_valid_name(const char* name);                // a-z 0-9 _ - (не с '-')

// Первоначальная настройка (OOBE): нужна, если флаг /etc/oobe не выставлен
// и в системе нет ни одного обычного пользователя (uid >= 1000)
int  users_needs_oobe(void);
void users_finish_oobe(void);

// Системные язык, часовой пояс и раскладки (/etc/locale.conf, /etc/timezone, /etc/vconsole.conf)
#define SYS_LANG_RU 0
#define SYS_LANG_EN 1

typedef struct {
    const char* name;      // "Europe/Moscow"
    const char* city_ru;   // "Москва"
    int         offset;    // часы от UTC
} tz_entry_t;

extern const tz_entry_t g_timezones[];
extern const int        g_timezone_count;

int  users_sys_lang(void);
int  users_sys_tz_index(void);
int  users_sys_tz_offset(void);
int  users_sys_layout2(void);                          // 1 — есть вторая раскладка (RU)
// -1 — не менять соответствующий параметр
void users_set_system(int lang, int tz_index, int layout2);
// Скорость указателя (percent 25..400, accel 0/1) -> драйвер + /etc/mouse.conf
void users_set_mouse(int speed, int accel);

// Абсолютный путь в нижнем регистре без "." / ".." / "//". base — текущий каталог.
void perm_normalize(const char* path, const char* base, char* out, int max);

// Права на уже нормализованный путь
void perm_get(const char* npath, perm_info_t* out);

// Проверки (path — как пришёл от пользователя, base — его текущий каталог).
// Кроме самого объекта проверяется право X на все каталоги по пути. root (uid 0) проходит всегда.
int  perm_check(int uid, int gid, const char* path, const char* base, int want);
int  perm_check_parent(int uid, int gid, const char* path, const char* base, int want);

// Изменения, сохраняются в /etc/perms. Вызывать БЕЗ взятого fs_lock.
int  perm_set(const char* path, const char* base, int uid, int gid, int mode);
void perm_on_create(int uid, int gid, const char* path, const char* base, int is_dir);
void perm_on_remove(const char* path, const char* base);

// "drwxr-xr-x"
void perm_format_mode(int mode, int is_dir, char out[11]);

// Учётные данные текущей задачи (kernel-задачи = root)
int  cred_uid(void);
int  cred_gid(void);

#endif
