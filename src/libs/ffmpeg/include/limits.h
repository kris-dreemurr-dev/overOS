#ifndef OV_LIMITS_H
#define OV_LIMITS_H
// Пределы, которых нет в limits.h компилятора (тот включает этот файл через
// #include_next): размеры путей и POSIX-овые максимумы.
#define PATH_MAX   260
#define NAME_MAX   255
#define SSIZE_MAX  0x7FFFFFFFFFFFFFFFL
#endif
