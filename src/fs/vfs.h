#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define MAX_FD          32
#define MAX_VFS_NODES   64
#define MAX_GLOBAL_FDS  128

// Типы узлов
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04

// Флаги sys_open
#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_CREAT         0x0040

struct vfs_node;

typedef struct vfs_ops {
    int64_t (*read)(struct vfs_node* node, uint64_t offset, uint64_t size, uint8_t* buffer);
    int64_t (*write)(struct vfs_node* node, uint64_t offset, uint64_t size, const uint8_t* buffer);
    int     (*open)(struct vfs_node* node, uint32_t flags);
    int     (*close)(struct vfs_node* node);
    int     (*ioctl)(struct vfs_node* node, uint64_t request, uint64_t arg);
} vfs_ops_t;

typedef struct vfs_node {
    char        name[64];
    uint32_t    type;
    uint64_t    size;
    uint32_t    flags;
    vfs_ops_t*  ops;
    void*       internal;
} vfs_node_t;

typedef struct {
    vfs_node_t* node;
    uint64_t    offset;
    uint32_t    flags;
    uint32_t    ref_count;
    int         is_used;
} file_descriptor_t;

// Системный интерфейс VFS ядра
void               vfs_init(void);
vfs_node_t*        vfs_resolve_path(const char* path);
file_descriptor_t* vfs_allocate_fd(vfs_node_t* node, uint32_t flags);
void               vfs_release_fd(file_descriptor_t* fd);

// Высокоуровневые сисколл-обработчики
int     vfs_sys_open(const char* path, uint32_t flags);
int64_t vfs_sys_read(int fd_num, void* buf, uint64_t count);
int64_t vfs_sys_write(int fd_num, const void* buf, uint64_t count);
int     vfs_sys_close(int fd_num);
int     vfs_sys_ioctl(int fd_num, uint64_t req, uint64_t arg);

#endif