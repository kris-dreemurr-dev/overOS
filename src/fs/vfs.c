#include "vfs.h"
#include "devfs.h"
#include "fs.h"
#include "../kernel/sched.h"

extern void kputs(const char* str, uint32_t color);

static file_descriptor_t g_fd_pool[MAX_GLOBAL_FDS];
static vfs_node_t        g_fat32_nodes[MAX_VFS_NODES];
static int               g_fat32_node_count = 0;

// Обертка операций над файлами FAT32
static int64_t vfs_fat32_read(vfs_node_t* node, uint64_t offset, uint64_t size, uint8_t* buffer) {
    (void)offset;
    if (size == 0 || !buffer) return 0;

    fs_lock();
    // Аппаратный драйвер EHCI читает секторами по 512 байт.
    // Если буфер процесса меньше 512 байт, читаем через промежуточный буфер ядра,
    // чтобы не затереть стек/память Ring 3 за пределами size.
    if (size < 512) {
        static uint8_t sec_tmp[512] __attribute__((aligned(64)));
        int rd = fs_read_file(node->name, sec_tmp, 512);
        fs_unlock();
        if (rd <= 0) return rd;

        uint32_t to_copy = (uint32_t)size;
        if ((uint32_t)rd < to_copy) to_copy = (uint32_t)rd;
        for (uint32_t i = 0; i < to_copy; i++) {
            buffer[i] = sec_tmp[i];
        }
        return (int64_t)to_copy;
    }

    int read_bytes = fs_read_file(node->name, buffer, (uint32_t)size);
    fs_unlock();
    return read_bytes;
}

static int64_t vfs_fat32_write(vfs_node_t* node, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)offset;
    fs_lock();
    int written = fs_write_file(node->name, buffer, (uint32_t)size);
    fs_unlock();
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

    // 1. Маршрутизация в devfs
    if (kstrncmp(p, "dev/", 4) == 0) {
        return devfs_get_node(p + 4);
    }

    // 2. Маршрутизация в смонтированную FAT32
    fs_lock();
    int exists = fs_file_exists(p);
    fs_unlock();

    if (!exists) return NULL;

    // Ищем узел в пуле или регистрируем новый
    for (int i = 0; i < g_fat32_node_count; i++) {
        const char* n1 = g_fat32_nodes[i].name;
        const char* n2 = p;
        int match = 1;
        while (*n1 && *n2) {
            if (*n1++ != *n2++) { match = 0; break; }
        }
        if (match && *n1 == *n2) return &g_fat32_nodes[i];
    }

    if (g_fat32_node_count < MAX_VFS_NODES) {
        vfs_node_t* node = &g_fat32_nodes[g_fat32_node_count++];
        kstrncpy(node->name, p, sizeof(node->name));
        node->type = VFS_FILE;
        node->ops = &g_fat32_ops;
        node->size = 0;
        return node;
    }

    return NULL;
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
        const char* p = path;
        if (p[0] == '/') p++;

        // В виртуальном каталоге /dev файлы создавать нельзя
        if (kstrncmp(p, "dev/", 4) != 0) {
            fs_lock();
            int created = fs_touch(p);
            fs_unlock();

            if (created) {
                node = vfs_resolve_path(path);
            }
        }
    }

    if (!node) return -1;

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