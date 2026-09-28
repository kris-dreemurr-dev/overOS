#include <stdio.h>
#include "m_argv.h"
#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);
extern void* memset(void* dest, int c, size_t n);
extern void wad_reset(void);

int main(void) {
    // Чистим структуру перед стартом
    wad_reset();

    char* argv[] = { 
        [0] = "doom", 
        [1] = "-iwad", 
        [2] = "DOOM1.WAD", 
        [3] = 0 
    };

    //printf("[DG] Initializing Doom engine...\n");
    doomgeneric_Create(3, argv);
    //printf("[DG] Init complete! Entering game loop...\n");
    
    __asm__ volatile ("int $0x80" : : "a"(5), "b"(0x00000000) : "memory");
    while (1) {
        doomgeneric_Tick();
    }

    return 0;
}

void doomgeneric_Create(int argc, char **argv)
{
    myargc = argc;
    myargv = argv;

    M_FindResponseFile();
    DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
    DG_Init();
    D_DoomMain ();
}