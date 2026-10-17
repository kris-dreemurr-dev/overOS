#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
// Структуры мыши, времени и видео
// -----------------------------------------------------------------------------
typedef struct {
    int x, y;
    uint8_t buttons;
    int last_dx, last_dy;
} __attribute__((packed)) user_mouse_t;

typedef struct {
    uint8_t sec, min, hour;
    uint8_t day, month, year;
} __attribute__((packed)) rtc_time_t;

typedef struct {
    uint32_t width, height, pitch, bpp;
    uint64_t paddr;
} fb_var_info_t;

typedef struct {
    char name[32];
    char arch[16];
    char version[32];
} __attribute__((packed)) kernel_info_t;

#define NET_STATE_DISCONNECTED 0
#define NET_STATE_LOCAL_ONLY   1
#define NET_STATE_ONLINE       2


typedef struct {
    char     name[32];      // Имя файла
    uint8_t  attr;          // 0x10 = папка, 0x20 = файл
    uint32_t size;          // Размер в байтах
    uint32_t cluster;       // Стартовый кластер
    char     clean_name[64];
} __attribute__((packed)) sys_file_info_t;

typedef struct {
    uint32_t state;       // NET_STATE_*
    uint32_t ip;
    uint32_t gateway_ip;
    uint16_t link_speed;  // 10, 100, 1000 Mbps
} __attribute__((packed)) sys_net_info_t;

// -----------------------------------------------------------------------------
// Системные вызовы ядра overOS (int 0x80)
// -----------------------------------------------------------------------------

static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
    while (1);
}

// Ядро кладёт результат сисколла в RAX: без "+a" компилятор считал бы, что RAX остался равен
// номеру вызова, и при повторном sys_sleep в цикле мог бы отправить уже другое число (например, exit).
static inline void sys_sleep(uint32_t ms) {
    uint64_t r = 4;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(ms) : "memory");
}

static inline uint8_t sys_get_key(void) {
    uint64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6) : "memory");
    return (uint8_t)ret;
}

static inline int sys_get_mouse(user_mouse_t* ms) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(7), "b"(ms) : "memory");
    return ret;
}

// Смещение мыши с прошлого вызова, без упора в край экрана (для захвата мыши)
static inline void sys_get_mouse_delta(int* dx, int* dy, uint8_t* buttons) {
    uint64_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(23), "b"(dx), "c"(dy), "d"(buttons) : "memory");
    (void)r;
}

static inline int sys_get_time(rtc_time_t* t) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(8), "b"(t) : "memory");
    return ret;
}

static inline int sys_get_kernel_info(kernel_info_t* info) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(9), "b"(info) : "memory");
    return ret;
}

static inline int sys_open(const char* fn, int flags) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(10), "b"(fn), "c"(flags) : "memory");
    return ret;
}

static inline int sys_read(int fd, void* buf, int sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(11), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

static inline void sys_close(int fd) {
    uint64_t r = 12;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(fd) : "memory");
}

static inline int sys_write(int fd, const void* buf, uint32_t sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(13), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

// Позиция дескриптора (сисколл 19): нужна для частичного вывода кадра в /dev/fb0
static inline int64_t sys_lseek(int fd, int64_t off, int whence) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(19), "b"(fd), "c"(off), "d"(whence) : "memory");
    return ret;
}

static inline void* sys_sbrk(int64_t inc) {
    uint64_t cur = 0;
    __asm__ volatile ("int $0x80" : "=a"(cur) : "a"(14), "b"(0) : "memory");
    if (inc == 0) return (void*)cur;
    uint64_t res = 0;
    __asm__ volatile ("int $0x80" : "=a"(res) : "a"(14), "b"(cur + inc) : "memory");
    if (res == (uint64_t)-1) return (void*)(uintptr_t)-1;   // ядро отказало (нет памяти)
    return (void*)cur;
}

static inline int sys_ioctl(int fd, uint64_t req, void* arg) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(15), "b"(fd), "c"(req), "d"(arg) : "memory");
    return ret;
}

static inline int sys_get_dir_files(const char* path, sys_file_info_t* buf, int max_files) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(16), "b"(path), "c"(buf), "d"(max_files) : "memory");
    return ret;
}

static inline int sys_kill(int pid, int sig) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(33), "b"(pid), "c"(sig) : "memory");
    return ret;
}

static inline int sys_waitpid(int pid, int* status, int options) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(32), "b"(pid), "c"(status), "d"(options) : "memory");
    return ret;
}

static inline void sys_system_reboot(void) {
    __asm__ volatile ("int $0x80" : : "a"(50) : "memory");
    while (1);
}

static inline void sys_system_shutdown(void) {
    __asm__ volatile ("int $0x80" : : "a"(51) : "memory");
    while (1);
}

static inline int sys_get_net_info(sys_net_info_t* info) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(60), "b"(info) : "memory");
    return ret;
}

// -----------------------------------------------------------------------------
// Аудиоподсистема Intel HDA (ABI Ring 3)
// -----------------------------------------------------------------------------
#define HDA_STATUS_NOT_FOUND   0
#define HDA_STATUS_INITIALIZED 1
#define HDA_STATUS_PLAYING     2

#define HDA_OUT_AUTO       0   // Автовыбор (динамики / наушники / HDMI)
#define HDA_OUT_SPEAKERS   1   // Только встроенные динамики
#define HDA_OUT_HEADPHONES 2   // Только наушники (3.5 мм Jack)
#define HDA_OUT_HDMI       3   // Только HDMI / DisplayPort

typedef struct {
    int controllers;
    int codecs;
    int has_speakers;
    int has_headphones;
    int has_lineout;
    int has_hdmi;
    int has_spdif;
    int headphones_plugged;
    int lineout_plugged;
    int hdmi_connected;
    int analog_playback;
    int hdmi_playback;
} __attribute__((packed)) sys_hda_outputs_t;

static inline int sys_audio_get_status(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(0) : "memory");
    return ret;
}

static inline int sys_audio_get_outputs(sys_hda_outputs_t* out) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(1), "c"(out) : "memory");
    return ret;
}

static inline int sys_audio_set_output(int mode) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(2), "c"(mode) : "memory");
    return ret;
}

static inline int sys_audio_get_output(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(3) : "memory");
    return ret;
}

static inline void sys_audio_play_test(void) {
    uint64_t r = 70;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(4) : "memory");
}

static inline void sys_audio_stop(void) {
    uint64_t r = 70;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(5) : "memory");
}

// Куда звук реально уходит: 0 никуда, 1 аналоговый выход, 2 HDMI
static inline int sys_audio_get_target(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(17) : "memory");
    return ret;
}

static inline int sys_audio_jack_poll(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(6) : "memory");
    return ret;
}

// Громкость и mute драйвера HDA (программная, 0..100) — те же подкоманды сисколла 70, что использует плеер
static inline void sys_audio_set_volume(int pct) {
    uint64_t r = 70;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(14), "c"((uint64_t)(uint32_t)pct) : "memory");
}

static inline int sys_audio_get_volume(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(70), "b"(15) : "memory");
    return ret;
}

static inline void sys_audio_set_mute(int m) {
    uint64_t r = 70;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(16), "c"((uint64_t)(uint32_t)m) : "memory");
}

// Сколько тактов TSC в миллисекунде (калибровка ядра, 0 = нет). Нужно, чтобы мерить время в Ring 3 через rdtsc.
static inline uint64_t sys_get_tsc_per_ms(void) {
    uint64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(22) : "memory");
    return ret;
}

static inline int sys_get_arg(char* out, int max_len) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(49), "b"(out), "c"(max_len) : "memory");
    return ret;
}

// -----------------------------------------------------------------------------
// Колесо мыши: щелчки с прошлого вызова (+ вниз, - вверх)
static inline int sys_get_wheel(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(24) : "memory");
    return ret;
}

// Пользователи и права (UNIX DAC)
// -----------------------------------------------------------------------------
typedef struct {
    char name[32];
    int  uid;
    int  gid;
    char home[64];
} user_info_t;

static inline int sys_getuid(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(80) : "memory");
    return ret;
}

static inline int sys_getgid(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(81) : "memory");
    return ret;
}

static inline int sys_setuid(int uid) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(82), "b"(uid) : "memory");
    return ret;
}

static inline int sys_setgid(int gid) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(83), "b"(gid) : "memory");
    return ret;
}

// Проверка пароля ядром: uid или -1 (после неудачи ядро держит паузу ~1 с)
static inline int sys_auth(const char* name, const char* pass) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(84), "b"(name), "c"(pass) : "memory");
    return ret;
}

static inline int sys_getpwuid(int uid, user_info_t* out) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(85), "b"(uid), "c"(out) : "memory");
    return ret;
}

// Первоначальная настройка (OOBE)
// admin = 1: пользователь в группе wheel (может sudo / окно пароля администратора)
static inline int sys_useradd(const char* name, const char* pass, int admin) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(90), "b"(name), "c"(pass), "d"(admin) : "memory");
    return ret;
}

// Повышение прав процесса до root паролем ТЕКУЩЕГО пользователя (он должен быть в wheel)
static inline int sys_sudo(const char* pass) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(93), "b"(pass) : "memory");
    return ret;
}

static inline int sys_is_admin(int uid) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(94), "b"(uid) : "memory");
    return ret;
}

#define SYS_LANG_RU 0
#define SYS_LANG_EN 1

typedef struct {
    int lang;        // SYS_LANG_*
    int tz_index;    // индекс в списке поясов ядра, -1 — UTC
    int tz_offset;   // часы от UTC
    int layout2;     // 1 — вторая раскладка (RU) включена
} sys_settings_t;

static inline int sys_get_system(sys_settings_t* out) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(95), "b"(out) : "memory");
    return ret;
}

// -1 — не менять параметр. Только root.
static inline int sys_set_system(int lang, int tz_index, int layout2) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(96), "b"(lang), "c"(tz_index), "d"(layout2) : "memory");
    return ret;
}

typedef struct {
    char name[32];   // "Europe/Moscow"
    char city[64];   // "Москва"
    int  offset;
} tz_info_t;

// Возвращает число поясов; out заполняется, если index в списке
static inline int sys_tz_info(int index, tz_info_t* out) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(97), "b"(index), "c"(out) : "memory");
    return ret;
}

static inline int sys_lock_user(const char* name) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(98), "b"(name) : "memory");
    return ret;
}

static inline int sys_oobe_state(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(91) : "memory");
    return ret;
}

static inline int sys_oobe_finish(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(92) : "memory");
    return ret;
}

// Смена пароля: root может без старого (old = "")
static inline int sys_passwd(const char* name, const char* new_pass, const char* old_pass) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(89), "b"(name), "c"(new_pass), "d"(old_pass) : "memory");
    return ret;
}

// Пользователь сеанса (greeter.c): программы desktop запускаются от его имени
extern int g_session_uid;
extern int g_session_gid;

// Запуск программы от имени пользователя сеанса (sys_spawn, без fork). <0 — код ошибки.
static inline int sys_spawn_as_session(const char* path, const char* arg) {
    int ret;
    uint64_t ug = ((uint64_t)(uint32_t)g_session_uid << 32) | (uint32_t)g_session_gid;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(109), "b"(path), "c"(arg ? arg : ""), "d"(ug) : "memory");
    return ret;
}

// Запуск программы с передачей пути к файлу как аргумента
static inline int sys_open_file(const char* app, const char* filepath) {
    return sys_spawn_as_session(app, filepath);
}


static inline int sys_mkdir(const char* path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(17), "b"(path) : "memory");
    return ret;
}

// Владелец и права файла (нужно для конфигов в домашнем каталоге: desktop — root)
static inline int sys_chown(const char* path, int uid, int gid) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(87), "b"(path), "c"(uid), "d"(gid) : "memory");
    return ret;
}

static inline int sys_chmod(const char* path, int mode) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(86), "b"(path), "c"(mode) : "memory");
    return ret;
}

static inline int sys_create_file(const char* path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(18), "b"(path) : "memory");
    return ret;
}

#endif