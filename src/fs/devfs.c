#include "devfs.h"
#include "../kernel/tty.h"
#include "../kernel/sched.h"
#include "../drivers/display.h"

extern void     kputc(char c, uint32_t color);
extern void     flush_buffer(void);
extern uint16_t screen_width;
extern uint16_t screen_height;
extern uint16_t screen_pitch;
extern volatile uint32_t* lfb;

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

    uint64_t read_bytes = 0;
    while (read_bytes < size) {
        uint8_t sc = 0;
        while (!tty_kbd_pop(tty_id, &sc)) {
            sched_yield(); // Ждем нажатия клавиши в своей очереди TTY
        }

        if (sc & 0x80) continue; // Игнорируем отпускание клавиши
        char c = (sc < 128) ? scancode_ascii[sc] : 0;
        if (!c) continue;

        buffer[read_bytes++] = (uint8_t)c;
        if (c == '\n') break; // Возвращаем управление на переносе строки (канонический ввод)
    }
    return (int64_t)read_bytes;
}

static int64_t dev_tty_write(vfs_node_t* node, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)node; (void)offset;
    task_t* caller = sched_get_current_task();
    int visible = (!caller || caller->tty_id < 0 || caller->tty_id == tty_get_active_id());

    for (uint64_t i = 0; i < size; i++) {
        kputc((char)buffer[i], 0x00FFFFFF);
    }
    if (visible) flush_buffer();
    return (int64_t)size;
}

static vfs_ops_t g_tty_ops = {
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .open  = NULL,
    .close = NULL,
    .ioctl = NULL
};

// ----------------- /dev/fb0 -----------------
// Функция получения текущего процесса и активного TTY

static int64_t dev_fb_write(vfs_node_t* node, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)offset;
    if (!lfb || !buffer) return -1;

    // ЗАЩИТА: Если процесс запущен на фоновом TTY — НЕ трогаем физический экран!
    task_t* curr = sched_get_current_task();
    if (curr && curr->tty_id >= 0 && curr->tty_id != tty_get_active_id()) {
        return (int64_t)size;
    }

    uint64_t max_bytes = (uint64_t)screen_width * screen_height * 4;
    uint64_t to_copy = size > max_bytes ? max_bytes : size;

    uint64_t* dst = (uint64_t*)lfb;
    const uint64_t* src = (const uint64_t*)buffer;
    uint64_t qwords = to_copy / 8;

    // Быстрый 64-битный перенос по шине
    __asm__ volatile (
        "rep movsq"
        : "+D"(dst), "+S"(src), "+c"(qwords)
        :
        : "memory"
    );

    // Докопируем остаток, если буфер не кратен 8 байтам
    uint32_t rem = to_copy % 8;
    if (rem) {
        uint8_t* d8 = (uint8_t*)dst;
        const uint8_t* s8 = (const uint8_t*)src;
        for (uint32_t i = 0; i < rem; i++) {
            d8[i] = s8[i];
        }
    }

    // Выталкиваем накопленные пиксели из кэш-очередей процессора прямо в PCIe
    __asm__ volatile ("sfence" ::: "memory");

    return (int64_t)to_copy;
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