#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

#define FBIOGET_VSCREENINFO 0x4600

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t paddr;
} fb_var_info_t;

void        devfs_init(void);
vfs_node_t* devfs_get_node(const char* name);

#endif