#ifndef OV_STDIO_H
#define OV_STDIO_H
// stdio.h сисрута: FFmpeg пишет только журнал (av_log -> stderr) и форматирует
// строки. Полноценных потоков с буферизацией в overOS нет, FILE* — заглушка на
// дескриптор ядра.
#include <stddef.h>
#include <stdarg.h>
typedef struct ov_file FILE;
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
#define _IONBF 2
#define _IOLBF 1
#define _IOFBF 0
int    snprintf(char* buf, size_t n, const char* fmt, ...);
int    vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int    sprintf(char* buf, const char* fmt, ...);
int    printf(const char* fmt, ...);
int    fprintf(FILE* f, const char* fmt, ...);
int    vfprintf(FILE* f, const char* fmt, va_list ap);
int    fputc(int c, FILE* f);
int    fputs(const char* s, FILE* f);
int    putchar(int c);
int    fflush(FILE* f);
int    setvbuf(FILE* f, char* buf, int mode, size_t size);
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f);
size_t fread(void* p, size_t sz, size_t n, FILE* f);
FILE*  fopen(const char* path, const char* mode);
int    fclose(FILE* f);
FILE*  fdopen(int fd, const char* mode);
int    fseek(FILE* f, long off, int whence);
long   ftell(FILE* f);
int    sscanf(const char* s, const char* fmt, ...);
#endif
