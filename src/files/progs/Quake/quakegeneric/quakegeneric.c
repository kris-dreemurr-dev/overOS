/*
Copyright (C) 1996-1997 Id Software, Inc.
*/

#include "quakedef.h"
#include <stdint.h>

extern void QG_Init(void);

// Системный вызов калибровки частоты процессора devOS[cite: 3]
static inline uint32_t sys_tsc_per_ms(void) {
    uint32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "0"(22) : "memory");
    return r;
}

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void QG_Tick(double duration)
{
	Host_Frame(duration);
}

void QG_Create(int argc, char *argv[])
{
	static quakeparms_t parms;

	parms.memsize = 8*1024*1024;
	parms.membase = malloc (parms.memsize);
	parms.basedir = ".";

	COM_InitArgv (argc, argv);

	parms.argc = com_argc;
	parms.argv = com_argv;

	printf ("Host_Init\n");
	Host_Init (&parms);
}

// Точка входа в стиле doomgeneric.c
int main(void)
{
    uint64_t tsc_per_ms = sys_tsc_per_ms();
    if (tsc_per_ms == 0) tsc_per_ms = 2000000;

    char* argv[] = {
        [0] = "quake",
        [1] = "-basedir",
        [2] = ".",
        [3] = 0
    };

    // 1. Инициализация HAL и самого движка Quake
    QG_Init();
    QG_Create(3, argv);

    // 2. Включаем видеорежим в devOS через int 0x80 (как в Doom)
    __asm__ volatile ("int $0x80" : : "a"(5), "b"(0x00000000) : "memory");

    uint64_t last_tsc = read_tsc();

    // 3. Игровой цикл
    while (1) {
        uint64_t current_tsc = read_tsc();
        
        // Время кадра в секундах
        double duration = ((double)(current_tsc - last_tsc) / (double)tsc_per_ms) / 1000.0;

        // Ограничитель, чтобы процессор не крутился чаще 1000 FPS
        if (duration < 0.001) {
            __asm__ volatile ("pause");
            continue;
        }

        last_tsc = current_tsc;

        // Отсекаем слишком длинные паузы (защита от скачков физики)
        if (duration > 0.1) duration = 0.1;

        QG_Tick(duration);
    }

    return 0;
}