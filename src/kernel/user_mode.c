#include "user_mode.h"
#include "../fs/mount.h"
#include "notify.h"
#include "../drivers/blk.h"
#include "../fs/fs.h"
#include "sched.h"
#include "smp.h"
_Static_assert(SMP_MAX * SMP_AP_STACK == 262144, "syscall_isr_stub: диапазон стеков рабочих ядер");
#include "../drivers/mouse.h"
#include "cpu.h"
#include "../drivers/ethernet.h"
#include "../drivers/hda.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "prog_loader.h"
#include "tty.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "config.h"
#include "users.h"
#include "ipc.h"
#include "sys_loader.h"
#include "lib_loader.h"
extern const char* fs_caller_cwd(void);
   // каталог вызывающего: процесс или оболочка TTY
extern int shell_kernel_builtin(const char* cmd);   // сервисные команды ядра (main.c)


// В ядре нет libc: при -ffreestanding GCC всё равно превращает циклы копирования/зануления
// и копии крупных структур в вызовы memcpy/memset, которых при линковке нет
// ("undefined reference to memcpy"). Поэтому все массовые операции — через rep movsb/stosb.
static inline void wm_copy(void* dst, const void* src, uint64_t bytes) {
    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(bytes) : : "memory");
}
static inline void wm_zero(void* dst, uint64_t bytes) {
    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(bytes) : "a"(0) : "memory");
}


extern void     kputs(const char* str, uint32_t color);
extern int      strcmp(const char* s1, const char* s2);
extern void     put_pixel(int x, int y, uint32_t color);
extern void     clear_screen(uint32_t color);
extern void     flush_buffer(void);
extern void     flush_dirty(void);
extern void     update_mouse_state(void);
extern void     sleep_ms(uint32_t ms);
extern void     get_mouse_delta(int* out_dx, int* out_dy, uint8_t* out_buttons);
extern uint8_t  inb(uint16_t port);
extern int      get_mouse_x(void);
extern int      get_mouse_y(void);
extern int      get_mouse_btn(void);
extern void     sys_reboot(void);
extern void     acpi_power_off(void);
extern void     itoa(int n, char* str);
extern uint16_t screen_width;
extern uint16_t screen_height;
extern void*    lfb;
extern uint16_t screen_pitch;   // в kernel.c это uint16_t
extern volatile uint32_t g_lfb_gen;   // +1 при каждой записи в LFB не через /dev/fb0 (см. devfs.c)
extern volatile uint32_t g_fb_last_write_tick;   // "программа сейчас выводит кадры" (см. keyboard.c, kernel.c)
extern volatile int      g_fb_write_tty;
extern uint32_t back_buffer[];
extern uint64_t g_tsc_per_ms;
extern uint32_t timer_ticks;

int kernel_create_fs_entry(const char* path, int is_dir) {
    if (!path || !path[0]) return -1;

    // Путь с каталогами разбирает сама ФС (fs_create_entry): ядру больше не нужно
    // ходить по компонентам и возвращать драйвер обратно в каталог оболочки.
    fs_lock();
    int res = is_dir ? fs_make_folder(path) : fs_touch(path);
    fs_unlock();
    return res;
}

static uint32_t now_ms(void) {
    if (!g_tsc_per_ms) return timer_ticks;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint32_t)((((uint64_t)hi << 32) | lo) / g_tsc_per_ms);
}

volatile int g_user_mode_active = 0;

uint64_t saved_kernel_rsp = 0;
volatile uint64_t kernel_exit_code = 0; // Безопасное хранилище кода выхода
__attribute__((used)) uint8_t  ring3_app_stack[512 * 1024] __attribute__((aligned(16)));

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_entry64_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) dtr64_t;

static gdt_entry64_t gdt[6];
static dtr64_t       gdt_ptr;
static tss_entry_t   tss;

static uint32_t g_doom_palette[256];

static int g_mouse_last_init = 0;
static int g_mouse_last_x = 0, g_mouse_last_y = 0;


static void str_to_fat83(const char* in, char* out83) {
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    out83[11] = '\0';
    int i = 0;
    while (*in && *in != '.' && i < 8) {
        char c = *in++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[i++] = c;
    }
    while (*in && *in != '.') in++;
    if (*in == '.') in++;
    int j = 8;
    while (*in && j < 11) {
        char c = *in++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[j++] = c;
    }
}

static void fork_child_resume(void);   // определена ниже, нужна форвард-декларация для case 30

// Растяжка кадра 320x200 -> экран (sys_blit_frame). Таблица X строится один раз на разрешение.
static uint16_t blit_xmap[1920];
static uint32_t blit_xmap_w = 0;

static void blit_320x200_scaled(const uint32_t* user_fb, uint32_t* dst_buf) {
    const uint32_t W = screen_width, H = screen_height;

    if (blit_xmap_w != W) {
        uint32_t step = (320u << 16) / W, fp = 0;
        for (uint32_t x = 0; x < W; x++, fp += step) {
            uint32_t sx = fp >> 16;
            blit_xmap[x] = (uint16_t)(sx > 319 ? 319 : sx);
        }
        blit_xmap_w = W;
    }

    uint32_t step_y = (200u << 16) / H, y_fp = 0, prev_sy = 0xFFFFFFFFu;
    uint32_t* prev_row = 0;
    for (uint32_t y = 0; y < H; y++, y_fp += step_y) {
        uint32_t sy = y_fp >> 16;
        if (sy > 199) sy = 199;
        uint32_t* dst_row = dst_buf + (size_t)y * W;
        if (sy == prev_sy) {   // та же исходная строка: копируем готовую
            uint32_t* d = dst_row; const uint32_t* sp = prev_row; uint64_t n = W >> 1;
            __asm__ volatile ("rep movsq" : "+D"(d), "+S"(sp), "+c"(n) :: "memory");
            if (W & 1) *d = *sp;
        } else {
            const uint32_t* src_row = user_fb + sy * 320;
            for (uint32_t x = 0; x < W; x++) dst_row[x] = src_row[blit_xmap[x]];
        }
        prev_sy = sy;
        prev_row = dst_row;
    }
}

// 1 = растягивать кадр прямо в LFB построчно (через маленький буфер в L1-кэше), минуя буфер TTY:
//     нет записи 6 МБ в RAM и повторного чтения их же при flush_buffer().
// 0 = старый путь: растяжка в буфер TTY + flush_buffer() (если что-то читает буфер TTY как "экран").
#define BLIT_DIRECT_TO_LFB 1

static uint32_t blit_row[1920] __attribute__((aligned(64)));   // одна растянутая строка

static void blit_320x200_to_lfb(const uint32_t* user_fb) {
    const uint32_t W = screen_width, H = screen_height;
    uint8_t* lfb8 = (uint8_t*)lfb;
    if (W > 1920) return;   // размер blit_row / blit_xmap

    if (blit_xmap_w != W) {
        uint32_t step = (320u << 16) / W, fp = 0;
        for (uint32_t x = 0; x < W; x++, fp += step) {
            uint32_t sx = fp >> 16;
            blit_xmap[x] = (uint16_t)(sx > 319 ? 319 : sx);
        }
        blit_xmap_w = W;
    }

    uint32_t step_y = (200u << 16) / H, y_fp = 0, prev_sy = 0xFFFFFFFFu;
    for (uint32_t y = 0; y < H; y++, y_fp += step_y) {
        uint32_t sy = y_fp >> 16;
        if (sy > 199) sy = 199;
        if (sy != prev_sy) {   // новая исходная строка: растягиваем по X в L1-кэше
            const uint32_t* src_row = user_fb + sy * 320;
            for (uint32_t x = 0; x < W; x++) blit_row[x] = src_row[blit_xmap[x]];
            prev_sy = sy;
        }
        // одна и та же готовая строка уходит в LFB столько раз, сколько нужно по вертикали
        void* d = lfb8 + (size_t)y * screen_pitch;
        const void* sp = blit_row;
        uint64_t n = W >> 1;
        __asm__ volatile ("rep movsq" : "+D"(d), "+S"(sp), "+c"(n) :: "memory");
        if (W & 1) *(uint32_t*)d = *(const uint32_t*)sp;
    }
    __asm__ volatile ("sfence" ::: "memory");
    g_lfb_gen++;   // копия экрана в devfs.c (/dev/fb0) теперь устарела
}

// =============================================================================
// Проверка указателей из Ring 3. Без неё любая программа могла передать в сисколл
// адрес ядра (верхняя половина) и прочитать/перезаписать его память — например, свой uid.
// =============================================================================
#define USER_SPACE_END 0x0000800000000000ULL
// NULL каждый сисколл обрабатывает сам; иначе — весь буфер отображён и доступен Ring 3
static int uptr_ok(uint64_t p, uint64_t len) {
    if (p == 0) return 1;
    return vmm_user_range_ok(p, len);
}

// Строку проверяем постранично до нулевого байта (не дальше, чем ядро из неё прочитает)
#define USTR_MAX 256
static int ustr_ok(uint64_t p) {
    if (p == 0) return 1;
    return vmm_user_str_ok(p, USTR_MAX);
}

static int user_args_ok(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (num) {
        case 1: case 10: case 17: case 18: case 86: case 87: case 93: case 98: case 102: case 103:
            return ustr_ok(a1);
        case SYS_SMP_INFO: return a1 == 0 || uptr_ok(a1, sizeof(ov_smp_info_t));
        case SYS_WAIT_U32: return (a1 & 3) == 0 && uptr_ok(a1, 4);
        case 100: return a3 <= IPC_MSG_MAX && uptr_ok(a2, a3);
        case 101: return uptr_ok(a1, sizeof(ipc_msg_t));
        case 109: return ustr_ok(a1) && ustr_ok(a2);
        case 7:  return uptr_ok(a1, 17);                 // user_mouse_t (packed)
        case 8:  return uptr_ok(a1, 6);                  // rtc_time_t
        case 9:  return uptr_ok(a1, 80);                 // kernel_info_t
        case 60: return uptr_ok(a1, 14);                 // sys_net_info_t
        case 20: return uptr_ok(a1, 256 * 4);            // палитра
        case 11: case 13:
            return uptr_ok(a2, a3);
        case 15:
            // FBIOGET_VSCREENINFO пишет fb_var_info_t; у остальных запросов arg — число (яркость)
            return a2 == 0x4600 ? uptr_ok(a3, sizeof(fb_var_info_t)) : 1;
        case 16:
            return ustr_ok(a1) && a3 < 4096 && uptr_ok(a2, a3 * sizeof(fs_file_info_t));
        case 21:
            return uptr_ok(a1, 320 * 200 * 4);
        case 23:
            return uptr_ok(a1, 4) && uptr_ok(a2, 4) && uptr_ok(a3, 1);
        case 31: case 84: case 90:
            return ustr_ok(a1) && ustr_ok(a2);
        case 32:
            return uptr_ok(a2, 4);
        case 49:
            return uptr_ok(a1, a2);
        case 70:
            if (a1 == 1) return uptr_ok(a2, sizeof(hda_outputs_t));
            if (a1 == 8) return uptr_ok(a2, a3);
            return 1;
        case 85: return uptr_ok(a2, sizeof(user_info_t));
        case 88: return ustr_ok(a1) && uptr_ok(a2, sizeof(perm_info_t));
        case 89: return ustr_ok(a1) && ustr_ok(a2) && ustr_ok(a3);
        case 95: return uptr_ok(a1, 16);
        case 113: return a2 < 256 && uptr_ok(a1, a2 * sizeof(sched_proc_info_t));
        case 114: case 118: case 119: return ustr_ok(a1);
        case 120: return ustr_ok(a2);
        case 115: return uptr_ok(a1, a2);
        case 124: return uptr_ok(a2, a3);
        case 128: return uptr_ok(a1, 16);
        case 131: return uptr_ok(a1, 32);
        case 133: return uptr_ok(a1, sizeof(knotify_t));
        case 134: return a2 <= 16 && uptr_ok(a1, a2 * sizeof(mount_info_t));
        case 135: return ustr_ok(a1);
        case 136: return a3 > 0 && a3 <= 256 && uptr_ok(a2, a3);
        case 130: return uptr_ok(a1, 128);
        case 143: return uptr_ok(a1, 24);
        case 129: return a2 == 0 || uptr_ok(a2, a3);
        case 116: case 117: return ustr_ok(a1) && ustr_ok(a2);
        case 97: return uptr_ok(a2, 100);
        default:
            return 1;
    }
}

// Кэш размера раздела для sys_diskinfo (см. ниже): пересчитывается не чаще
// раза в десять секунд, потому что считается проходом по таблице FAT.
extern uint32_t timer_ticks;
static uint32_t disk_stat_used = 0, disk_stat_total = 0, disk_stat_tick = 0;

// Копия строки пользователя в ядро (после user_args_ok)
static void ucopy_str(char* dst, const char* src, int max) {
    int i = 0;
    if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// frame указывает на 20 слотов (160 байт), которые syscall_isr_stub сохранил на стеке:
// frame[0..13]  = r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rdx,rcx,rbx,rax
// frame[14]     = rbp вызывающего
// frame[15..19] = RIP,CS,RFLAGS,RSP,SS — аппаратно сохранённые CPU при входе через int 0x80
// Нужен только sys_fork: остальные сисколлы его не трогают.
uint64_t syscall_dispatcher(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t* frame) {
    task_t* caller = sched_get_current_task();
    // Псевдотерминал на экран не выводится вообще: его картинку забирает окно,
    // поэтому вывод из него нельзя глушить как "фоновый".
    tty_t* caller_tty = (caller && caller->tty_id >= 0) ? tty_get(caller->tty_id) : NULL;
    int visible = (!caller || caller->tty_id < 0 || caller->tty_id == tty_get_active_id() ||
                   (caller_tty && caller_tty->is_pty));
    // Фоновая программа (её TTY не на экране) не рисует в чужой экран
    if (!visible && (num == 1 || num == 2 || num == 3 || num == 5)) return 0;

    // Вызов из Ring 3 (CS.RPL == 3): указатели обязаны лежать в пользовательской половине
    if (frame && (frame[16] & 3) == 3 && !user_args_ok(num, arg1, arg2, arg3)) return (uint64_t)-1;

    int c_uid = caller ? caller->uid : 0;
    int c_gid = caller ? caller->gid : 0;

    // Сырые клавиатура/мышь — только процессу на переднем плане TTY (остальные получают ввод
    // событиями от desktop): иначе фоновый процесс прошлого сеанса читал бы чужие пароли
    if ((num == 6 || num == 7 || num == 23 || num == 24) && !tty_is_foreground(caller)) return 0;

    // Рабочие ядра: запуск потоков-вычислителей, ожидание и пробуждение (smp.c)
    if (num >= SYS_SMP_INFO && num <= SYS_WORKER_ID)
        return smp_bsp_syscall(num, arg1, arg2, arg3, caller ? (int)caller->pid : 0, caller ? caller->cr3 : 0);

    switch (num) {
        case 0: // exit(code)
            // Печатаем в консоль только если НЕ включен графический режим
            tty_t* cur_tty = (caller && caller->tty_id >= 0) ? tty_get(caller->tty_id) : NULL;
            if (cur_tty && !cur_tty->gfx_mode) {
                kputs("\n>>> [KERNEL 64] Syscall 0 (exit) received! Code: ", 0x55FF55);
                char ebuf[16];
                itoa((int)arg1, ebuf);
                kputs(ebuf, 0xFFFFFF);
                kputs("\n", 0x55FF55);
                flush_buffer();
            }
            if (caller && caller->is_process) sched_exit_current((int)arg1);
            kernel_exit_code = arg1;
            return arg1;

        case 1: { // print(str, color)
            kputs((const char*)arg1, (uint32_t)(arg2 ? arg2 : 0x00FFFFFF));
            flush_dirty();
            return 0;
        }

        case 2: // put_pixel(x, y, color)
            put_pixel((int)arg1, (int)arg2, (uint32_t)arg3);
            return 0;

        case 3: // flush()
            flush_buffer();
            return 0;

        case 4: { // sleep(ms): задача спит в планировщике и не занимает процессор
            sched_sleep_ms((uint32_t)arg1);
            return 0;
        }

        case 5: // clear(color)
            clear_screen((uint32_t)arg1);
            flush_buffer();
            return 0;

        case 6: { // get_key() -> uint8_t
            if (!visible) return 0;   // фоновая программа клавиатуру не трогает
            if (!caller || caller->tty_id < 0) return 0;
            uint8_t sc;
            if (tty_kbd_pop(caller->tty_id, &sc)) return (uint64_t)sc;
            return 0;
        }

        case 7: { // sys_get_mouse(user_mouse_t* out) -> int
            if (!visible) return 0;

            update_mouse_state();

            if (arg1) {
                typedef struct {
                    int x;
                    int y;
                    uint8_t buttons;
                    int last_dx;
                    int last_dy;
                } __attribute__((packed)) user_mouse_t;

                extern int get_mouse_last_dx(void);
                extern int get_mouse_last_dy(void);

                user_mouse_t* ums = (user_mouse_t*)arg1;
                ums->x = get_mouse_x();
                ums->y = get_mouse_y();
                ums->buttons = get_mouse_buttons_raw();
                ums->last_dx = get_mouse_last_dx();
                ums->last_dy = get_mouse_last_dy();
                return 1;
            }
            return 0;
        }
        case 8: { // sys_get_time(rtc_time_t* out) -> int
            if (!arg1) return 0;

            typedef struct {
                uint8_t sec, min, hour;
                uint8_t day, month, year;
            } __attribute__((packed)) user_rtc_time_t;

            extern uint8_t inb(uint16_t port);
            extern void outb(uint16_t port, uint8_t val);

            // Ждём готовности RTC (в Ring 0 inb/outb полностью легальны)
            outb(0x70, 0x0A);
            while (inb(0x71) & 0x80);

            #define BCD2BIN(v) (((v) & 0x0F) + (((v) >> 4) * 10))

            outb(0x70, 0x00); uint8_t s = BCD2BIN(inb(0x71));
            outb(0x70, 0x02); uint8_t m = BCD2BIN(inb(0x71));
            outb(0x70, 0x04); uint8_t h_raw = inb(0x71);
            outb(0x70, 0x07); uint8_t d = BCD2BIN(inb(0x71));
            outb(0x70, 0x08); uint8_t mo = BCD2BIN(inb(0x71));
            outb(0x70, 0x09); uint8_t y = BCD2BIN(inb(0x71));

            uint8_t is_pm = h_raw & 0x80;
            uint8_t h = BCD2BIN(h_raw & 0x7F);
            if (is_pm) h = (h + 12) % 24;

            user_rtc_time_t* ut = (user_rtc_time_t*)arg1;
            ut->sec = s;
            ut->min = m;
            ut->hour = h;
            ut->day = d;
            ut->month = mo;
            ut->year = y;
            return 1;
        }
        case 9: { // sys_get_kernel_info(kernel_info_t* out)
            if (!arg1) return (uint64_t)-1;

            typedef struct {
                char name[32];
                char arch[16];
                char version[32];
            } __attribute__((packed)) kinfo_t;

            kinfo_t* k = (kinfo_t*)arg1;

            // Копируем данные из src/kernel/config.h
            const char* s_name = OS_NAME;
            int i = 0;
            while (s_name[i] && i < 31) { k->name[i] = s_name[i]; i++; }
            k->name[i] = '\0';

            const char* s_arch = OS_ARCH;
            i = 0;
            while (s_arch[i] && i < 15) { k->arch[i] = s_arch[i]; i++; }
            k->arch[i] = '\0';

            const char* s_ver = OS_VERSION " devbuild";
            i = 0;
            while (s_ver[i] && i < 31) { k->version[i] = s_ver[i]; i++; }
            k->version[i] = '\0';

            return 0;
        }

        case 10: { // sys_open(const char* path, uint32_t flags)
            return (uint64_t)(int64_t)vfs_sys_open((const char*)arg1, (uint32_t)arg2);
        }

        case 11: { // sys_read(int fd, void* buf, size_t count)
            return (uint64_t)vfs_sys_read((int)arg1, (void*)arg2, arg3);
        }

        case 12: { // sys_close(int fd)
            return (uint64_t)(int64_t)vfs_sys_close((int)arg1);
        }

        case 13: { // sys_write(int fd, const void* buf, size_t count)
            return (uint64_t)vfs_sys_write((int)arg1, (const void*)arg2, arg3);
        }
        case 19: { // sys_lseek(int fd, int64_t off, int whence) -> новая позиция или -1
            return (uint64_t)vfs_sys_lseek((int)arg1, (int64_t)arg2, (int)arg3);
        }

        case 14: { // sys_brk(new_brk) -> uint64_t
            task_t* curr = sched_get_current_task();   // куча принадлежит задаче, а не «главному» процессу
            if (!curr) return (uint64_t)-1;

            uint64_t new_brk = arg1;

            // Если передали 0 — программа запрашивает текущую границу кучи
            if (new_brk == 0) {
                return curr->heap_end;
            }

            // Нельзя опускать кучу ниже начального адреса и поднимать в область стека
            if (new_brk < curr->heap_start || new_brk > SHM_BASE) {   // выше — область разделяемой памяти
                return (uint64_t)-1;
            }

            // Выделяем и мапим новые страницы для кучи процесса
            uint64_t cur_page = (curr->heap_end + 4095) & ~0xFFFULL;
            uint64_t req_page = (new_brk + 4095) & ~0xFFFULL;

            // Куча сжалась — отдаём страницы обратно в PMM (раньше память не возвращалась)
            while (cur_page > req_page) {
                cur_page -= 4096;
                uint64_t phys = vmm_unmap_page_take((uint64_t*)curr->cr3, cur_page);
                if (phys) {
                    pmm_free_page((void*)phys);
                    if (curr->mem_size >= 4096) curr->mem_size -= 4096;
                }
            }

            while (cur_page < req_page) {
                void* phys = pmm_alloc_page();
                if (!phys) {
                    kputs("[!] sys_brk: Out of physical memory!\n", 0x00FF5555);
                    return (uint64_t)-1;
                }
                if (!vmm_map_page((uint64_t*)curr->cr3, cur_page, (uint64_t)phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE)) {
                    pmm_free_page(phys);   // не замапилась (нет памяти или адрес в identity-области ядра) — не теряем страницу
                    return (uint64_t)-1;
                }
                curr->mem_size += 4096;
                cur_page += 4096;
            }

            curr->heap_end = new_brk;
            return curr->heap_end;
        }
        case 15: { // sys_ioctl(int fd, uint64_t req, uint64_t arg)
            return (uint64_t)(int64_t)vfs_sys_ioctl((int)arg1, arg2, arg3);
        }
        case 16: { // sys_get_dir_files(path, buf, max_files)
            const char* path = (const char*)arg1;
            void* buf = (void*)arg2;
            int max_files = (int)arg3;
            if (path && !perm_check(c_uid, c_gid, path, fs_caller_cwd(), PERM_R | PERM_X)) return (uint64_t)-1;
            fs_lock();
            int count = fs_get_dir_files(path, (fs_file_info_t*)buf, max_files);
            fs_unlock();
            return (uint64_t)count;
        }
        case 17: { // sys_mkdir(const char* path) -> int
            const char* upath = (const char*)arg1;
            if (!upath) return (uint64_t)-1;
            char kpath[128];
            int i = 0;
            while (upath[i] && i < 127) { kpath[i] = upath[i]; i++; }
            kpath[i] = '\0';
            if (!perm_check_parent(c_uid, c_gid, kpath, fs_caller_cwd(), PERM_W)) return (uint64_t)-1;
            int res = kernel_create_fs_entry(kpath, 1);
            if (res > 0) perm_on_create(c_uid, c_gid, kpath, fs_caller_cwd(), 1);
            return (uint64_t)res;
        }

        case 18: { // sys_create_file(const char* path) -> int
            const char* upath = (const char*)arg1;
            if (!upath) return (uint64_t)-1;
            char kpath[128];
            int i = 0;
            while (upath[i] && i < 127) { kpath[i] = upath[i]; i++; }
            kpath[i] = '\0';
            if (!perm_check_parent(c_uid, c_gid, kpath, fs_caller_cwd(), PERM_W)) return (uint64_t)-1;
            int res = kernel_create_fs_entry(kpath, 0);
            if (res > 0) perm_on_create(c_uid, c_gid, kpath, fs_caller_cwd(), 0);
            return (uint64_t)res;
        }

        case 20: // sys_set_palette
            if (arg1) {
                const uint32_t* pal = (const uint32_t*)arg1;
                for (int i = 0; i < 256; i++) g_doom_palette[i] = pal[i];
            }
            return 0;

        case 21: { // sys_blit_frame(fb) -> сколько мс задача простояла на паузе (пауза часов)
            if (!arg1 || arg1 >= 0x0000800000000000ULL) return 0;
            if (!caller || caller->tty_id < 0 || screen_width == 0 || screen_height == 0) return 0;
            tty_t* my_tty = tty_get(caller->tty_id);
            if (!my_tty || !my_tty->buffer) return 0;
            const uint32_t* user_fb = (const uint32_t*)arg1;

            uint32_t paused = 0;
            if (tty_get_active_id() != caller->tty_id) {
                // TTY программы не на экране: стоим и отдаём процессор другим задачам
                uint32_t t0 = now_ms();
                while (tty_get_active_id() != caller->tty_id) sched_yield();
                paused = now_ms() - t0;

                // Alt-up прочитала оболочка другого TTY: шлём релизы в НАШУ очередь TTY,
                // чтобы клавиши не залипли (синтетические байты, в обход ps2_hw_service —
                // прямиком в очередь, как раньше ps2_pump писал прямо в g_kbd_buf)
                static const uint8_t rel[3] = {0xB8, 0x9D, 0xAA};   // Alt, Ctrl, Shift up
                for (int i = 0; i < 3; i++) {
                    tty_kbd_push(caller->tty_id, rel[i]);
                }
            }

            g_fb_write_tty = caller->tty_id;          // этот TTY сейчас занят игрой:
            g_fb_last_write_tick = timer_ticks;       // Ctrl+C не убивает её, курсор консоли не рисуется

#if BLIT_DIRECT_TO_LFB
            (void)my_tty;
            blit_320x200_to_lfb(user_fb);   // TTY программы сейчас активен: рисуем сразу на экран
#else
            uint32_t* dst_buf = my_tty->buffer;   // буфер TTY, в котором запущена программа
            blit_320x200_scaled(user_fb, dst_buf);
            flush_buffer();   // TTY программы сейчас активен
#endif
            return paused;
        }
        case 22: return g_tsc_per_ms;
        // В диспетчере syscall ядра overOS:
        case 23: {
            // Вход: EBX = указатель на int dx, ECX = int dy, EDX = uint8_t buttons
            int* u_dx = (int*)arg1;       // вместо regs->rbx
            int* u_dy = (int*)arg2;       // вместо regs->rcx
            uint8_t* u_btn = (uint8_t*)arg3; // вместо regs->rdx
    
            // Фоновая программа мышь не трогает (и не съедает накопленное движение)
            int k_dx = 0, k_dy = 0;
            uint8_t k_btn = 0;
            if (visible) {
                extern void get_mouse_delta(int* dx, int* dy, uint8_t* buttons);   // mouse.c
                update_mouse_state();
                get_mouse_delta(&k_dx, &k_dy, &k_btn);   // движение с прошлого вызова + кнопки
            }

            if (u_dx) *u_dx = k_dx;
            if (u_dy) *u_dy = k_dy;
            if (u_btn) *u_btn = k_btn;
            break;
        }
        case 24: { // sys_get_wheel() -> щелчки колеса мыши с прошлого вызова (+ вниз, - вверх)
            if (!visible) return 0;
            update_mouse_state();
            return (uint64_t)(int64_t)get_mouse_wheel_delta();
        }

        case 30: { // sys_fork()
            task_t* parent = sched_get_current_task();
            if (!parent) return (uint64_t)-1;

            // 1. Выделяем память под структуру task_t ребенка
            task_t* child = (task_t*)pmm_alloc_page();
            if (!child) return (uint64_t)-1;

            // 2. Копируем базовые поля процесса и сбрасываем состояние задач
            sched_task_fpu_capture(child);   // состояние FPU/SSE наследуется, как и регистры
            child->pid = sched_alloc_pid();
            child->state = TASK_READY;
            child->parent = parent;
            child->exit_code = 0;
            child->is_process = 1;        // ребёнок — такой же процесс Ring 3, как и родитель
            child->tty_id = parent->tty_id;
            child->heap_start = parent->heap_start;
            child->heap_end   = parent->heap_end;
            child->user_entry = 0;
            child->user_stack_top = 0;
            child->uid = parent->uid;
            child->gid = parent->gid;
            child->wake_tick = 0;
            child->wait_flags = 0;
            child->ipc_pending = 0;
            for (int ai = 0; ai < (int)sizeof(child->arg); ai++) child->arg[ai] = parent->arg[ai];

            // Очищаем таблицу дескрипторов перед аллокациями памяти
            for (int fd = 0; fd < MAX_FD; fd++) {
                child->fd_table[fd] = NULL;
            }

            // Копируем имя родительского процесса
            int i = 0;
            while (parent->name[i] && i < 15) { 
                child->name[i] = parent->name[i]; 
                i++; 
            }
            child->name[i] = '\0';
            for (i = 0; i < (int)sizeof(child->exe); i++) child->exe[i] = parent->exe[i];

            // Наследуем текущую рабочую директорию (CWD)
            int c = 0;
            while (parent->cwd[c] && c < (int)sizeof(child->cwd) - 1) {
                child->cwd[c] = parent->cwd[c];
                c++;
            }
            child->cwd[c] = '\0';

            // 3. Клонируем виртуальное адресное пространство (память User Space)
            uint64_t* child_cr3 = vmm_clone_address_space((uint64_t*)parent->cr3);
            if (!child_cr3) {
                pmm_free_page(child);
                return (uint64_t)-1;
            }
            child->cr3 = (uint64_t)child_cr3;

            // 4. Выделяем собственный ядерный стек процесса (64 КБ)
            void* kst_phys = pmm_alloc_pages(PROC_KSTACK_SIZE / 4096);
            if (!kst_phys) {
                vmm_destroy_address_space(child_cr3);
                pmm_free_page(child);
                return (uint64_t)-1;
            }
            uint8_t* kstack = (uint8_t*)PHYS_TO_VIRT(kst_phys);
            child->kstack_bottom = (uint64_t)kstack;
            child->kstack_top    = (uint64_t)(kstack + PROC_KSTACK_SIZE);
            child->mem_size = 4096 + PROC_KSTACK_SIZE;

            // 4b. Открытые .ll достались ребёнку вместе с памятью — отмечаем это
            lib_fork(parent->pid, child->pid);

            // 5. UNIX-WAY: Клонирование таблицы файловых дескрипторов
            // Выполняется строго после всех аллокаций, чтобы при OOM не повреждать ref_count
            for (int fd = 0; fd < MAX_FD; fd++) {
                child->fd_table[fd] = parent->fd_table[fd];
                if (child->fd_table[fd]) {
                    child->fd_table[fd]->ref_count++;
                }
            }

            // 6. Копируем на вершину стека ребёнка сохранённый кадр прерывания (160 байт).
            //    Подменяем слот RAX на 0 (для ребёнка fork() возвращает 0)
            uint64_t* tf = (uint64_t*)(kstack + PROC_KSTACK_SIZE - 160);
            for (int k = 0; k < 20; k++) tf[k] = frame[k];
            tf[13] = 0;   // frame[13] = RAX дочернего процесса

            // 7. Трамплин для switch_to: 6 callee-saved регистров + fork_child_resume
            uint64_t* stk = tf;
            *(--stk) = (uint64_t)fork_child_resume;
            *(--stk) = 0; // rbp
            *(--stk) = 0; // rbx
            *(--stk) = 0; // r12
            *(--stk) = 0; // r13
            *(--stk) = 0; // r14
            *(--stk) = 0; // r15
            child->rsp = (uint64_t)stk;

            child->in_user = 1;
            child->saved_krsp = child->kstack_top;

            // 8. Добавляем ребёнка в планировщик
            sched_enqueue_task(child);

            /* kputs("[fork] new child PID=", 0x55FFFF);
            char fbuf[16]; itoa((int)child->pid, fbuf);
            kputs(fbuf, 0xFFFFFF); kputs(", parent PID=", 0x55FFFF);
            itoa((int)parent->pid, fbuf); kputs(fbuf, 0xFFFFFF); kputs("\n", 0x55FFFF);
            sched_dump_tasks(); */

            // Родителю возвращаем PID ребёнка
            return child->pid;
        }

        case 32: { // sys_waitpid(int pid, int* status, int options)
            int target_pid = (int)arg1;
            int* out_status = (int*)arg2;
            (void)arg3; // WNOHANG

            int code = 0;
            int reaped = sched_reap_zombie(caller, target_pid, &code);
            if (reaped > 0) {
                if (out_status) *out_status = code;
                return (uint64_t)reaped;
            }
            return 0;
        }   

        case 33: { // sys_kill(int pid, int sig)
            uint64_t target_pid = (uint64_t)arg1;
            int sig = (int)arg2;
            // Потоки ядра (оболочки TTY) не убиваются; процессы — только свои (root — любые).
            // Раньше было "pid <= 2" в расчёте, что desktop — это PID 2, а это не гарантировано.
            // Сигнал 0 — только проверка, жив ли процесс (POSIX kill(pid, 0))
            if (sig == 0) return sched_pid_is_process(target_pid) ? 0 : (uint64_t)-1;
            int t_uid = sched_pid_uid(target_pid);
            if (t_uid < 0 || !sched_pid_is_process(target_pid)) return (uint64_t)-1;
            if (c_uid != UID_ROOT && t_uid != c_uid) return (uint64_t)-1;

            return (uint64_t)sched_send_signal(target_pid, sig);
        }

case 31: { // sys_execve(const char* filename, const char* args)
            const char* ufn = (const char*)arg1;
            if (!ufn) return (uint64_t)-1;

            char kname[64];
            int ki = 0;
            while (ufn[ki] && ki < 63) { kname[ki] = ufn[ki]; ki++; }
            kname[ki] = '\0';
            const char* filename = kname;
            if (!filename || filename[0] == '\0') return (uint64_t)-1;

            // 1. ЗАЩИТА ОТ ДУРАКА: запрещаем запуск desktop.prg внутри существующей сессии
            const char* b_check = filename;
            for (int i = 0; filename[i]; i++) {
                if (filename[i] == '/') b_check = &filename[i + 1];
            }
            const char* dt = "desktop.prg";
            int is_dt = 1;
            for (int i = 0; dt[i] || b_check[i]; i++) {
                char c1 = b_check[i], c2 = dt[i];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) { is_dt = 0; break; }
            }
            if (is_dt) {
                return (uint64_t)-1;
            }

            // 2. СОХРАНЯЕМ АРГУМЕНТ (arg2): путь к файлу для notepad, overimages и т.д.
            // Аргумент хранится в самой задаче (раньше — одна глобальная переменная на всех:
            // два быстро открытых файла могли достаться не тем программам)
            char new_arg[128];
            ucopy_str(new_arg, (const char*)arg2, sizeof(new_arg));

            // 3. Право на исполнение
            if (!perm_check(c_uid, c_gid, filename, fs_caller_cwd(), PERM_X)) return (uint64_t)-1;

            // 4. Читаем бинарник с диска. fs_lock держим, пока образ не скопирован:
            // kernel_temp_buf общий с prog_load_module
            extern uint8_t kernel_temp_buf[];
            fs_lock();
            int bytes = fs_read_file(filename, kernel_temp_buf, 1024 * 1024 * 4);
            if (bytes <= 0) { fs_unlock(); return (uint64_t)-1; }

            task_t* current = sched_get_current_task();
            if (!current) { fs_unlock(); return (uint64_t)-1; }

            // 5. Разбор и проверка заголовка (недоверенные данные из файла)
            prg_image_t img;
            if (prg_parse_image(kernel_temp_buf, bytes, &img) != 0) { fs_unlock(); return (uint64_t)-1; }

            // 6. Новое адресное пространство создаём раньше уничтожения старого
            uint64_t* new_pml4 = vmm_create_address_space();
            if (!new_pml4) { fs_unlock(); return (uint64_t)-1; }
            if (prg_map_image(new_pml4, &img) != 0) {
                vmm_destroy_address_space(new_pml4);
                fs_unlock();
                return (uint64_t)-1;
            }

            // 7. Копируем тело и зануляем .bss
            vmm_switch_directory(new_pml4);

            uint8_t* target = (uint8_t*)img.load_base;
            uint8_t* src = kernel_temp_buf + img.payload_offset;
            for (uint64_t i = 0; i < img.payload_bytes; i++) target[i] = src[i];
            for (uint64_t i = img.payload_bytes; i < (uint64_t)img.image_pages * 4096; i++) target[i] = 0;
            fs_unlock();

            for (int ai = 0; ai < (int)sizeof(current->arg); ai++) current->arg[ai] = new_arg[ai];

            uint64_t entry_vaddr = img.entry;
            uint64_t stack_top = img.stack_top;
            uint32_t num_image_pages = img.image_pages;
            uint32_t num_stack_pages = img.stack_pages;
            uint64_t load_base = img.load_base;

            // 8. Старое пространство заменяем новым и освобождаем его
            uint64_t old_cr3 = current->cr3;
            current->cr3 = (uint64_t)new_pml4;
            vmm_destroy_address_space((uint64_t*)old_cr3);
            lib_release_pid(current->pid);     // образы .ll ушли вместе со старым пространством

            current->heap_start = load_base + (uint64_t)num_image_pages * 4096;
            current->heap_end   = current->heap_start;
            current->mem_size   = ((uint64_t)num_image_pages + (uint64_t)num_stack_pages) * 4096 + 4096 + PROC_KSTACK_SIZE;

            // 9. Обновляем имя процесса в task_t для sched_dump_tasks
            const char* bname = filename;
            for (int i = 0; filename[i]; i++) {
                if (filename[i] == '/') bname = &filename[i + 1];
            }
            int np = 0;
            while (bname[np] && np < 15) {
                char c = bname[np];
                if (c >= 'a' && c <= 'z') c -= 32;
                caller->name[np] = c;
                np++;
            }
            caller->name[np] = '\0';
            sched_set_exe(caller, filename, caller->cwd);

            // 10. Очистка дескрипторов старого процесса (0, 1, 2 сохраняются)
            for (int fd = 3; fd < MAX_FD; fd++) {
                if (current->fd_table[fd]) {
                    vfs_release_fd(current->fd_table[fd]);
                    current->fd_table[fd] = NULL;
                }
            }

            vfs_node_t* tty_node = devfs_get_node("tty");
            if (tty_node) {
                if (!current->fd_table[0]) current->fd_table[0] = vfs_allocate_fd(tty_node, O_RDONLY);
                if (!current->fd_table[1]) current->fd_table[1] = vfs_allocate_fd(tty_node, O_WRONLY);
                if (!current->fd_table[2]) current->fd_table[2] = vfs_allocate_fd(tty_node, O_WRONLY);
            }

            // 11. Подменяем RIP/RSP в кадре прерывания для перехода в новую программу
            frame[15] = entry_vaddr;   // RIP
            frame[18] = stack_top;     // RSP

            return 0;
        }
        // 40..48 — бывший оконный сервер ядра и почтовый ящик ошибок: окна теперь живут
        // в desktop (оконный протокол поверх IPC и разделяемой памяти, src/libs/common/wproto.h)

        case 49: { // sys_get_arg(char* out, int max_len) -> len
            char* u_out = (char*)arg1;
            int max_l = (int)arg2;
            if (!u_out || max_l <= 0) return 0;
            if (!caller) return 0;
            int i = 0;
            while (caller->arg[i] && i < max_l - 1) {
                u_out[i] = caller->arg[i];
                i++;
            }
            u_out[i] = '\0';
            return (uint64_t)i;
        }

        case 50: { // sys_reboot()
            extern void sys_reboot(void);
            sys_reboot();
            return 0;
        }

        case 51: { // sys_shutdown()
            extern void acpi_power_off(void);
            acpi_power_off();
            return 0;
        }

        case 60: { // sys_get_net_info(sys_net_info_t* info)
            if (!arg1) return (uint64_t)-1;
            __asm__ volatile ("sti");
            extern int net_get_status_info(void* out_buf);
            int res = net_get_status_info((void*)arg1);
            __asm__ volatile ("cli");
            return (uint64_t)res;
        }
        // ---------------- Пользователи и права (UNIX DAC) ----------------
        case 80: return (uint64_t)(int64_t)c_uid;          // sys_getuid()
        case 81: return (uint64_t)(int64_t)c_gid;          // sys_getgid()

        case 82: { // sys_setuid(uid) — только root (так greeter/login отдают сессию пользователю)
            if (!caller || c_uid != UID_ROOT) return (uint64_t)-1;
            caller->uid = (int)arg1;
            return 0;
        }
        case 83: { // sys_setgid(gid) — только root
            if (!caller || c_uid != UID_ROOT) return (uint64_t)-1;
            caller->gid = (int)arg1;
            return 0;
        }

        case 84: { // sys_auth(name, pass) -> uid или -1
            char kname[USER_NAME_MAX], kpass[128];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            ucopy_str(kpass, (const char*)arg2, sizeof(kpass));
            int uid = users_auth(kname, kpass);
            for (int i = 0; i < (int)sizeof(kpass); i++) kpass[i] = 0;
            if (uid < 0) {
                // задержка после неудачи, как pam_faildelay: перебор становится дорогим
                uint32_t t0 = now_ms();
                while (now_ms() - t0 < 1000) sched_yield();
            }
            return (uint64_t)(int64_t)uid;
        }

        case 85: { // sys_getpwuid(uid, user_info_t* out) -> 1 / 0
            user_info_t info;
            if (!arg2 || !users_find_by_uid((int)arg1, &info)) return 0;
            wm_copy((void*)arg2, &info, sizeof(info));
            return 1;
        }

        case 86: { // sys_chmod(path, mode) — владелец или root
            char kpath[PERM_PATH_MAX], npath[PERM_PATH_MAX];
            ucopy_str(kpath, (const char*)arg1, sizeof(kpath));
            perm_normalize(kpath, fs_caller_cwd(), npath, sizeof(npath));
            perm_info_t pi;
            perm_get(npath, &pi);
            if (c_uid != UID_ROOT && c_uid != pi.uid) return (uint64_t)-1;
            return perm_set(npath, "/", pi.uid, pi.gid, (int)arg2) ? 0 : (uint64_t)-1;
        }

        case 87: { // sys_chown(path, uid, gid) — только root
            if (c_uid != UID_ROOT) return (uint64_t)-1;
            char kpath[PERM_PATH_MAX], npath[PERM_PATH_MAX];
            ucopy_str(kpath, (const char*)arg1, sizeof(kpath));
            perm_normalize(kpath, fs_caller_cwd(), npath, sizeof(npath));
            perm_info_t pi;
            perm_get(npath, &pi);
            return perm_set(npath, "/", (int)arg2, (int)arg3, pi.mode) ? 0 : (uint64_t)-1;
        }

        case 88: { // sys_stat_perm(path, perm_info_t* out)
            if (!arg2) return (uint64_t)-1;
            char kpath[PERM_PATH_MAX], npath[PERM_PATH_MAX];
            ucopy_str(kpath, (const char*)arg1, sizeof(kpath));
            perm_normalize(kpath, fs_caller_cwd(), npath, sizeof(npath));
            perm_info_t pi;
            perm_get(npath, &pi);
            wm_copy((void*)arg2, &pi, sizeof(pi));
            return 0;
        }

        case 89: { // sys_passwd(name, new_pass, old_pass) — root меняет любой, остальные свой со старым
            char kname[USER_NAME_MAX], kpass[128], kold[128];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            ucopy_str(kpass, (const char*)arg2, sizeof(kpass));
            ucopy_str(kold,  (const char*)arg3, sizeof(kold));
            user_info_t info;
            int ok = users_find_by_name(kname, &info);
            if (ok && c_uid != UID_ROOT) ok = (c_uid == info.uid) && users_auth(kname, kold) == info.uid;
            if (ok) ok = users_set_password(kname, kpass);
            for (int i = 0; i < (int)sizeof(kpass); i++) { kpass[i] = 0; kold[i] = 0; }
            return ok ? 0 : (uint64_t)-1;
        }

        case 90: { // sys_useradd(name, pass) -> uid или -1 — только root (OOBE, админка)
            if (c_uid != UID_ROOT) return (uint64_t)-1;
            char kname[USER_NAME_MAX], kpass[128];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            ucopy_str(kpass, (const char*)arg2, sizeof(kpass));
            int uid = users_add(kname, kpass, (int)arg3);   // arg3 = 1: администратор (wheel)
            for (int i = 0; i < (int)sizeof(kpass); i++) kpass[i] = 0;
            return (uint64_t)(int64_t)uid;
        }

        case 93: { // sys_sudo(pass) — администратор (wheel) со СВОИМ паролем становится root (polkit/sudo)
            if (!caller) return (uint64_t)-1;
            if (c_uid == UID_ROOT) return 0;
            char kpass[128];
            ucopy_str(kpass, (const char*)arg1, sizeof(kpass));
            user_info_t me;
            int ok = users_is_admin(c_uid) && users_find_by_uid(c_uid, &me) && users_auth(me.name, kpass) == c_uid;
            for (int i = 0; i < (int)sizeof(kpass); i++) kpass[i] = 0;
            if (!ok) {
                uint32_t t0 = now_ms();
                while (now_ms() - t0 < 1000) sched_yield();
                return (uint64_t)-1;
            }
            caller->uid = UID_ROOT;
            caller->gid = 0;
            return 0;
        }

        case 94: // sys_is_admin(uid) -> 1, если может sudo
            return (uint64_t)users_is_admin((int)arg1);

        case 95: { // sys_get_system(sys_settings_t* out): язык, пояс, смещение, вторая раскладка
            if (!arg1) return (uint64_t)-1;
            int v[4] = { users_sys_lang(), users_sys_tz_index(), users_sys_tz_offset(), users_sys_layout2() };
            wm_copy((void*)arg1, v, sizeof(v));
            return 0;
        }

        case 96: // sys_set_system(lang, tz_index, layout2) — только root; -1 = не менять
            if (c_uid != UID_ROOT) return (uint64_t)-1;
            users_set_system((int)arg1, (int)arg2, (int)arg3);
            return 0;

        case 97: { // sys_tz_info(index, tz_info_t* out) -> число поясов (out заполняется, если index в списке)
            int idx = (int)arg1;
            if (arg2 && idx >= 0 && idx < g_timezone_count) {
                struct { char name[32]; char city[64]; int offset; } info;
                ucopy_str(info.name, g_timezones[idx].name, sizeof(info.name));
                ucopy_str(info.city, g_timezones[idx].city_ru, sizeof(info.city));
                info.offset = g_timezones[idx].offset;
                wm_copy((void*)arg2, &info, sizeof(info));
            }
            return (uint64_t)g_timezone_count;
        }

        case 98: { // sys_lock_user(name) — только root: запрет входа по паролю (root после OOBE)
            if (c_uid != UID_ROOT) return (uint64_t)-1;
            char kname[USER_NAME_MAX];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            return users_lock(kname) ? 0 : (uint64_t)-1;
        }

        case 129: { // sys_tty_history(idx, buf, max): idx < 0 -- только количество
            if (!caller || caller->tty_id < 0) return 0;
            int tid = caller->tty_id;
            int n = tty_hist_count(tid);
            if ((int64_t)arg1 < 0 || !arg2) return (uint64_t)n;
            const char* line = tty_hist_get(tid, (int)arg1);
            if (!line) return 0;
            int len = 0;
            while (line[len]) len++;
            if ((uint64_t)len + 1 > arg3) len = (int)arg3 - 1;
            if (len < 0) return 0;
            wm_copy((void*)arg2, line, (uint64_t)len);
            ((char*)arg2)[len] = '\0';
            return (uint64_t)tty_hist_number(tid, (int)arg1);
        }

        case 143: { // sys_cpu_sensors(out): температура и измеренная частота (overMark, overTask)
            if (!arg1) return (uint64_t)-1;
            int pkg, core, tj;
            int ok = cpu_temperature(&pkg, &core, &tj);
            int32_t o[6] = { ok, pkg, core, tj, cpu_current_mhz(), cpu_get()->base_mhz };
            uint8_t* d = (uint8_t*)arg1;
            for (int i = 0; i < 24; i++) d[i] = ((uint8_t*)o)[i];
            return (uint64_t)ok;
        }
        case 130: { // sys_cpuinfo(out): модель, ядра, частоты, режим управления
            if (!arg1) return (uint64_t)-1;
            const cpu_info_t* ci = cpu_get();
            struct {
                char brand[52];
                int32_t cores, threads;
                int32_t base_mhz, max_mhz, cur_mhz;
                int32_t turbo_on, governor;
            } out;
            ucopy_str(out.brand, ci->brand[0] ? ci->brand : ci->vendor, sizeof(out.brand));
            out.cores    = ci->cores;
            out.threads  = ci->threads;
            out.base_mhz = ci->base_mhz;
            out.max_mhz  = ci->max_mhz;
            out.cur_mhz  = ci->cur_mhz;
            out.turbo_on = ci->turbo_on;
            out.governor = ci->governor;
            wm_copy((void*)arg1, &out, sizeof(out));
            return 0;
        }

        case 128: { // sys_meminfo(uint64_t out[2]): всего и занято байт физпамяти
            if (!arg1) return (uint64_t)-1;
            uint64_t v[2] = { pmm_get_total_blocks() * (uint64_t)PAGE_SIZE,
                              pmm_get_used_blocks()  * (uint64_t)PAGE_SIZE };
            wm_copy((void*)arg1, v, sizeof(v));
            return 0;
        }

        case 132: // sys_storage_state(): 0 -- флешка с системой на месте, 1 -- выдернута,
                  // 2 -- вместо неё вставили другую. Рабочий стол опрашивает раз в
                  // секунду и показывает уведомление; на шину вызов не ходит.
            return (uint64_t)mount_root_state();

        case 133: { // sys_notify_poll(knotify_t*): следующее уведомление системы (только root --
                    // это рабочий стол, демон уведомлений). 1 -- есть, 0 -- очередь пуста.
            if (!caller || caller->uid != 0 || !arg1) return (uint64_t)-1;
            knotify_t n;
            if (!knotify_pop(&n)) return 0;
            wm_copy((void*)arg1, &n, sizeof(n));
            return 1;
        }

        case 134: { // sys_mounts(mount_info_t* out, max): подключённые тома, корень первым
            int max = (int)arg2;
            if (max <= 0 || !arg1) return 0;
            mount_info_t tmp[16];
            int n = mount_list(tmp, max > 16 ? 16 : max);
            wm_copy((void*)arg1, tmp, (uint64_t)n * sizeof(mount_info_t));
            return (uint64_t)n;
        }

        case 136: { // sys_proc_exe(pid, buf, len): путь к исполняемому файлу процесса
            char kb[96];
            if (sched_proc_exe(arg1, kb, sizeof(kb)) != 0) return (uint64_t)-1;
            int n = 0;
            while (kb[n] && n < (int)arg3 - 1) n++;
            kb[n] = '\0';
            wm_copy((void*)arg2, kb, (uint64_t)n + 1);
            return 0;
        }

        case 135: { // sys_eject(name): безопасно извлечь накопитель (как "Отключить" в Dolphin).
                    // Разрешено любому вошедшему: флешку и так можно просто выдернуть.
            char kn[32];
            ucopy_str(kn, (const char*)arg1, sizeof(kn));
            return (uint64_t)(int64_t)mount_eject(kn);
        }

        case 131: { // sys_diskinfo(ov_diskinfo_t*): размер раздела и обмен с флешкой
            if (!arg1) return (uint64_t)-1;
            // Занятое место считается проходом по всей таблице FAT, то есть
            // сотнями секторов с накопителя. Диспетчер задач спрашивает раз в
            // секунду, и на настоящей флешке это была бы заметная нагрузка
            // (она же показалась бы в собственном счётчике обмена), поэтому
            // держим ответ десять секунд.
            if (!disk_stat_total || (uint32_t)(timer_ticks - disk_stat_tick) > 10000) {
                fs_get_stats(&disk_stat_used, &disk_stat_total);
                disk_stat_tick = timer_ticks;
            }
            uint32_t used_mb = disk_stat_used, total_mb = disk_stat_total;
            // Сектор флешки — 512 байт, отдаём сразу в килобайтах
            uint64_t v[4] = { (uint64_t)total_mb, (uint64_t)used_mb,
                              g_blk_rd_sectors / 2, g_blk_wr_sectors / 2 };
            wm_copy((void*)arg1, v, sizeof(v));
            return 0;
        }

        // ---------------- Псевдотерминалы для графического эмулятора ----------------
        case 123: { // sys_pty_open(w_px, h_px) -> id псевдотерминала или -1
            if (!caller) return (uint64_t)-1;
            return (uint64_t)pty_open((int)arg1, (int)arg2, caller->uid, caller->gid, caller->pid);
        }

        case 124: { // sys_pty_read(id, dst, max_bytes) -> скопировано байт картинки
            tty_t* pt = tty_get((int)arg1);
            if (!pt || !pt->is_pty || pt->owner_pid != caller->pid) return (uint64_t)-1;
            return (uint64_t)pty_snapshot((int)arg1, (uint32_t*)arg2, arg3);
        }

        case 125: { // sys_pty_key(id, scancode)
            tty_t* pt = tty_get((int)arg1);
            if (!pt || !pt->is_pty || pt->owner_pid != caller->pid) return (uint64_t)-1;
            tty_kbd_push((int)arg1, (uint8_t)arg2);
            return 0;
        }

        case 126: { // sys_pty_close(id)
            tty_t* pt = tty_get((int)arg1);
            if (!pt || !pt->is_pty || pt->owner_pid != caller->pid) return (uint64_t)-1;
            pty_close((int)arg1);
            return 0;
        }

        case 127: { // sys_pty_alive(id) -> 1, пока оболочка жива
            tty_t* pt = tty_get((int)arg1);
            if (!pt || !pt->is_pty || pt->owner_pid != caller->pid) return 0;
            return (uint64_t)pty_alive((int)arg1);
        }

        case 122: { // sys_mouse(cmd, speed, accel): 0 = прочитать, 1 = задать
            if (arg1 == 0) {
                // упакованно: скорость в младших 16 битах, ускорение в 16-м
                return (uint64_t)((uint32_t)mouse_get_speed() | ((uint32_t)mouse_get_accel() << 16));
            }
            if (arg1 == 1) {
                // Менять скорость указателя может администратор: файл общий для системы
                if (c_uid != UID_ROOT && !users_is_admin(c_uid)) return (uint64_t)-1;
                users_set_mouse((int)arg2, (int)arg3);
                return 0;
            }
            return (uint64_t)-1;
        }

        case 91: // sys_oobe_state() -> 1, если нужна первоначальная настройка
            return (uint64_t)users_needs_oobe();

        case 92: // sys_oobe_finish() — только root
            if (c_uid != UID_ROOT) return (uint64_t)-1;
            users_finish_oobe();
            return 0;

        // ---------------- IPC: сообщения, имена сервисов, разделяемая память ----------------
        case 100: // sys_ipc_send(pid, buf, len) -> 0 / -1 нет процесса / -2 очередь полна
            return (uint64_t)(int64_t)ipc_send(caller, (int)arg1, (const void*)arg2, (uint32_t)arg3);
        case 101: { // sys_ipc_recv(ipc_msg_t* out, timeout_ms) -> 1 есть / 0 таймаут
            ipc_msg_t m;
            int r = ipc_recv(caller, &m, (uint32_t)arg2);
            if (r == 1) wm_copy((void*)arg1, &m, sizeof(m));
            return (uint64_t)r;
        }
        case 102: { // sys_ipc_register(name) — только root (системные сервисы)
            char kname[IPC_NAME_MAX];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            return (uint64_t)(int64_t)ipc_register(caller, kname);
        }
        case 103: { // sys_ipc_lookup(name) -> pid / -1
            char kname[IPC_NAME_MAX];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            return (uint64_t)(int64_t)ipc_lookup(kname);
        }
        case 104: return (uint64_t)(int64_t)shm_create(caller, arg1);          // sys_shm_create(size) -> id
        case 105: return shm_map(caller, (int)arg1);                           // sys_shm_map(id) -> адрес / 0
        case 106: return (uint64_t)(int64_t)shm_unmap(caller, (int)arg1);      // sys_shm_unmap(id)
        case 107: return (uint64_t)(int64_t)shm_grant(caller, (int)arg1, (int)arg2);   // sys_shm_grant(id, pid)
        case 108: return (uint64_t)(int64_t)shm_destroy(caller, (int)arg1);    // sys_shm_destroy(id)
        case 111: return shm_size((int)arg1);                                  // sys_shm_size(id)

        case 109: { // sys_spawn(path, arg, uidgid) -> pid: запуск без fork (не копирует память родителя)
            // uidgid = (uid << 32) | gid — от чьего имени; задать чужого может только root (desktop)
            char kpath[128], karg[128];
            ucopy_str(kpath, (const char*)arg1, sizeof(kpath));
            ucopy_str(karg, (const char*)arg2, sizeof(karg));
            int uid = c_uid, gid = c_gid;
            if (arg3 != (uint64_t)-1) {
                if (c_uid != UID_ROOT) return (uint64_t)-1;
                uid = (int)(arg3 >> 32);
                gid = (int)(arg3 & 0xFFFFFFFF);
            }
            int err;
            task_t* p = prog_spawn(kpath, karg, uid, gid, 1, &err);
            if (p) sched_start_process(p);          // sys_spawn: передним планом не владеет
            return p ? p->pid : (uint64_t)(int64_t)err;
        }

        case 112: { // sys_tty_setfg(pid) — отдать клавиатуру TTY процессу (как tcsetpgrp)
            // Менять может только текущий владелец переднего плана или root: иначе
            // фоновый процесс увёл бы себе ввод следующего пользователя
            if (!caller || caller->tty_id < 0) return (uint64_t)-1;
            tty_t* t = tty_get(caller->tty_id);
            if (!t) return (uint64_t)-1;
            // Забрать ввод можно, если он ничей, уже наш, прежний владелец умер
            // (так оболочка получает клавиатуру обратно после программы) или мы root
            if (c_uid != UID_ROOT && t->fg_pid > 0 && t->fg_pid != (int)caller->pid &&
                sched_pid_is_process(t->fg_pid)) return (uint64_t)-1;
            int target = (int)(int64_t)arg1;
            if (target == 0) target = (int)caller->pid;           // забрать ввод себе
            if (target > 0 && !sched_pid_is_process(target)) return (uint64_t)-1;
            t->fg_pid = target;
            // Оболочка забрала ввод обратно после программы: перерисовываем консоль,
            // иначе на экране остаётся последний кадр игры поверх текста
            if (target == (int)caller->pid) flush_buffer();
            return 0;
        }

        case 113: { // sys_ps(sched_proc_info_t* out, max) -> сколько записей
            int max = (int)arg2;
            if (max <= 0) return 0;
            return (uint64_t)(int64_t)sched_task_list((sched_proc_info_t*)arg1, max);
        }

        case 114: { // sys_chdir(path) -> 0 / -1: каталог процесса (у каждого свой)
            char kp[128], abs[128];
            ucopy_str(kp, (const char*)arg1, sizeof(kp));
            if (!caller) return (uint64_t)-1;
            perm_normalize(kp, caller->cwd, abs, sizeof(abs));
            if (!perm_check(c_uid, c_gid, abs, caller->cwd, PERM_X)) return (uint64_t)-1;
            fs_lock();
            int ok = fs_dir_exists(abs);
            fs_unlock();
            if (!ok) return (uint64_t)-1;
            int i = 0;
            while (abs[i] && i < (int)sizeof(caller->cwd) - 1) { caller->cwd[i] = abs[i]; i++; }
            caller->cwd[i] = '\0';
            return 0;
        }

        case 115: { // sys_getcwd(buf, max) -> длина
            if (!caller) return 0;
            char* out = (char*)arg1;
            int max = (int)arg2;
            int i = 0;
            while (caller->cwd[i] && i < max - 1) { out[i] = caller->cwd[i]; i++; }
            if (max > 0) out[i] = '\0';
            return (uint64_t)i;
        }

        case 116: { // sys_su(name, pass) -> 0: стать другим пользователем по ЕГО паролю
            // root переключается без пароля (как su в UNIX), остальные — только зная пароль
            char kname[32], kpass[64];
            ucopy_str(kname, (const char*)arg1, sizeof(kname));
            ucopy_str(kpass, (const char*)arg2, sizeof(kpass));
            user_info_t info;
            if (c_uid == UID_ROOT) {
                if (!users_find_by_name(kname, &info)) return (uint64_t)-1;
            } else {
                int uid = users_auth(kname, kpass);
                if (uid < 0 || !users_find_by_uid(uid, &info)) return (uint64_t)-1;
            }
            if (caller) { caller->uid = info.uid; caller->gid = info.gid; }
            return 0;
        }

        case 117: { // sys_load_module(path, args) — только root (.sys работает в Ring 0)
            if (c_uid != UID_ROOT) return (uint64_t)-1;
            char kp[128], ka[128];
            ucopy_str(kp, (const char*)arg1, sizeof(kp));
            ucopy_str(ka, (const char*)arg2, sizeof(ka));
            return sys_load_module(kp, ka) ? 0 : (uint64_t)-1;
        }

        case 118: { // sys_kcmd(line) -> 1, если команду знает встроенная часть ядра
            // Мост на время переноса: сервисные команды ядра (taskmgr, hda-dump,
            // ping, fastfetch, модули) ещё живут в main.c. Пользовательские
            // команды и сеанс уже в Ring 3 (sh.prg), эти переедут следующими.
            char kl[256];
            ucopy_str(kl, (const char*)arg1, sizeof(kl));
            return (uint64_t)(int64_t)shell_kernel_builtin(kl);
        }

        case 110: return caller ? caller->pid : 0;                              // sys_getpid()

        // ---- разделяемые библиотеки .ll (linked library): dlopen/dlsym/dlclose ----
        case 119: { // sys_lib_open(path) -> дескриптор >= 0 или LIB_E*
            char lp[128];
            ucopy_str(lp, (const char*)arg1, sizeof(lp));
            return (uint64_t)(int64_t)lib_open(lp, c_uid, c_gid);
        }
        case 120: { // sys_lib_sym(handle, name) -> адрес в памяти процесса или 0
            char nm[64];
            ucopy_str(nm, (const char*)arg2, sizeof(nm));
            return lib_sym((int)arg1, nm);
        }
        case 121: // sys_lib_close(handle)
            return (uint64_t)(int64_t)lib_close((int)arg1);

        case 70: { // sys_audio(cmd, arg1, arg2)
            uint64_t cmd = arg1; // подкоманда из RBX
            switch (cmd) {
                case 0: // get_status
                    return (uint64_t)hda_get_status();

                case 1: { // get_outputs (arg2 = указатель на hda_outputs_t)
                    if (!arg2) return (uint64_t)-2;
                    return (uint64_t)check_audio_outputs((hda_outputs_t*)arg2);
                }

                case 2: // set_output (arg2 = mode: 0 AUTO, 1 SPEAKERS, 2 HEADPHONES, 3 HDMI)
                    return (uint64_t)hda_set_output((int)arg2);

                case 3: // get_output
                    return (uint64_t)hda_get_output();

                case 4: // play_test
                    hda_play_test_tone();
                    return 0;

                case 5: // stop -- гасим только тестовый тон, чужой звук не трогаем
                    hda_stop_test_tone();
                    return 0;

                case 6: // jack_poll
                    return (uint64_t)hda_jack_poll();

                // ---- потоковый PCM для плееров и громкость ----
                case 7: // pcm_open(arg2 = частота, arg3 = каналы | (бит на сэмпл << 8)) -> 0 или <0
                    return (uint64_t)(int64_t)hda_pcm_open((uint32_t)arg2, (int)(arg3 & 0xFF), (int)((arg3 >> 8) & 0xFF),
                                                           caller ? caller->pid : 0);

                case 8: { // pcm_write(arg2 = указатель на PCM, arg3 = байт) -> сколько принято (0 = очередь полна)
                    if (!arg2 || arg2 >= 0x0000800000000000ULL) return 0;
                    if (arg3 > 0x100000) arg3 = 0x100000;
                    return (uint64_t)(int64_t)hda_pcm_write((const void*)arg2, (uint32_t)arg3, caller ? caller->pid : 0);
                }

                case 9:  // pcm_close
                    hda_pcm_close(caller ? caller->pid : 0);
                    return 0;

                case 10: // pcm_flush (перемотка/пауза: сбросить очередь и позицию)
                    hda_pcm_flush(caller ? caller->pid : 0);
                    return 0;

                case 11: // pcm_position_ms
                    return (uint64_t)hda_pcm_position_ms(caller ? caller->pid : 0);

                case 12: // pcm_queued_ms
                    return (uint64_t)hda_pcm_queued_ms(caller ? caller->pid : 0);

                case 13: // pcm_drained -> 1, если всё записанное проиграно
                    return (uint64_t)hda_pcm_drained(caller ? caller->pid : 0);

                case 14: // set_volume (arg2 = 0..100)
                    hda_set_volume((int)arg2);
                    return 0;

                case 15: // get_volume
                    return (uint64_t)hda_get_volume();

                case 16: // set_mute (arg2 = 0/1)
                    hda_set_mute((int)arg2);
                    return 0;

                case 17: // get_target -> куда реально идёт звук: 0 никуда, 1 аналог, 2 HDMI
                    return (uint64_t)hda_get_target();

                case 18: { // get_caps -> маска: 1 динамики, 2 наушники/line-out, 4 HDMI
                    hda_outputs_t o;
                    if (check_audio_outputs(&o) < 0) return 0;
                    return (uint64_t)((o.has_speakers ? 1 : 0) |
                                      ((o.has_headphones || o.has_lineout) ? 2 : 0) |
                                      (o.hdmi_playback ? 4 : 0));
                }

                case 19: // pcm_underruns
                    return (uint64_t)hda_pcm_underruns(caller ? caller->pid : 0);

                default:
                    return (uint64_t)-1;
            }
        }

        default:
            return 0;
    }
    return 0;   // case 23 (мышь DOOM) выходит через break — без этого в RAX оставался мусор
}

// Первая "точка возврата" для ребёнка sys_fork: на стеке под ней уже лежит
// копия кадра прерывания родителя (см. case 30). Эти 15 pop + iretq —
// побайтово то же самое, что .L_normal_iretq ниже: ребёнок продолжит
// исполнение Ring 3 с той же RIP/RSP, что и родитель в момент fork(),
// но с RAX = 0 (это значение подставлено в кадр заранее).
__attribute__((naked)) static void fork_child_resume(void) {
    __asm__ volatile (
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        "pop %rbp\n\t"
        "iretq\n\t"
    );
}

__attribute__((naked)) static void syscall_isr_stub(void) {
    __asm__ volatile (
        "push %rbp\n\t"
        "mov %rsp, %rbp\n\t"
        
        "push %rax\n\t"
        "push %rbx\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"

        "mov %rax, %r12\n\t"

        // 5-й аргумент (SysV: r8) — указатель на начало только что сохранённого
        // блока регистров (r15 был запушен последним, значит лежит по текущему rsp).
        // Берём ДО перестановки остальных регистров под аргументы 1-4: этот mov
        // не трогает rsp, так что адрес кадра остаётся верным.
        "mov %rsp, %r8\n\t"

        "mov %rdx, %r9\n\t"
        "mov %rax, %rdi\n\t"
        "mov %rbx, %rsi\n\t"
        "mov %rcx, %rdx\n\t"
        "mov %r9,  %rcx\n\t"

        // Вызов с рабочего ядра (стек в smp_ap_stacks) в ядро ОС не пускаем:
        // оно однопроцессорное. Там свой маленький набор (smp.c).
        "lea smp_ap_stacks(%rip), %r10\n\t"
        "cmp %r10, %rsp\n\t"
        "jb 2f\n\t"
        "add $262144, %r10\n\t"            // SMP_MAX * SMP_AP_STACK (проверка ниже)
        "cmp %r10, %rsp\n\t"
        "jae 2f\n\t"
        "call smp_ap_syscall\n\t"
        "jmp 3f\n\t"
        "2:\n\t"
        "call syscall_dispatcher\n\t"
        "3:\n\t"

        "mov %rax, 104(%rsp)\n\t"

        "cmpq $0, %r12\n\t"
        "jne .L_normal_iretq\n\t"

        // ЕСЛИ ЭТО EXIT: восстанавливаем оригинальный стек ядра
        "movq saved_kernel_rsp(%rip), %rsp\n\t"

        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rbx\n\t"
        "pop %rbp\n\t"

        "movq $0, g_user_mode_active\n\t"
        "movq kernel_exit_code(%rip), %rax\n\t" // Загружаем точный код выхода в RAX
        "ret\n\t"

    ".L_normal_iretq:\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        
        "pop %rbp\n\t"
        
        "iretq\n\t"
    );
}

// Структуры IDT и TSS
typedef struct {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t base_middle;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry64_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr64_t;

static idt_entry64_t idt64[256] __attribute__((aligned(16)));
static idt_ptr64_t   idtr64;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

static tss64_t kernel_tss;

extern void timer_isr_asm(void); // Обработчик таймера PIT/IRQ0 (interrupts.asm) — нужен ниже для idt64[32]

// Настоящие обработчики исключений (interrupts.asm): каждый кладёт номер своего
// вектора на стек перед общим кадром и зовёт default_exception_handler (bsod.c) —
// именно оттуда bsod.c берёт exc_no/fault_eip/fault_esp (frame[15]/[17]/[20]).
// Раньше здесь на все 32 вектора стоял один default_isr_stub, который номер
// вектора вообще не знал и в Ring 0 просто делал hlt без всякой диагностики.
extern void isr_0(void);  extern void isr_1(void);  extern void isr_2(void);  extern void isr_3(void);
extern void isr_4(void);  extern void isr_5(void);  extern void isr_6(void);  extern void isr_7(void);
extern void isr_8(void);  extern void isr_9(void);  extern void isr_10(void); extern void isr_11(void);
extern void isr_12(void); extern void isr_13(void); extern void isr_14(void); extern void isr_15(void);
extern void isr_16(void); extern void isr_17(void); extern void isr_18(void); extern void isr_19(void);
extern void isr_20(void); extern void isr_21(void); extern void isr_22(void); extern void isr_23(void);
extern void isr_24(void); extern void isr_25(void); extern void isr_26(void); extern void isr_27(void);
extern void isr_28(void); extern void isr_29(void); extern void isr_30(void); extern void isr_31(void);

static void (*const exc_isr_table[32])(void) = {
    isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7,
    isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15,
    isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23,
    isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31,
};

extern uint64_t gdt64_tss[];

typedef struct {
    uint16_t limit_low;
    uint16_t base_low16;
    uint8_t  base_mid8;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high8;
    uint32_t base_upper32;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_desc_t;

// Увеличиваем стек ядра до безопасных 16 КБ (выравнивание 16 байт по ABI)
static uint8_t kernel_interrupt_stack[16384] __attribute__((aligned(16)));

// Заглушка для фантомных и необработанных аппаратных прерываний (IRQ 7, IRQ 15 и др.)
__attribute__((naked)) static void spurious_irq_stub(void) {
    __asm__ volatile ("iretq");
}

void init_user_mode(void) {
    // Временно маскируем все IRQ на контроллере PIC
    __asm__ volatile ("outb %0, $0x21" : : "a"((uint8_t)0xFF));
    __asm__ volatile ("outb %0, $0xA1" : : "a"((uint8_t)0xFF));

    // Очереди клавиатуры по TTY уже обнулены в tty_init_core/tty_spawn;
    // состояние мыши — в mouse.c, туда отдельного сброса тоже не нужно.

    for (int i = 0; i < (int)sizeof(kernel_tss); i++) ((uint8_t*)&kernel_tss)[i] = 0;
    
    // Вершина проверенного статического стека ядра
    uint64_t stack_base = (uint64_t)((uintptr_t)kernel_interrupt_stack);
    kernel_tss.rsp0 = stack_base + sizeof(kernel_interrupt_stack);
    kernel_tss.iomap_base = sizeof(kernel_tss);

    gdt_tss_desc_t* tss_desc = (gdt_tss_desc_t*)gdt64_tss;
    uint64_t tss_base = (uint64_t)&kernel_tss;
    uint32_t tss_limit = sizeof(kernel_tss) - 1;

    tss_desc->limit_low    = tss_limit & 0xFFFF;
    tss_desc->base_low16   = tss_base & 0xFFFF;
    tss_desc->base_mid8    = (tss_base >> 16) & 0xFF;
    tss_desc->access       = 0x89;
    tss_desc->flags_limit  = (tss_limit >> 16) & 0x0F;
    tss_desc->base_high8   = (tss_base >> 24) & 0xFF;
    tss_desc->base_upper32 = (tss_base >> 32);
    tss_desc->reserved     = 0;

    __asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));

    // Обнуляем все 256 дескрипторов IDT
    for (int i = 0; i < 256; i++) {
        idt64[i].base_low    = 0;
        idt64[i].selector    = 0;
        idt64[i].ist         = 0;
        idt64[i].type_attr   = 0;
        idt64[i].base_middle = 0;
        idt64[i].base_high   = 0;
        idt64[i].reserved    = 0;
    }

    // 1. Исключения процессора (векторы 0..31, DPL 0) — настоящий isr_N на каждый
    for (int i = 0; i < 32; i++) {
        uint64_t exc_handler = (uint64_t)exc_isr_table[i];
        idt64[i].base_low    = exc_handler & 0xFFFF;
        idt64[i].selector    = 0x08;
        idt64[i].ist         = 0;
        idt64[i].type_attr   = 0x8E;
        idt64[i].base_middle = (exc_handler >> 16) & 0xFFFF;
        idt64[i].base_high   = (exc_handler >> 32) & 0xFFFFFFFF;
        idt64[i].reserved    = 0;
    }

    // 2. Все аппаратные линии IRQ 0..15 (векторы 32..47)
    // Закрываем безопасной заглушкой iretq от спонтанных всплесков реального чипсета
    uint64_t spurious_handler = (uint64_t)spurious_irq_stub;
    for (int i = 32; i < 48; i++) {
        idt64[i].base_low    = spurious_handler & 0xFFFF;
        idt64[i].selector    = 0x08;
        idt64[i].ist         = 0;
        idt64[i].type_attr   = 0x8E;
        idt64[i].base_middle = (spurious_handler >> 16) & 0xFFFF;
        idt64[i].base_high   = (spurious_handler >> 32) & 0xFFFFFFFF;
        idt64[i].reserved    = 0;
    }

    // 3. Вектор 32 — рабочий таймер PIT/IRQ0
    uint64_t timer_handler = (uint64_t)timer_isr_asm;
    idt64[32].base_low    = timer_handler & 0xFFFF;
    idt64[32].selector    = 0x08;
    idt64[32].ist         = 0;
    idt64[32].type_attr   = 0x8E;
    idt64[32].base_middle = (timer_handler >> 16) & 0xFFFF;
    idt64[32].base_high   = (timer_handler >> 32) & 0xFFFFFFFF;
    idt64[32].reserved    = 0;

    // 4. Вектор 0x80 — шлюз системных вызовов (DPL 3 для Ring 3)
    uint64_t syscall_handler = (uint64_t)syscall_isr_stub;
    idt64[0x80].base_low    = syscall_handler & 0xFFFF;
    idt64[0x80].selector    = 0x08;
    idt64[0x80].ist         = 0;
    idt64[0x80].type_attr   = 0xEE; // Разрешён вызов из User Mode
    idt64[0x80].base_middle = (syscall_handler >> 16) & 0xFFFF;
    idt64[0x80].base_high   = (syscall_handler >> 32) & 0xFFFFFFFF;
    idt64[0x80].reserved    = 0;

    // Загружаем обновлённую таблицу дескрипторов прерываний
    idtr64.limit = sizeof(idt64) - 1;
    idtr64.base  = (uint64_t)&idt64;

    __asm__ volatile("lidt %0" : : "m"(idtr64));

    g_user_mode_active = 0;
}

// Рабочие ядра (smp.c) делят с основным один IDT
void idt_load_this_cpu(void) {
    __asm__ volatile("lidt %0" : : "m"(idtr64));
}

void idt_set_gate(int vec, void (*handler)(void), int dpl) {
    uint64_t h = (uint64_t)handler;
    idt64[vec].base_low    = h & 0xFFFF;
    idt64[vec].selector    = 0x08;
    idt64[vec].ist         = 0;
    idt64[vec].type_attr   = (uint8_t)(0x8E | ((dpl & 3) << 5));
    idt64[vec].base_middle = (h >> 16) & 0xFFFF;
    idt64[vec].base_high   = (h >> 32) & 0xFFFFFFFF;
    idt64[vec].reserved    = 0;
}

// Вершина стека ядра для входа из Ring 3: у каждой задачи свой (sched_yield переставляет при переключении)
void set_tss_rsp0(uint64_t rsp0) {
    if (rsp0) kernel_tss.rsp0 = rsp0;
}

void reset_tss_to_default(void) {
    kernel_tss.rsp0 = (uint64_t)kernel_interrupt_stack + sizeof(kernel_interrupt_stack);
}

__attribute__((naked)) int run_in_user_mode(void (*entry_point)(void), void* user_stack_top) {
    __asm__ volatile (
        "push %rbp\n\t"
        "push %rbx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"

        "movq %rdi, %r12\n\t"            // entry_point
        "movq %rsi, %r13\n\t"            // user_stack_top
        "movq %rsp, saved_kernel_rsp(%rip)\n\t"

        // rsp0 этой задачи = область прямо под сохранёнными регистрами (свой стек входа в ядро)
        "movq %rsp, %rdi\n\t"
        "subq $8, %rsp\n\t"              // выравнивание 16 байт перед call
        "call set_tss_rsp0\n\t"
        "addq $8, %rsp\n\t"

        "movq $1, g_user_mode_active\n\t"

        "pushq $0x23\n\t"                // User SS
        "pushq %r13\n\t"                 // User RSP
        "pushfq\n\t"
        "popq %rax\n\t"
        "orq $0x200, %rax\n\t"           // Включаем прерывания (IF = 1)
        "pushq %rax\n\t"                 // RFLAGS
        "pushq $0x1B\n\t"                // User CS
        "pushq %r12\n\t"                 // User RIP
        "iretq\n\t"
    );
}

void return_from_user_mode(int exit_code) {
    (void)exit_code;
}