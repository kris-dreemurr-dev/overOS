// =============================================================================
// loader_kernel.c — запуск подсистем overOS и отчёт о нём.
//
// Раньше здесь был театр: функция печатала список строк с жёстко вписанным
// [ OK ] для того, что к этому моменту уже подняли в kernel_main, и ничего не
// проверяла. Теперь каждая строка — настоящая проверка настоящей подсистемы,
// с настоящими цифрами (сколько памяти, какой процессор, какая флешка, сколько
// устройств на шине), а результат остаётся в boot_report_t: по нему потом видно,
// чего в системе нет, не гадая по экрану.
//
// Оформление — как у systemd на Arch: сначала бежит строка "Starting ...",
// затем её сменяет "[  OK  ] Started ...". Текст лога английский намеренно:
// это служебный вывод ядра, его читают и те, кто пришёл из Linux, и он должен
// выглядеть одинаково на любой машине (в консоли CP866, а не UTF-8).
//
// Порядок менять осторожно: то, что идёт в kernel_main до вызова (память,
// прерывания, таймер, пользовательский режим, процессор), здесь только
// ПРОВЕРЯЕТСЯ; всё остальное — шина, экран, USB, файловая система, звук, сеть,
// ввод и модули ядра — именно здесь и поднимается.
// =============================================================================

#include "loader_kernel.h"
#include "smp.h"
#include "config.h"
#include "../drivers/display.h"
#include "../drivers/pci.h"
#include "../drivers/ethernet.h"
#include "../drivers/hda.h"
#include "../drivers/mouse.h"
#include "../drivers/usb/usb.h"
#include "../drivers/usb/xhci.h"
#include "../drivers/blk.h"
#include "../fs/mount.h"
#include "../fs/fs.h"
#include "../fs/vfs.h"
#include "../fs/fat32.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "cpu.h"
#include "tty.h"
#include "sched.h"
#include "prog_loader.h"
#include "contheme.h"
#include <stdint.h>

// Внешние функции инициализации подсистем
extern void init_crash_guard_idt(void);
extern void init_user_mode(void);
extern void pic_remap(void);
extern void pit_init(uint32_t freq);
extern void tsc_calibrate(void);
extern void render_boot_logo(void);

// Вывод, задержки и мелочи из kernel.c
extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern void sleep_ms(uint32_t ms);
extern void itoa(int n, char* str);
extern uint32_t timer_ticks;
extern uint16_t screen_width, screen_height;   // объявлены так в kernel.c
extern int g_lfb_wc_entries;
extern uint8_t kernel_temp_buf[];

static boot_report_t g_boot;

const boot_report_t* boot_get_report(void) { return &g_boot; }

// ---------------------------------------------------------------- вывод строк
// Бегущая строка: "         Starting foo..." — то, что systemd показывает,
// пока работа идёт. Своей "перерисовки строки на месте" у консоли нет (нет и
// ANSI-курсора), поэтому результат печатается следующей строкой, как в журнале
// systemd при выводе без TTY-анимации.
static void log_start(const char* msg) {
    kputs("         ", CON_DIM);
    kputs(msg, CON_DIM);
    kputs("\n", CON_FG);
    flush_buffer();
    sleep_ms(20);
}

// Итог: "[  OK  ] ..." или "[FAILED] ...".
static void log_done(int ok, const char* msg, const char* detail) {
    kputs("[", CON_DIM);
    kputs(ok ? "  OK  " : "FAILED", ok ? CON_GREEN : CON_RED);
    kputs("] ", CON_DIM);
    kputs(msg, ok ? CON_FG : CON_RED);
    if (detail && detail[0]) {
        kputs(" ", CON_FG);
        kputs(detail, ok ? CON_WHITE : CON_DIM);
    }
    kputs("\n", CON_FG);
    flush_buffer();
    sleep_ms(ok ? 25 : 150);              // после сбоя даём прочитать
}

// Сборка строки вида "123 MiB": своего snprintf в ядре нет
typedef struct { char buf[96]; int n; } sb_t;
static void sb_clear(sb_t* s) { s->n = 0; s->buf[0] = '\0'; }
static void sb_str(sb_t* s, const char* t) {
    while (*t && s->n < (int)sizeof(s->buf) - 1) s->buf[s->n++] = *t++;
    s->buf[s->n] = '\0';
}
static void sb_num(sb_t* s, int v) {
    char num[16];
    itoa(v, num);
    sb_str(s, num);
}
// "1 core" / "4 cores"
static void sb_count(sb_t* s, int n, const char* word) {
    sb_num(s, n);
    sb_str(s, " ");
    sb_str(s, word);
    if (n != 1) sb_str(s, "s");
}

// ---------------------------------------------------------------- модули ядра
// /etc/modules — по одному имени в строке, как в Linux. Пустые строки и
// начинающиеся с # пропускаем. Имя без пути ищем в /utils.
static int load_kernel_modules(sb_t* out) {
    char* txt = (char*)kernel_temp_buf;
    int len = fs_read_file("/etc/modules", txt, 4000);
    if (len <= 0) { sb_str(out, "(/etc/modules is empty)"); return 1; }
    txt[len] = '\0';

    int loaded = 0, failed = 0;
    int i = 0;
    while (i < len) {
        while (i < len && (txt[i] == '\n' || txt[i] == '\r' || txt[i] == ' ' || txt[i] == '\t')) i++;
        if (i >= len) break;
        if (txt[i] == '#') { while (i < len && txt[i] != '\n') i++; continue; }

        char name[64];
        int n = 0;
        while (i < len && txt[i] != '\n' && txt[i] != '\r' && n < (int)sizeof(name) - 1) name[n++] = txt[i++];
        while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t')) n--;
        name[n] = '\0';
        if (!n) continue;

        char path[80];
        int p = 0;
        if (name[0] != '/') { const char* d = "/utils/"; while (*d) path[p++] = *d++; }
        for (int k = 0; name[k] && p < (int)sizeof(path) - 1; k++) path[p++] = name[k];
        path[p] = '\0';

        if (prog_load_module(path, 0) == 0) loaded++;
        else failed++;
    }

    sb_str(out, "("); sb_count(out, loaded, "module"); sb_str(out, " loaded");
    if (failed) { sb_str(out, ", "); sb_num(out, failed); sb_str(out, " failed"); }
    sb_str(out, ")");
    return failed == 0;
}

// ---------------------------------------------------------------- сам запуск
void kernel_system_bootstrap(void) {
    sb_t d;

    kputs("\nWelcome to " OS_NAME " " OS_VERSION " (" OS_ARCH ")!\n\n", CON_CYAN);
    flush_buffer();

    // --- 1. Память. Цифры настоящие: карту дал BIOS, их же покажет free -----
    uint64_t total_mb = (pmm_get_total_blocks() * 4096ULL) / (1024 * 1024);
    uint64_t used_mb  = (pmm_get_used_blocks()  * 4096ULL) / (1024 * 1024);
    g_boot.ram_mb = (uint32_t)total_mb;
    sb_clear(&d);
    sb_num(&d, (int)total_mb); sb_str(&d, " MiB total, "); sb_num(&d, (int)used_mb); sb_str(&d, " MiB in use.");
    g_boot.memory = (total_mb > 0);
    log_done(g_boot.memory, "Detected system memory:", d.buf);

    // --- 2. Процессор -------------------------------------------------------
    const cpu_info_t* ci = cpu_get();
    sb_clear(&d);
    sb_count(&d, ci->cores, "core");
    sb_str(&d, ", "); sb_count(&d, ci->threads, "thread");
    int mhz = ci->cur_mhz ? ci->cur_mhz : ci->base_mhz;
    if (mhz) { sb_str(&d, ", "); sb_num(&d, mhz); sb_str(&d, " MHz"); }
    if (ci->turbo_on) sb_str(&d, ", turbo");
    sb_str(&d, ".");
    g_boot.cpu = (ci->threads > 0);
    log_done(g_boot.cpu, "Detected CPU:", d.buf);

    // --- 3. Прерывания и таймер: проверяем, что IRQ0 реально тикает ---------
    uint32_t t0 = timer_ticks;
    sleep_ms(5);
    g_boot.timer = (timer_ticks != t0);
    log_done(g_boot.timer,
             g_boot.timer ? "Reached target Interrupts and Timer."
                          : "Failed to reach target Interrupts and Timer.",
             g_boot.timer ? "(IDT, PIC, PIT 1000 Hz)" : "(timer is not ticking)");

    // --- 3.5. Рабочие ядра: остальные ядра процессора для вычислений программ.
    // Ядро ОС остаётся на основном; поднятые ядра спят в hlt, пока программа
    // не даст им работу (smp.c). Если не вышло — система работает как раньше.
    log_start("Starting Worker Cores...");
    int aps = smp_init(ci->threads);
    sb_clear(&d);
    if (aps > 0) {
        sb_num(&d, aps); sb_str(&d, aps == 1 ? " core online" : " cores online");
        sb_str(&d, ", "); sb_num(&d, aps + 1); sb_str(&d, " threads in total.");
    } else {
        sb_str(&d, "("); sb_str(&d, smp_status_text()); sb_str(&d, ")");
    }
    log_done(1, aps > 0 ? "Started Worker Cores:" : "Running on a single core", d.buf);

    // --- 4. Шина PCI --------------------------------------------------------
    log_start("Starting PCI bus scan...");
    int pci_n = pci_count_devices();
    g_boot.pci_devices = pci_n;
    sb_clear(&d);
    sb_count(&d, pci_n, "device");
    sb_str(&d, ".");
    g_boot.pci = (pci_n > 0);
    log_done(g_boot.pci, g_boot.pci ? "Finished PCI bus scan:" : "Failed PCI bus scan:", d.buf);

    // --- 5. Экран -----------------------------------------------------------
    log_start("Starting Display Service (VBE LFB)...");
    intel_set_backlight(100, 1);
    sb_clear(&d);
    sb_num(&d, (int)screen_width); sb_str(&d, "x"); sb_num(&d, (int)screen_height);
    // Без WC каждая запись в видеопамять идёт на шину отдельно: рабочий стол
    // будет тормозить, и по этой строке это видно сразу
    sb_str(&d, g_lfb_wc_entries > 0 ? "x32, write-combining, backlight 100%." : "x32, uncached LFB, backlight 100%.");
    g_boot.display = (screen_width > 0 && screen_height > 0);
    log_done(g_boot.display, g_boot.display ? "Started Display Service:" : "Failed to start Display Service.",
             g_boot.display ? d.buf : "");

    // --- 5.5. Виртуальная файловая система и устройства /dev -----------------
    // Строго до всего, что открывает файлы: /dev/tty нужен уже первой программе.
    vfs_init();

    // --- 6. USB и накопитель ------------------------------------------------
    // Драйвер выбирается сам: есть xHCI -- поднимаем его (на новых машинах
    // EHCI в чипсете уже нет), и в любом случае остаётся EHCI для старых.
    log_start("Starting USB Host Controllers...");
    int drv = usb_bootstrap();
    sb_clear(&d);
    if (drv == USB_DRV_XHCI) {
        sb_str(&d, "xHCI, "); sb_count(&d, xhci_port_count(), "port");
        sb_str(&d, ", "); sb_num(&d, xhci_ports_connected()); sb_str(&d, " connected");
    } else if (drv == USB_DRV_EHCI) {
        sb_str(&d, "EHCI");
    } else {
        sb_str(&d, "no working controller");
    }
    // Накопители берём из реестра дисков: ему всё равно, каким контроллером
    // они воткнуты. Флешек может быть несколько -- корень выберет монтирование.
    int disks = 0;
    uint32_t mb = 0;
    for (int i = 0; i < BLK_MAX; i++) if (blk_alive(i)) { if (!disks) mb = blk_size_mb(i); disks++; }
    g_boot.usb = (disks > 0);
    if (disks == 1) {
        sb_str(&d, ", storage ");
        if (mb >= 1024) { sb_num(&d, (int)(mb / 1024)); sb_str(&d, " GiB"); }
        else            { sb_num(&d, (int)mb);          sb_str(&d, " MiB"); }
    } else if (disks > 1) {
        sb_str(&d, ", "); sb_count(&d, disks, "storage device");
    } else {
        sb_str(&d, ", no storage");
    }
    sb_str(&d, ".");
    log_done(g_boot.usb, g_boot.usb ? "Started USB Host Controllers:" : "Failed to start USB Host Controllers:", d.buf);

    // --- 7. Файловая система ------------------------------------------------
    log_start("Mounting Root File System...");
    g_boot.fs = fat32_is_mounted();
    if (!g_boot.fs) g_boot.fs = (fs_mount(0) != 0) && fat32_is_mounted();
    sb_clear(&d);
    if (g_boot.fs) {
        // Именно размер тома, а не занятое место: занятое считается обходом
        // всей таблицы FAT, то есть сотнями чтений с флешки -- на загрузке это
        // несколько секунд впустую.
        sb_str(&d, "FAT32, "); sb_num(&d, (int)fat32_total_mb()); sb_str(&d, " MiB.");
    } else {
        sb_str(&d, "(root volume not found)");
        g_boot.failed = 1;
    }
    log_done(g_boot.fs, g_boot.fs ? "Mounted Root File System:" : "Failed to mount Root File System.", d.buf);
    if (!g_boot.fs) {
        // Самая частая причина на ноутбуках: в BIOS переключён режим USB.
        kputs("         Hint: set \"XHCI Mode\" to Smart Auto/Auto in the BIOS,\n", CON_YELLOW);
        kputs("         or report the [xHCI] lines above.\n", CON_YELLOW);
        flush_buffer();
    }

    // --- 8. Ввод ------------------------------------------------------------
    log_start("Starting PS/2 Keyboard and Mouse...");
    // Контроллера PS/2 может и не быть: тогда ввод только с USB (drivers/usb/hid.c)
    extern int g_ps2_present;
    init_ps2_mouse();
    int ps2 = g_ps2_present;
    g_boot.input = 1;
    log_done(g_boot.input, "Started PS/2 Keyboard and Mouse:", ps2 ? "(ports 0x60/0x64)" : "(no PS/2 controller, USB input only)");

    // --- 9. Звук ------------------------------------------------------------
    log_start("Starting HD Audio Driver...");
    g_boot.audio = (hda_init() == 0);
    sb_clear(&d);
    if (g_boot.audio) {
        const char* nm = pci_get_audio_controller_name();
        sb_str(&d, nm && nm[0] ? nm : "Intel HDA");
        sb_str(&d, ".");
    } else {
        sb_str(&d, "(no controller found)");
    }
    log_done(g_boot.audio, g_boot.audio ? "Started HD Audio Driver:" : "Failed to start HD Audio Driver.", d.buf);

    // --- 10. Сеть -----------------------------------------------------------
    log_start("Starting Network Interface...");
    g_boot.net = (net_bootstrap() == 0);
    log_done(g_boot.net, g_boot.net ? "Started Network Interface." : "Failed to start Network Interface.",
             g_boot.net ? "" : "(no card found)");

    // --- 11. Модули ядра ----------------------------------------------------
    // Теперь загрузчик действительно что-то грузит: .sys-модули из /etc/modules,
    // как /etc/modules-load.d в systemd. Раньше модули можно было поднять только
    // руками из оболочки.
    log_start("Starting Load Kernel Modules...");
    sb_clear(&d);
    int mod_ok = g_boot.fs ? load_kernel_modules(&d) : (sb_str(&d, "(no file system)"), 0);
    g_boot.modules = mod_ok;
    log_done(mod_ok, mod_ok ? "Finished Load Kernel Modules." : "Failed Load Kernel Modules.", d.buf);

    // --- Итог ---------------------------------------------------------------
    if (g_boot.failed || !g_boot.fs) {
        log_done(0, "Failed to reach target Basic System.", "(see the lines above)");
        kputs("\n", CON_FG);
    } else {
        log_done(1, "Reached target Basic System.", "");
        log_done(1, "Reached target Graphical Interface.", "");
        kputs("\n", CON_FG);
    }
    flush_buffer();
    sleep_ms(300);
}
