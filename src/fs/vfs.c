#include "vfs.h"
#include "devfs.h"
#include "fs.h"
#include "../kernel/sched.h"
#include "../kernel/users.h"
#include "../kernel/tty.h"
extern const char* fs_caller_cwd(void);
#include "../memory/pmm.h"
#include "../memory/vmm.h"

extern void kputs(const char* str, uint32_t color);
extern int  strcmp(const char* s1, const char* s2);

static file_descriptor_t g_fd_pool[MAX_GLOBAL_FDS];
static vfs_node_t        g_fat32_nodes[MAX_VFS_NODES];
static int               g_fat32_node_count = 0;

// Обертка операций над файлами FAT32
static int64_t vfs_fat32_read(vfs_node_t* node, uint64_t offset, uint64_t size, uint8_t* buffer) {
    if (size == 0 || !buffer) return 0;
    if (offset > 0xFFFFFFFFULL) return 0;
    if (size > 0xFFFFFFFFULL) size = 0xFFFFFFFFULL;

    // С текущей позиции дескриптора; 0 — конец файла (раньше каждый read читал с начала)
    fs_lock();
    int rd = fs_read_at(node->name, (uint32_t)offset, buffer, (uint32_t)size);
    fs_unlock();
    return rd < 0 ? -1 : rd;
}

#define VFS_MAX_RMW (16u * 1024 * 1024)

// Запись со смещения 0 заменяет файл целиком (так рассчитаны текущие программы: открыть
// на запись и записать всё одним write). Запись дальше — дописывание/правка: файл
// читается, меняется нужный кусок и записывается заново (FAT-драйвер пока умеет только целиком).
static int64_t vfs_fat32_write(vfs_node_t* node, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    if (offset == 0) {
        fs_lock();
        int written = fs_write_file(node->name, buffer, (uint32_t)size);
        fs_unlock();
        return written ? (int64_t)size : -1;
    }

    fs_lock();
    uint32_t old_size = fs_file_size(node->name);
    uint64_t new_size = offset + size;
    if (new_size < old_size) new_size = old_size;
    if (new_size > VFS_MAX_RMW) { fs_unlock(); return -1; }

    uint32_t pages = (uint32_t)((new_size + 4095) / 4096);
    void* phys = pmm_alloc_pages(pages);
    if (!phys) { fs_unlock(); return -1; }
    uint8_t* tmp = (uint8_t*)PHYS_TO_VIRT(phys);

    for (uint64_t i = 0; i < new_size; i++) tmp[i] = 0;
    if (old_size) fs_read_at(node->name, 0, tmp, old_size);
    for (uint64_t i = 0; i < size; i++) tmp[offset + i] = buffer[i];
    int written = fs_write_file(node->name, tmp, (uint32_t)new_size);
    fs_unlock();

    for (uint32_t i = 0; i < pages; i++) pmm_free_page((uint8_t*)phys + (uint64_t)i * 4096);
    return written ? (int64_t)size : -1;
}

static vfs_ops_t g_fat32_ops = {
    .read  = vfs_fat32_read,
    .write = vfs_fat32_write,
    .open  = NULL,
    .close = NULL,
    .ioctl = NULL
};

void vfs_init(void) {
    for (int i = 0; i < MAX_GLOBAL_FDS; i++) {
        g_fd_pool[i].is_used = 0;
        g_fd_pool[i].ref_count = 0;
    }
    devfs_init();
}

static int kstrncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') return (uint8_t)s1[i] - (uint8_t)s2[i];
    }
    return 0;
}

static void kstrncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    while (src && src[i] && i + 1 < n) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

vfs_node_t* vfs_resolve_path(const char* path) {
    if (!path || path[0] == '\0') return NULL;

    const char* p = path;
    if (p[0] == '/') p++;

    // 1. Устройства /dev/
    if (kstrncmp(p, "dev/", 4) == 0) {
        return devfs_get_node(p + 4);
    }

    // 2. Для FAT32 проверяем абсолютный путь (с сохранением '/')
    fs_lock();
    int exists = fs_file_exists(path);
    if (!exists && p != path) {
        exists = fs_file_exists(p);
    }
    fs_unlock();

    if (!exists) return NULL;

    for (int i = 0; i < g_fat32_node_count; i++) {
        if (strcmp(g_fat32_nodes[i].name, path) == 0) {
            return &g_fat32_nodes[i];
        }
    }

    vfs_node_t* node = NULL;
    if (g_fat32_node_count < MAX_VFS_NODES) {
        node = &g_fat32_nodes[g_fat32_node_count++];
    } else {
        // Кэш полон: переиспользуем узел, который не держит ни один открытый дескриптор
        // (раньше после 64 разных файлов open() навсегда возвращал -1)
        for (int i = 0; i < MAX_VFS_NODES && !node; i++) {
            int used = 0;
            for (int f = 0; f < MAX_GLOBAL_FDS; f++) {
                if (g_fd_pool[f].is_used && g_fd_pool[f].node == &g_fat32_nodes[i]) { used = 1; break; }
            }
            if (!used) node = &g_fat32_nodes[i];
        }
    }
    if (!node) return NULL;
    kstrncpy(node->name, path, sizeof(node->name));
    node->type = VFS_FILE;
    node->ops = &g_fat32_ops;
    node->size = 0;
    return node;
}

file_descriptor_t* vfs_allocate_fd(vfs_node_t* node, uint32_t flags) {
    if (!node) return NULL;
    for (int i = 0; i < MAX_GLOBAL_FDS; i++) {
        if (!g_fd_pool[i].is_used) {
            g_fd_pool[i].is_used = 1;
            g_fd_pool[i].node = node;
            g_fd_pool[i].offset = 0;
            g_fd_pool[i].flags = flags;
            g_fd_pool[i].ref_count = 1;
            return &g_fd_pool[i];
        }
    }
    return NULL;
}

void vfs_release_fd(file_descriptor_t* fd) {
    if (!fd) return;
    if (fd->ref_count > 0) fd->ref_count--;
    if (fd->ref_count == 0) {
        if (fd->node && fd->node->ops && fd->node->ops->close) {
            fd->node->ops->close(fd->node);
        }
        fd->is_used = 0;
        fd->node = NULL;
    }
}

int vfs_sys_open(const char* path, uint32_t flags) {
    task_t* curr = sched_get_current_task();
    if (!curr) return -1;

    int user_fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (curr->fd_table[i] == NULL) { user_fd = i; break; }
    }
    if (user_fd < 0) return -1;

    vfs_node_t* node = vfs_resolve_path(path);

    // Если файла еще нет, но мы открываем его на запись — создаем через fs_touch
    if (!node && (flags & (O_WRONLY | O_RDWR | O_CREAT))) {
        // В виртуальном каталоге /dev файлы создавать нельзя
        const char* p = path;
        if (p[0] == '/') p++;
        if (kstrncmp(p, "dev/", 4) != 0) {
            // Создать файл можно только с правом записи в каталог
            if (!perm_check_parent(curr->uid, curr->gid, path, fs_caller_cwd(), PERM_W)) return -1;

            fs_lock();
            int created = fs_touch(path); // Передаем полный путь path!
            fs_unlock();

            if (created) {
                perm_on_create(curr->uid, curr->gid, path, fs_caller_cwd(), 0);
                node = vfs_resolve_path(path);
            }
        }
    } else if (node && node->type == VFS_FILE) {
        // Существующий файл: R для чтения, W для записи
        int want = 0;
        uint32_t acc = flags & 3;
        if (acc == O_RDONLY || acc == O_RDWR) want |= PERM_R;
        if (acc == O_WRONLY || acc == O_RDWR) want |= PERM_W;
        if (!perm_check(curr->uid, curr->gid, path, fs_caller_cwd(), want)) return -1;
    }

    if (!node) return -1;

    // Кадровый буфер (весь экран) — только root и процессу на переднем плане TTY
    if (node->type == VFS_CHARDEVICE && strcmp(node->name, "fb0") == 0 &&
        curr->uid != UID_ROOT && !tty_is_foreground(curr)) return -1;

    if (node->ops && node->ops->open) {
        if (node->ops->open(node, flags) != 0) return -1;
    }

    file_descriptor_t* desc = vfs_allocate_fd(node, flags);
    if (!desc) return -1;

    curr->fd_table[user_fd] = desc;
    return user_fd;
}

int64_t vfs_sys_read(int fd_num, void* buf, uint64_t count) {
    task_t* curr = sched_get_current_task();
    if (!curr || fd_num < 0 || fd_num >= MAX_FD || !curr->fd_table[fd_num]) return -1;

    file_descriptor_t* fd = curr->fd_table[fd_num];
    if (!fd->node || !fd->node->ops || !fd->node->ops->read) return -1;

    int64_t ret = fd->node->ops->read(fd->node, fd->offset, count, (uint8_t*)buf);
    if (ret > 0) fd->offset += ret;
    return ret;
}

int64_t vfs_sys_write(int fd_num, const void* buf, uint64_t count) {
    task_t* curr = sched_get_current_task();
    if (!curr || fd_num < 0 || fd_num >= MAX_FD || !curr->fd_table[fd_num]) return -1;

    file_descriptor_t* fd = curr->fd_table[fd_num];
    if (!fd->node || !fd->node->ops || !fd->node->ops->write) return -1;

    int64_t ret = fd->node->ops->write(fd->node, fd->offset, count, (const uint8_t*)buf);
    if (ret > 0) fd->offset += ret;
    return ret;
}

int64_t vfs_sys_lseek(int fd_num, int64_t off, int whence) {
    task_t* curr = sched_get_current_task();
    if (!curr || fd_num < 0 || fd_num >= MAX_FD || !curr->fd_table[fd_num]) return -1;

    file_descriptor_t* fd = curr->fd_table[fd_num];
    if (!fd->node) return -1;

    int64_t base;
    switch (whence) {
        case 0: base = 0; break;                       // SEEK_SET
        case 1: base = (int64_t)fd->offset; break;     // SEEK_CUR
        case 2: {                                      // SEEK_END
            // Размер в узле VFS не хранится (он создаётся на лету), поэтому
            // спрашиваем файловую систему
            if (fd->node->type != VFS_FILE) return -1;
            fs_lock();
            uint32_t sz = fs_file_size(fd->node->name);
            fs_unlock();
            base = (int64_t)sz;
            break;
        }
        default: return -1;
    }
    int64_t pos = base + off;
    if (pos < 0) return -1;
    fd->offset = (uint64_t)pos;
    return pos;
}

int vfs_sys_close(int fd_num) {
    task_t* curr = sched_get_current_task();
    if (!curr || fd_num < 0 || fd_num >= MAX_FD || !curr->fd_table[fd_num]) return -1;

    vfs_release_fd(curr->fd_table[fd_num]);
    curr->fd_table[fd_num] = NULL;
    return 0;
}

int vfs_sys_ioctl(int fd_num, uint64_t req, uint64_t arg) {
    task_t* curr = sched_get_current_task();
    if (!curr || fd_num < 0 || fd_num >= MAX_FD || !curr->fd_table[fd_num]) return -1;

    file_descriptor_t* fd = curr->fd_table[fd_num];
    if (!fd->node || !fd->node->ops || !fd->node->ops->ioctl) return -1;

    return fd->node->ops->ioctl(fd->node, req, arg);
}