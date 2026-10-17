#include "devfs.h"
#include "../kernel/tty.h"
#include "../kernel/sched.h"
#include "../drivers/display.h"
#include "../memory/pmm.h"

// 1 = писать в LFB только изменившиеся участки кадра (сравнение с копией экрана в RAM).
// Если картинка вдруг "залипает" (ghost-курсор и т.п.) — поставь 0, останется простая полная запись.
#define FB_DIFF_WRITE 1

extern void     kputc(char c, uint32_t color);
extern void     flush_buffer(void);
extern void     flush_dirty(void);
extern uint16_t screen_width;
extern uint16_t screen_height;
extern uint16_t screen_pitch;
extern volatile uint32_t* lfb;
extern uint32_t timer_ticks;
extern volatile uint32_t g_lfb_gen;
extern volatile uint32_t g_fb_last_write_tick;
extern volatile int      g_fb_write_tty;

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
};

// ----------------- /dev/tty -----------------
static int64_t dev_tty_read(vfs_node_t* node, uint64_t offset, uint64_t size, uint8_t* buffer) {
    (void)node; (void)offset;
    if (size == 0) return 0;

    task_t* caller = sched_get_current_task();
    int tty_id = (caller && caller->tty_id >= 0) ? caller->tty_id : tty_get_active_id();

    // Клавиатуру читает только процесс переднего плана этого TTY: иначе фоновая
    // программа прошлого сеанса перехватила бы пароль следующего пользователя
    if (!tty_is_foreground(caller)) return -1;

    // Канонический ввод: строку собирает драйвер TTY (эхо, Backspace, раскладка),
    // а не каждая программа сама по сканкодам
    char line[256];
    int n = tty_line_read(tty_id, line, sizeof(line));
    uint64_t out = 0;
    for (int i = 0; i < n && out < size; i++) buffer[out++] = (uint8_t)line[i];
    if (out < size) buffer[out++] = '\n';
    return (int64_t)out;
}

// Управление эхо (ввод пароля): ioctl(fd, TTY_IOCTL_ECHO, 0/1), как termios ECHO
#define TTY_IOCTL_ECHO 0x5401
// Запоминать ли строки в истории команд: включает её только оболочка. Ядру
// не отличить команду от имени пользователя, которое спрашивает login.prg, --
// решает тот, кто читает строку.
#define TTY_IOCTL_HIST 0x5402

static int dev_tty_ioctl(vfs_node_t* node, uint64_t request, uint64_t arg) {
    (void)node;
    task_t* caller = sched_get_current_task();
    int tty_id = (caller && caller->tty_id >= 0) ? caller->tty_id : tty_get_active_id();
    if (request == TTY_IOCTL_ECHO) { tty_set_echo(tty_id, (int)arg); return 0; }
    if (request == TTY_IOCTL_HIST) { tty_set_history(tty_id, (int)arg); return 0; }
    return -1;
}

static int64_t dev_tty_write(vfs_node_t* node, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)node; (void)offset;
    task_t* caller = sched_get_current_task();
    int visible = (!caller || caller->tty_id < 0 || caller->tty_id == tty_get_active_id());

    for (uint64_t i = 0; i < size; i++) {
        kputc((char)buffer[i], 0x00FFFFFF);
    }
    if (visible) flush_dirty();
    return (int64_t)size;
}

static vfs_ops_t g_tty_ops = {
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .open  = NULL,
    .close = NULL,
    .ioctl = dev_tty_ioctl
};

// ----------------- /dev/fb0 -----------------
// Функция получения текущего процесса и активного TTY

// Копия того, что сейчас лежит в LFB (в обычной кэшируемой RAM). Читать сам LFB нельзя —
// это write-combining память, чтение из неё очень медленное.
static uint32_t* g_fb_shadow = 0;
static uint64_t  g_fb_shadow_bytes = 0;
static int       g_fb_shadow_ok = 0;      // копия совпадает с LFB
static uint32_t  g_fb_shadow_gen = 0;     // значение g_lfb_gen на момент нашей последней записи
// Строки, которые писала частичная запись мимо теневой копии: следующий полный
// кадр переписывает их целиком, не сравнивая. Раньше для этого тень на том же
// участке портили побайтово (~байт), и на окне во весь экран это были
// мегабайты байтового цикла на каждый кадр.
#define FB_MAX_ROWS 4096
static uint8_t   g_fb_row_dirty[FB_MAX_ROWS];

static inline void fb_copy_q(void* d, const void* s, uint64_t qwords) {
    __asm__ volatile ("rep movsq" : "+D"(d), "+S"(s), "+c"(qwords) : : "memory");
}

static int64_t dev_fb_write(vfs_node_t* node, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)node;
    // Смещение теперь уважается: композитор выводит одну полосу строк (едет
    // курсор — меняются двадцать строк, а не весь экран), сделав перед записью
    // lseek. Полный кадр по-прежнему идёт с нулевого смещения.
    if (!lfb || !buffer) return -1;

    // ЗАЩИТА: Если процесс запущен на фоновом TTY — НЕ трогаем физический экран!
    task_t* curr = sched_get_current_task();
    if (curr && curr->tty_id >= 0 && curr->tty_id != tty_get_active_id()) {
        return (int64_t)size;
    }

    // Говорим ядру: экран сейчас рисует программа, курсор консоли поверх не рисовать
    g_fb_write_tty = (curr && curr->tty_id >= 0) ? curr->tty_id : tty_get_active_id();
    g_fb_last_write_tick = timer_ticks;

    uint64_t row   = (uint64_t)screen_width * 4;
    uint64_t total = row * screen_height;
    uint64_t base  = offset;
    if (base >= total) return 0;
    uint64_t len   = (size < total - base) ? size : (total - base);
    uint8_t* lfb8  = (uint8_t*)lfb;

#if FB_DIFF_WRITE
    if (!g_fb_shadow) {
        uint64_t pages = (total + 4095) / 4096;
        g_fb_shadow = (uint32_t*)pmm_alloc_pages((uint32_t)pages);
        g_fb_shadow_bytes = g_fb_shadow ? total : 0;
        g_fb_shadow_ok = 0;
    }

    if (g_fb_shadow && g_fb_shadow_bytes == total && base == 0 && len == total && (row & 7) == 0) {
        uint64_t q = row / 8;
        uint8_t* sh8 = (uint8_t*)g_fb_shadow;

        if (g_fb_shadow_ok && g_fb_shadow_gen == g_lfb_gen) {
            // Копия актуальна: пишем в LFB только участок строки, который реально изменился
            for (uint64_t y = 0; y < screen_height; y++) {
                const uint64_t* s  = (const uint64_t*)(buffer + y * row);
                uint64_t*       sh = (uint64_t*)(sh8 + y * row);
                if (y < FB_MAX_ROWS && g_fb_row_dirty[y]) {   // тень тут не знает, что в LFB
                    g_fb_row_dirty[y] = 0;
                    fb_copy_q(lfb8 + y * screen_pitch, s, q);
                    fb_copy_q(sh, s, q);
                    continue;
                }
                uint64_t a = 0;
                while (a < q && s[a] == sh[a]) a++;
                if (a == q) continue;                       // строка не изменилась
                uint64_t b = q;
                while (b > a && s[b - 1] == sh[b - 1]) b--;
                fb_copy_q(lfb8 + y * screen_pitch + a * 8, s + a, b - a);
                fb_copy_q(sh + a, s + a, b - a);
            }
        } else {
            // Первый кадр или экран трогал кто-то ещё (консоль, смена TTY): пишем всё и запоминаем
            for (uint64_t y = 0; y < screen_height; y++) {
                fb_copy_q(lfb8 + y * screen_pitch, buffer + y * row, q);
                fb_copy_q(sh8 + y * row, buffer + y * row, q);
            }
            g_fb_shadow_ok = 1;
            for (uint64_t y = 0; y < screen_height && y < FB_MAX_ROWS; y++) g_fb_row_dirty[y] = 0;
        }
        __asm__ volatile ("sfence" ::: "memory");
        g_fb_shadow_gen = g_lfb_gen;
        return (int64_t)len;
    }
#endif

    // Простой путь (частичная запись, нет памяти под копию, FB_DIFF_WRITE=0): строки с учётом pitch.
    // Теневую копию при частичной записи не выбрасываем, а обновляем на том же
    // участке: иначе следующий полный кадр пришлось бы гнать в LFB целиком.
    // Теневая копия экрана -- это кэш "что уже лежит в LFB", по которому полный
    // кадр пишет только изменившееся. Частичная запись (полоса строк под
    // курсором) в этот кэш попадать НЕ должна: пробовали записывать туда те же
    // байты -- и обои пропадали, полный кадр считал эти строки совпавшими и не
    // трогал их. Поэтому такие строки помечаются в g_fb_row_dirty: следующий
    // полный кадр перепишет ровно их, а все остальные по-прежнему сравнит.
    int shadow_live = (g_fb_shadow && g_fb_shadow_bytes == total &&
                       g_fb_shadow_ok && g_fb_shadow_gen == g_lfb_gen && screen_height <= FB_MAX_ROWS);
    if (!shadow_live) g_fb_shadow_ok = 0;
    uint64_t left = len, done = 0;
    while (left) {
        uint64_t y = (base + done) / row;
        uint64_t x = (base + done) % row;
        uint64_t n = (row - x < left) ? (row - x) : left;
        void* d = lfb8 + y * screen_pitch + x;
        const void* s = buffer + done;
        uint64_t cnt = n;
        if ((((uint64_t)d | (uint64_t)s | cnt) & 7) == 0) {
            cnt >>= 3;
            __asm__ volatile ("rep movsq" : "+D"(d), "+S"(s), "+c"(cnt) : : "memory");
        } else {
            __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(cnt) : : "memory");
        }
        if (shadow_live) g_fb_row_dirty[y] = 1;     // «тут не знаю, перепиши»
        done += n;
        left -= n;
    }
    __asm__ volatile ("sfence" ::: "memory");
    if (shadow_live) g_fb_shadow_gen = g_lfb_gen;
    return (int64_t)done;
}

static int dev_fb_ioctl(vfs_node_t* node, uint64_t request, uint64_t arg) {
    (void)node;
    if (request == FBIOGET_VSCREENINFO && arg != 0) {
        fb_var_info_t* info = (fb_var_info_t*)arg;
        info->width  = screen_width;
        info->height = screen_height;
        info->pitch  = screen_pitch;
        info->bpp    = 32;
        info->paddr  = (uint64_t)(uintptr_t)lfb;
        return 0;
    }
    // 0x4601: Установка аппаратной подсветки матрицы
    if (request == 0x4601) {
        intel_set_backlight((int)arg, 1);
        return 0;
    }
    return -1;
}

static vfs_ops_t g_fb_ops = {
    .read  = NULL,
    .write = dev_fb_write,
    .open  = NULL,
    .close = NULL,
    .ioctl = dev_fb_ioctl
};

// ----------------- /dev/null и /dev/zero -----------------
static int64_t dev_null_read(vfs_node_t* n, uint64_t o, uint64_t s, uint8_t* b)  { (void)n;(void)o;(void)s;(void)b; return 0; }
static int64_t dev_null_write(vfs_node_t* n, uint64_t o, uint64_t s, const uint8_t* b) { (void)n;(void)o;(void)b; return (int64_t)s; }

static int64_t dev_zero_read(vfs_node_t* n, uint64_t o, uint64_t s, uint8_t* b) {
    (void)n; (void)o;
    for (uint64_t i = 0; i < s; i++) b[i] = 0;
    return (int64_t)s;
}

static vfs_ops_t g_null_ops = { .read = dev_null_read, .write = dev_null_write };
static vfs_ops_t g_zero_ops = { .read = dev_zero_read, .write = dev_null_write };

// Каталог устройств
static vfs_node_t g_dev_nodes[6];

void devfs_init(void) {
    // /dev/tty
    g_dev_nodes[0] = (vfs_node_t){ .name = "tty", .type = VFS_CHARDEVICE, .ops = &g_tty_ops };
    // /dev/fb0
    g_dev_nodes[1] = (vfs_node_t){ .name = "fb0", .type = VFS_CHARDEVICE, .ops = &g_fb_ops };
    // /dev/null
    g_dev_nodes[2] = (vfs_node_t){ .name = "null", .type = VFS_CHARDEVICE, .ops = &g_null_ops };
    // /dev/zero
    g_dev_nodes[3] = (vfs_node_t){ .name = "zero", .type = VFS_CHARDEVICE, .ops = &g_zero_ops };
    // /dev/stdin, stdout, stderr (псевдонимы tty)
    g_dev_nodes[4] = (vfs_node_t){ .name = "stdin",  .type = VFS_CHARDEVICE, .ops = &g_tty_ops };
    g_dev_nodes[5] = (vfs_node_t){ .name = "stdout", .type = VFS_CHARDEVICE, .ops = &g_tty_ops };
}

static int kstrcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

vfs_node_t* devfs_get_node(const char* name) {
    for (int i = 0; i < 6; i++) {
        if (kstrcmp(g_dev_nodes[i].name, name) == 0) {
            return &g_dev_nodes[i];
        }
    }
    return NULL;
}