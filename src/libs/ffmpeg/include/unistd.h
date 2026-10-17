#ifndef OV_UNISTD_H
#define OV_UNISTD_H
// Файловый ввод-вывод overOS: open/read/write/close/lseek — настоящие сисколлы.
#include <sys/types.h>
#include <stddef.h>
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
int     close(int fd);
ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
off_t   lseek(int fd, off_t off, int whence);
int     unlink(const char* path);
int     isatty(int fd);
unsigned sleep(unsigned sec);
int     usleep(unsigned usec);
#endif
