#ifndef OVOS_H
#define OVOS_H

// =============================================================================
// overOS userspace: единые обёртки системных вызовов (int 0x80).
// RAX = номер, RBX/RCX/RDX = аргументы, RAX = результат (64 бита — без обрезки адресов).
// Новые программы подключают только этот файл, а не копируют обёртки к себе.
// =============================================================================

#include <stdint.h>
#include <stddef.h>

static inline uint64_t ov_sc(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return r;
}

// --- Процессы ---
static inline void     ov_exit(int code)               { ov_sc(0, (uint64_t)code, 0, 0); for (;;) { } }
static inline void     ov_sleep(uint32_t ms)           { ov_sc(4, ms, 0, 0); }
static inline int      ov_getpid(void)                 { return (int)ov_sc(110, 0, 0, 0); }
static inline int      ov_getuid(void)                 { return (int)ov_sc(80, 0, 0, 0); }
static inline int      ov_kill(int pid, int sig)       { return (int)ov_sc(33, (uint64_t)pid, (uint64_t)sig, 0); }
// sig 0 — только проверить, жив ли процесс (как kill(pid, 0) в POSIX)
static inline int      ov_alive(int pid)               { return ov_kill(pid, 0) == 0; }
static inline int      ov_waitpid(int pid, int* st, int nohang) {
    return (int)ov_sc(32, (uint64_t)pid, (uint64_t)st, (uint64_t)nohang);
}
static inline int      ov_get_arg(char* out, int max)  { return (int)ov_sc(49, (uint64_t)out, (uint64_t)max, 0); }
// Запуск программы без fork. uidgid: (uid << 32) | gid, или (uint64_t)-1 — от своего имени
static inline int      ov_spawn(const char* path, const char* arg, uint64_t uidgid) {
    return (int)ov_sc(109, (uint64_t)path, (uint64_t)arg, uidgid);
}
static inline uint64_t ov_brk(uint64_t new_brk)        { return ov_sc(14, new_brk, 0, 0); }

// --- Разделяемые библиотеки .ll (linked library), аналог dlopen/dlsym/dlclose ---
// Ядро грузит .ll в адресное пространство процесса и чинит релокации; символ
// возвращается обычным адресом, по нему можно вызывать функцию или читать данные.
static inline int      ov_lib_open(const char* path)   { return (int)ov_sc(119, (uint64_t)path, 0, 0); }
static inline uint64_t ov_lib_sym(int lib, const char* name) {
    return ov_sc(120, (uint64_t)lib, (uint64_t)name, 0);
}
static inline int      ov_lib_close(int lib)           { return (int)ov_sc(121, (uint64_t)lib, 0, 0); }
// Как sbrk(): сдвигает границу кучи и возвращает старую (или (void*)-1 при отказе)
static inline void* ov_sbrk(int64_t inc) {
    uint64_t cur = ov_brk(0);
    if (inc == 0) return (void*)(uintptr_t)cur;
    if (ov_brk(cur + (uint64_t)inc) == (uint64_t)-1) return (void*)(uintptr_t)-1;
    return (void*)(uintptr_t)cur;
}

// --- Файлы ---
#define OV_O_RDONLY 0x0000
#define OV_O_WRONLY 0x0001
#define OV_O_RDWR   0x0002
#define OV_O_CREAT  0x0040
static inline int      ov_open(const char* p, int fl)  { return (int)ov_sc(10, (uint64_t)p, (uint64_t)fl, 0); }
static inline int64_t  ov_read(int fd, void* b, uint64_t n)        { return (int64_t)ov_sc(11, (uint64_t)fd, (uint64_t)b, n); }
static inline int64_t  ov_write(int fd, const void* b, uint64_t n) { return (int64_t)ov_sc(13, (uint64_t)fd, (uint64_t)b, n); }
static inline int      ov_close(int fd)                { return (int)ov_sc(12, (uint64_t)fd, 0, 0); }
// Позиция чтения/записи: whence 0 — от начала, 1 — от текущей, 2 — от конца
#define OV_SEEK_SET 0
#define OV_SEEK_CUR 1
#define OV_SEEK_END 2
static inline int64_t  ov_lseek(int fd, int64_t off, int whence) {
    return (int64_t)ov_sc(19, (uint64_t)fd, (uint64_t)off, (uint64_t)whence);
}
// Размер файла по дескриптору (через позицию конца)
static inline int64_t  ov_fsize(int fd) {
    int64_t cur = ov_lseek(fd, 0, OV_SEEK_CUR);
    int64_t end = ov_lseek(fd, 0, OV_SEEK_END);
    if (cur >= 0) ov_lseek(fd, cur, OV_SEEK_SET);
    return end;
}

// Содержимое каталога: n записей или -1 (нет доступа)
typedef struct {
    char     name[32];
    uint8_t  attr;           // 0x10 — каталог
    uint32_t size;
    uint32_t cluster;
    char     clean_name[64]; // длинное имя (LFN), если есть
} __attribute__((packed)) ov_dirent_t;
#define OV_ATTR_DIR 0x10
static inline int      ov_listdir(const char* p, ov_dirent_t* out, int max) { return (int)ov_sc(16, (uint64_t)p, (uint64_t)out, (uint64_t)max); }
// mkdir/create: > 0 — создано, иначе отказ (нет прав, уже есть, диск полон)
static inline int      ov_mkdir(const char* p)         { return (int)ov_sc(17, (uint64_t)p, 0, 0); }
static inline int      ov_create(const char* p)        { return (int)ov_sc(18, (uint64_t)p, 0, 0); }

// --- Консоль TTY (текстовый режим) ---
// Сисколл 1 печатает строку цветом, 13 пишет в дескриптор (fd 1 — /dev/tty).
static inline void ov_print_color(const char* s, uint32_t color) { ov_sc(1, (uint64_t)s, color, 0); }
static inline void ov_print(const char* s)                       { ov_print_color(s, 0x00FFFFFF); }

// --- Клавиатура и мышь (сырой ввод своего TTY; только процесс переднего плана) ---
static inline uint8_t ov_get_key(void)   { return (uint8_t)ov_sc(6, 0, 0, 0); }   // сканкод или 0
static inline int     ov_get_wheel(void) { return (int)(int64_t)ov_sc(24, 0, 0, 0); }
typedef struct {
    int     x, y;
    uint8_t buttons;     // бит 0 — ЛКМ, бит 1 — ПКМ
    int     last_dx, last_dy;
} __attribute__((packed)) ov_mouse_t;
static inline int ov_get_mouse(ov_mouse_t* m) { return (int)ov_sc(7, (uint64_t)m, 0, 0); }
// Относительное перемещение с прошлого вызова (игры: DOOM, Quake)
static inline void ov_get_mouse_rel(int* dx, int* dy, uint8_t* buttons) {
    ov_sc(23, (uint64_t)dx, (uint64_t)dy, (uint64_t)buttons);
}

// --- Графика: кадр 320x200 через ядро (игры) и /dev/fb0 (всё остальное) ---
static inline void     ov_set_palette(const uint32_t* pal)  { ov_sc(20, (uint64_t)pal, 0, 0); }
// Возвращает мс, которые программа простояла в фоне (её TTY не был на экране)
static inline uint32_t ov_blit_frame(const uint32_t* fb)    { return (uint32_t)ov_sc(21, (uint64_t)fb, 0, 0); }
static inline int      ov_ioctl(int fd, uint64_t req, void* arg) { return (int)ov_sc(15, (uint64_t)fd, req, (uint64_t)arg); }

// --- Процессы: fork/execve (sys_spawn обычно удобнее) ---
static inline int ov_fork(void)                                  { return (int)ov_sc(30, 0, 0, 0); }
static inline int ov_execve(const char* path, const char* args)  { return (int)ov_sc(31, (uint64_t)path, (uint64_t)args, 0); }

// --- TTY: строчный ввод, эхо, передний план ---
#define OV_TTY_ECHO 0x5401
#define OV_TTY_HIST 0x5402
static inline int  ov_tty_echo(int on)      { return ov_ioctl(0, OV_TTY_ECHO, (void*)(uintptr_t)on); }
// Копить ли введённые строки в истории терминала (стрелки вверх/вниз + history).
// Включает только оболочка: имя пользователя на входе в историю не нужно.
static inline int  ov_tty_hist(int on)      { return ov_ioctl(0, OV_TTY_HIST, (void*)(uintptr_t)on); }
// Кому из процессов отдан ввод этого TTY (0 — себе). Как tcsetpgrp(3).
static inline int  ov_tty_setfg(int pid)    { return (int)ov_sc(112, (uint64_t)pid, 0, 0); }
// Строка из /dev/tty (эхо и Backspace делает ядро). Возвращает длину без '\n'.
// < 0 — читать нельзя (мы не процесс переднего плана этого TTY)
static inline int ov_readline(char* buf, int max) {
    int64_t n = ov_read(0, buf, (uint64_t)max - 1);
    if (n < 0) { buf[0] = '\0'; return -1; }
    if (n == 0) { buf[0] = '\0'; return 0; }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    return (int)n;
}

// --- Каталог процесса ---
static inline int ov_chdir(const char* path)   { return (int)ov_sc(114, (uint64_t)path, 0, 0); }
static inline int ov_getcwd(char* buf, int max){ return (int)ov_sc(115, (uint64_t)buf, (uint64_t)max, 0); }

// --- Сеанс и система ---
// Стать другим пользователем по ЕГО паролю (root — без пароля), как su(1)
static inline int ov_su(const char* name, const char* pass) { return (int)ov_sc(116, (uint64_t)name, (uint64_t)pass, 0); }
static inline int ov_setuid(int uid)        { return (int)ov_sc(82, (uint64_t)uid, 0, 0); }
static inline int ov_setgid(int gid)        { return (int)ov_sc(83, (uint64_t)gid, 0, 0); }
static inline int ov_auth(const char* name, const char* pass) { return (int)ov_sc(84, (uint64_t)name, (uint64_t)pass, 0); }
static inline int ov_passwd(const char* name, const char* newp, const char* oldp) {
    return (int)ov_sc(89, (uint64_t)name, (uint64_t)newp, (uint64_t)oldp);
}
static inline int ov_useradd(const char* name, const char* pass, int admin) {
    return (int)ov_sc(90, (uint64_t)name, (uint64_t)pass, (uint64_t)admin);
}
static inline int ov_chmod(const char* path, int mode)        { return (int)ov_sc(86, (uint64_t)path, (uint64_t)mode, 0); }
static inline int ov_chown(const char* path, int uid, int gid) { return (int)ov_sc(87, (uint64_t)path, (uint64_t)uid, (uint64_t)gid); }
typedef struct { int uid, gid, mode; } ov_perm_t;
static inline int ov_stat_perm(const char* path, ov_perm_t* out) { return (int)ov_sc(88, (uint64_t)path, (uint64_t)out, 0); }
static inline int ov_load_module(const char* path, const char* args) { return (int)ov_sc(117, (uint64_t)path, (uint64_t)args, 0); }
// Сервисные команды, которые пока остались в ядре (taskmgr, сеть, звук, демо)
static inline int ov_kcmd(const char* line)  { return (int)ov_sc(118, (uint64_t)line, 0, 0); }
static inline void ov_reboot(void)           { ov_sc(50, 0, 0, 0); }
static inline void ov_shutdown(void)         { ov_sc(51, 0, 0, 0); }

// --- Первоначальная настройка и системные параметры ---
#define OV_LANG_RU 0
#define OV_LANG_EN 1
static inline int  ov_oobe_state(void)   { return (int)ov_sc(91, 0, 0, 0); }   // 1 — нужна настройка
static inline int  ov_oobe_finish(void)  { return (int)ov_sc(92, 0, 0, 0); }   // только root
static inline int  ov_lock_user(const char* name) { return (int)ov_sc(98, (uint64_t)name, 0, 0); }
// -1 в любом параметре — не менять. Только root.
static inline int  ov_set_system(int lang, int tz_index, int layout2) {
    return (int)ov_sc(96, (uint64_t)(int64_t)lang, (uint64_t)(int64_t)tz_index, (uint64_t)(int64_t)layout2);
}
// --- Псевдотерминал: настоящий TTY системы, картинку которого рисует сама
// программа. Оболочка там обычная (/sys/sh.prg), со всеми командами ядра.
static inline int ov_pty_open(int w_px, int h_px) { return (int)ov_sc(123, (uint64_t)w_px, (uint64_t)h_px, 0); }
static inline int ov_pty_read(int id, void* dst, uint64_t max_bytes) { return (int)ov_sc(124, (uint64_t)id, (uint64_t)dst, max_bytes); }
static inline int ov_pty_key(int id, unsigned char sc) { return (int)ov_sc(125, (uint64_t)id, (uint64_t)sc, 0); }
static inline int ov_pty_close(int id) { return (int)ov_sc(126, (uint64_t)id, 0, 0); }
static inline int ov_pty_alive(int id) { return (int)ov_sc(127, (uint64_t)id, 0, 0); }

// Скорость указателя: 25..400 процентов, ускорение 0/1. Читать может кто угодно,
// менять -- администратор (значение общее для системы, /etc/mouse.conf).
static inline int ov_mouse_get_speed(void) { return (int)(ov_sc(122, 0, 0, 0) & 0xFFFF); }
static inline int ov_mouse_get_accel(void) { return (int)((ov_sc(122, 0, 0, 0) >> 16) & 1); }
static inline int ov_mouse_set(int speed, int accel) { return (int)ov_sc(122, 1, (uint64_t)speed, (uint64_t)accel); }

typedef struct { int lang, tz_index, tz_offset, layout2; } ov_sys_settings_t;
static inline int  ov_get_system(ov_sys_settings_t* out) { return (int)ov_sc(95, (uint64_t)out, 0, 0); }
typedef struct { char name[32]; char city[64]; int offset; } ov_tz_t;
// Возвращает число поясов; out заполняется, если index в списке
static inline int  ov_tz_info(int index, ov_tz_t* out) { return (int)ov_sc(97, (uint64_t)(int64_t)index, (uint64_t)out, 0); }

// --- Таблица процессов (ps) ---
typedef struct {
    int      pid;
    int      uid;
    int      tty;
    int      state;        // 0 READY, 1 RUNNING, 2 SLEEPING, 3 ZOMBIE, 4 DEAD
    int      is_process;
    uint32_t mem_kb;
    uint64_t cpu_ms;       // процессорное время задачи с запуска (тик таймера = 1 мс)
    char     name[16];
} ov_proc_t;
static inline int ov_ps(ov_proc_t* out, int max) { return (int)ov_sc(113, (uint64_t)out, (uint64_t)max, 0); }

// --- О системе ---
typedef struct {
    char name[32];
    char arch[16];
    char version[32];
} __attribute__((packed)) ov_kernel_info_t;
static inline int ov_kernel_info(ov_kernel_info_t* i) { return (int)ov_sc(9, (uint64_t)i, 0, 0); }

// История команд своего терминала. Её ведёт драйвер TTY (строку собирает ядро),
// поэтому стрелки вверх/вниз работают и в консоли, и в окне терминала.
// idx < 0 -- вернуть количество записей; иначе заполнить buf и вернуть номер команды.
static inline int ov_tty_history(int idx, char* buf, uint64_t max) {
    return (int)ov_sc(129, (uint64_t)(int64_t)idx, (uint64_t)buf, max);
}

// Процессор: модель, сколько ядер и потоков, частоты и чем она управляется.
// cur_mhz -- измеренная по счётчикам APERF/MPERF, а не паспортная из CPUID.
#define OV_CPU_GOV_NONE   0    // частотой управляет только прошивка
#define OV_CPU_GOV_PSTATE 1    // мы запросили максимальный P-state
#define OV_CPU_GOV_HWP    2    // аппаратный регулятор Intel, выставлен на максимум
typedef struct {
    char    brand[52];
    int32_t cores, threads;
    int32_t base_mhz, max_mhz, cur_mhz;
    int32_t turbo_on, governor;
} ov_cpuinfo_t;
static inline int ov_cpuinfo(ov_cpuinfo_t* out) { return (int)ov_sc(130, (uint64_t)out, 0, 0); }

// Датчики процессора: температура пакета и текущего ядра (°C), TjMax — порог,
// где процессор сам сбрасывает частоту, и измеренная частота. valid = 0 —
// датчика нет (виртуальная машина, неизвестный процессор)
typedef struct {
    int32_t valid, pkg_c, core_c, tjmax_c, cur_mhz, base_mhz;
} ov_cpu_sensors_t;
static inline int ov_cpu_sensors(ov_cpu_sensors_t* out) { return (int)ov_sc(143, (uint64_t)out, 0, 0); }

// Физическая память системы: всего и занято, в байтах
typedef struct { uint64_t total, used; } ov_meminfo_t;
static inline int ov_meminfo(ov_meminfo_t* out) { return (int)ov_sc(128, (uint64_t)out, 0, 0); }

// Раздел системы и обмен с накопителем с момента загрузки
typedef struct { uint64_t total_mb, used_mb, read_kb, write_kb; } ov_diskinfo_t;
static inline int ov_diskinfo(ov_diskinfo_t* out) { return (int)ov_sc(131, (uint64_t)out, 0, 0); }

// Флешка с системой: 0 -- на месте, 1 -- выдернута, 2 -- вставлена чужая.
// Дешёвый вызов (на шину не ходит), можно звать раз в секунду.
#define OV_STORAGE_OK       0
#define OV_STORAGE_LOST     1
#define OV_STORAGE_FOREIGN  2
static inline int ov_storage_state(void) { return (int)ov_sc(132, 0, 0, 0); }

// Подключённые тома (sys_mounts): корень и съёмные накопители в /media.
// Раскладка совпадает с mount_info_t ядра (src/fs/mount.h).
#define OV_MOUNT_ROOT      1
#define OV_MOUNT_REMOVABLE 2
typedef struct {
    char     name[24];      // имя каталога в /media ("/" у корня)
    char     label[24];     // метка тома, "" -- нет
    char     dev[40];       // "ADATA USB Flash Drive"
    char     path[40];      // "/media/NAME"
    uint32_t size_mb;
    uint32_t flags;         // OV_MOUNT_*
} ov_mount_t;
static inline int ov_mounts(ov_mount_t* out, int max) { return (int)ov_sc(134, (uint64_t)out, (uint64_t)max, 0); }
// Безопасно извлечь накопитель по имени в /media: 0 -- можно вынимать
static inline int ov_eject(const char* name) { return (int)(int64_t)ov_sc(135, (uint64_t)name, 0, 0); }

// Уведомления от имени системы (только для рабочего стола, root): ядро кладёт
// их в очередь, композитор забирает. Раскладка -- knotify_t ядра (notify.h).
#define OV_URGENCY_LOW      0
#define OV_URGENCY_NORMAL   1
#define OV_URGENCY_CRITICAL 2
typedef struct {
    uint32_t id;
    uint16_t urgency;
    uint16_t timeout_s;
    char     icon[24];
    char     title[64];
    char     body[200];
    char     act_label[2][20];
    char     act_cmd[2][64];       // "open:/media/X", "eject:X"
    uint32_t close;                // 1 -- убрать карточку id
} ov_knotify_t;
static inline int ov_notify_poll(ov_knotify_t* out) { return (int)(int64_t)ov_sc(133, (uint64_t)out, 0, 0); }

// Путь к исполняемому файлу процесса (как /proc/<pid>/exe): 0 -- есть
static inline int ov_proc_exe(int pid, char* buf, int len) {
    return (int)(int64_t)ov_sc(136, (uint64_t)pid, (uint64_t)buf, (uint64_t)len);
}

// --- Рабочие ядра (smp.c) ---
// Остальные ядра процессора — вычислители для программ. ov_worker_start
// запускает функцию на свободном ядре в адресном пространстве программы; она
// видит ту же память, но из системных вызовов ей доступны только
// ov_wait_u32, ov_worker_id и выход. Обычный ввод-вывод, окна, файлы, brk —
// только с основного потока. Свободное ядро спит, пока его не разбудят.
#ifndef OV_SMP_TYPES
#define OV_SMP_TYPES
#define OV_SMP_MAX 16
typedef struct {
    int32_t  state;          // 0 нет, 1 свободно, 2 назначено, 3 работает, 4 сломано
    int32_t  owner_pid;
    uint32_t apic_id;
    uint32_t reserved;
    uint64_t busy_tsc;       // тактов вне hlt с подъёма ядра
    uint64_t total_tsc;
} ov_smp_cpu_t;
typedef struct {
    int32_t  online;         // рабочих ядер (без основного)
    int32_t  free;
    uint32_t bsp_apic_id;
    uint32_t reserved;
    uint64_t tsc_per_ms;
    ov_smp_cpu_t cpu[OV_SMP_MAX];
} ov_smp_info_t;
#endif
static inline int ov_smp_info(ov_smp_info_t* out) { return (int)(int64_t)ov_sc(137, (uint64_t)out, 0, 0); }
static inline int ov_smp_workers(void)          { return ov_smp_info(0); }

// Функция рабочего: void fn(void* arg). Если она вернётся, рабочий завершится
// сам: на вершину стека кладётся адрес выхода. stack_top выровнен на 16.
static inline void ov__worker_ret(void) { ov_sc(141, 0, 0, 0); for (;;) { } }
static inline int ov_worker_start(void (*fn)(void*), void* arg, void* stack_top) {
    uint64_t top = (uint64_t)stack_top & ~15ULL;
    *(uint64_t*)(top - 8) = (uint64_t)ov__worker_ret;
    return (int)(int64_t)ov_sc(138, (uint64_t)fn, (uint64_t)arg, top);
}
// Ждать, пока *p == v. На рабочем ядре — сон до ov_wake; на основном — уступая
// процессор, не дольше timeout_ms (0 — без предела). 1 — вышли по таймауту.
static inline int ov_wait_u32(volatile uint32_t* p, uint32_t v, uint32_t timeout_ms) {
    return (int)(int64_t)ov_sc(139, (uint64_t)p, v, timeout_ms);
}
// Разбудить рабочих этой программы, ждущих в ov_wait_u32 (после записи в слово)
static inline void ov_wake(volatile uint32_t* p) { ov_sc(140, (uint64_t)p, 0, 0); }
static inline void ov_worker_exit(void)          { ov__worker_ret(); }
static inline int  ov_worker_stop(int cpu)        { return (int)(int64_t)ov_sc(141, (uint64_t)cpu, 0, 0); }
static inline int  ov_worker_id(void)            { return (int)(int64_t)ov_sc(142, 0, 0, 0); }

// --- Пользователи ---
typedef struct {
    char name[32];
    int  uid;
    int  gid;
    char home[64];
} ov_user_t;
static inline int      ov_getgid(void)                 { return (int)ov_sc(81, 0, 0, 0); }
static inline int      ov_getpwuid(int uid, ov_user_t* u) { return (int)ov_sc(85, (uint64_t)uid, (uint64_t)u, 0); }
static inline int      ov_is_admin(int uid)            { return (int)ov_sc(94, (uint64_t)uid, 0, 0); }
// Права root процессу по паролю ТЕКУЩЕГО пользователя (он должен быть в wheel). 0 — успех.
static inline int      ov_sudo(const char* pass)       { return (int)ov_sc(93, (uint64_t)pass, 0, 0); }

// --- Время ---
typedef struct { uint8_t sec, min, hour, day, month, year; } __attribute__((packed)) ov_rtc_t;
static inline int      ov_rtc(ov_rtc_t* t)             { return (int)ov_sc(8, (uint64_t)t, 0, 0); }
static inline uint64_t ov_tsc_per_ms(void)             { return ov_sc(22, 0, 0, 0); }
static inline uint64_t ov_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// Монотонные миллисекунды (по TSC)
static inline uint64_t ov_ms(void) {
    uint64_t k = ov_tsc_per_ms();
    return k ? ov_rdtsc() / k : 0;
}

// --- IPC: сообщения ---
#define OV_MSG_MAX 256
typedef struct {
    int32_t  sender_pid;   // подписывает ядро — подделать нельзя
    int32_t  sender_uid;
    uint32_t len;
    uint32_t reserved;
    uint8_t  data[OV_MSG_MAX];
} ov_msg_t;

#define OV_WAIT_FOREVER 0xFFFFFFFFu
static inline int ov_send(int pid, const void* data, uint32_t len) { return (int)ov_sc(100, (uint64_t)pid, (uint64_t)data, len); }
// 1 — сообщение получено, 0 — таймаут (0 — не ждать, OV_WAIT_FOREVER — ждать сколько угодно)
static inline int ov_recv(ov_msg_t* m, uint32_t timeout_ms)       { return (int)ov_sc(101, (uint64_t)m, timeout_ms, 0); }
static inline int ov_register(const char* name)                   { return (int)ov_sc(102, (uint64_t)name, 0, 0); }
static inline int ov_lookup(const char* name)                     { return (int)ov_sc(103, (uint64_t)name, 0, 0); }

// --- Разделяемая память ---
static inline int      ov_shm_create(uint64_t size)    { return (int)ov_sc(104, size, 0, 0); }
static inline void*    ov_shm_map(int id)              { return (void*)(uintptr_t)ov_sc(105, (uint64_t)id, 0, 0); }
static inline int      ov_shm_unmap(int id)            { return (int)ov_sc(106, (uint64_t)id, 0, 0); }
static inline int      ov_shm_grant(int id, int pid)   { return (int)ov_sc(107, (uint64_t)id, (uint64_t)pid, 0); }
static inline int      ov_shm_destroy(int id)          { return (int)ov_sc(108, (uint64_t)id, 0, 0); }

// --- Аудио (подкоманды сисколла 70) ---
static inline int64_t  ov_audio(uint64_t cmd, uint64_t a, uint64_t b) { return (int64_t)ov_sc(70, cmd, a, b); }

// --- Мелочи без libc ---
static inline void* ov_memset(void* d, int c, size_t n) {
    void* r = d;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
static inline void* ov_memcpy(void* d, const void* s, size_t n) {
    void* r = d;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
static inline int ov_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static inline void ov_strlcpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static inline int ov_streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

// Привязка указателя на функцию к символу: OV_BIND(h, m.sin, "sin").
// Значение кладётся побайтно: запись через (uint64_t*) по адресу указателя на
// функцию нарушает правила перекрытия объектов, и компилятор об этом ругается.
#define OV_BIND(lib, fnptr, name)  do {                                   \
        uint64_t ov_bind_addr_ = ov_lib_sym((lib), (name));               \
        void* ov_bind_ptr_ = (void*)(uintptr_t)ov_bind_addr_;             \
        ov_memcpy((void*)&(fnptr), &ov_bind_ptr_, sizeof(void*));          \
    } while (0)

#endif
