CC        := gcc
LD        := ld
NASM      := nasm

BUILD_DIR := build
CRT0_SRC  := $(firstword $(wildcard ../../../asm/crt0.asm ../../asm/crt0.asm ../src/asm/crt0.asm crt0.asm)) #

# Флаги компиляции из вашего Doom
CFLAGS    := -m64 -std=gnu99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mcmodel=large -Wall -Wextra \
             -Iquakegeneric -I.

# Ищем исходники Quake
SRCS_ROOT := $(wildcard *.c)
SRCS_QG := $(filter-out quakegeneric/quakegeneric_dos.c quakegeneric/quakegeneric_sdl.c quakegeneric/quakegeneric_sdl2.c quakegeneric/quakegeneric_w32.c quakegeneric/quakegeneric_x11.c quakegeneric/quakegeneric_null.c, $(wildcard quakegeneric/*.c))

OBJS_ROOT := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS_ROOT))
OBJS_QG   := $(patsubst quakegeneric/%.c, $(BUILD_DIR)/quakegeneric/%.o, $(SRCS_QG))

# crt0.o на первом месте
ALL_OBJS  := $(BUILD_DIR)/crt0.o $(OBJS_ROOT) $(OBJS_QG)

LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name) #

all: QUAKE.PRG

$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/quakegeneric/%.o: quakegeneric/%.c
	@mkdir -p $(BUILD_DIR)/quakegeneric
	$(CC) $(CFLAGS) -c $< -o $@

quake.elf: $(ALL_OBJS)
	# Используем quake.ld (скопируйте doom.ld и переименуйте)
	$(LD) -m elf_x86_64 -T quake.ld $(ALL_OBJS) $(LIBGCC) -o quake.elf #

QUAKE.PRG: quake.elf
	python3 ../../../../tools/elf2prg.py quake.elf QUAKE.PRG #
	@echo "========================================="
	@echo "  SUCCESS: overOS 64-bit QUAKE.PRG ready!"
	@echo "========================================="

clean:
	rm -rf $(BUILD_DIR) quake.elf QUAKE.PRG *.o