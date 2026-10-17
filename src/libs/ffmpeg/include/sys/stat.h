#ifndef OV_SYS_STAT_H
#define OV_SYS_STAT_H
// Прав доступа у файлов FAT32 нет (они в /etc/perms), поэтому здесь только то,
// без чего не собирается os_support.h: тип stat и биты режима.
#include <sys/types.h>
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IRWXU  0700
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
struct stat {
    mode_t st_mode;
    off_t  st_size;
    long   st_mtime;
};
int stat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
int mkdir(const char* path, mode_t mode);
#endif
