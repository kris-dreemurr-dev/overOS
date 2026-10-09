#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// ==================== [ OS System Config ] ====================
#define  OS_NAME            "overOS"
#define  OS_LOWER_NAME      "overos"
#define  OS_ARCH            "x86_64"
#define  OS_VERSION         "0.0.1"

#define  FUN_EDITION        1
#define  DEBUG_MODE         0
#define  BSOD_ENABLED       1
// ==============================================================

#if !FUN_EDITION
#define LOGO_ASCII_ROWS     16
#define LOGO_ASCII_COLS     30

static const char* const logo_ascii[LOGO_ASCII_ROWS] = {
    "           @@@@@@@@           ",
    "           @      @           ",
    "      @@    @@  @@            ",
    "     @@ @@@@@@@@@@@@@         ",
    "      @@@  @@@@@@@@  @@       ",
    "     @@ @@@        @@@ @@     ",
    "    @  @@     @@     @@  @    ",
    "   @@ @       @@       @ @@   ",
    "   @ @@      @@@@      @@ @   ",
    "   @ @@      @  @      @@ @   ",
    "   @ @@       @@       @@ @   ",
    "   @@ @@              @@ @@   ",
    "    @@ @@            @@ @@    ",
    "     @@  @@@      @@@  @@     ",
    "       @@@   @@@@   @@@       ",
    "          @@@@@@@@@@          "
};
#endif

#endif // CONFIG_H