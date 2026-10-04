CC        := gcc
LD        := ld
NASM      := nasm
OBJCOPY   := objcopy

BUILD_DIR := build

# Автопоиск пути к crt0.asm относительно директории с Doom
CRT0_SRC  := $(firstword $(wildcard ../../../asm/crt0.asm ../../asm/crt0.asm ../src/asm/crt0.asm crt0.asm))

CFLAGS    := -m64 -std=gnu99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mcmodel=large -Wall -Wextra \
             -DDOOMGENERIC -Idoomgeneric -I.

# Исходники из корня DOOM и из подпапки doomgeneric
SRCS_ROOT := $(wildcard *.c)
SRCS_DG   := $(filter-out doomgeneric/doomgeneric_%.c doomgeneric/i_allegro%.c doomgeneric/i_sdl%.c, $(wildcard doomgeneric/*.c))

# Объектные файлы складываем строго в папку build/
OBJS_ROOT := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS_ROOT))
OBJS_DG   := $(patsubst doomgeneric/%.c, $(BUILD_DIR)/doomgeneric/%.o, $(SRCS_DG))

# crt0.o СТРОГО на первом месте для правильной точки входа по адресу 0x01000000
ALL_OBJS  := $(BUILD_DIR)/crt0.o $(OBJS_ROOT) $(OBJS_DG)

# Библиотека для 64-битной математики (__udivdi3) под x86_64
LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

all: DOOM.PRG

# Сборка универсального 64-битного рантайма в ELF64
$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

# Компиляция файлов из корня DOOM/ в build/
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Компиляция движка из doomgeneric/ в build/doomgeneric/
$(BUILD_DIR)/doomgeneric/%.o: doomgeneric/%.c
	@mkdir -p $(BUILD_DIR)/doomgeneric
	$(CC) $(CFLAGS) -c $< -o $@

doom.elf: $(ALL_OBJS)
	$(LD) -m elf_x86_64 -T doom.ld $(ALL_OBJS) $(LIBGCC) -o doom.elf

DOOM.PRG: doom.elf
	python3 ../../../../tools/elf2prg.py doom.elf DOOM.PRG
	@echo "========================================="
	@echo "  SUCCESS: overOS 64-bit DOOM.PRG ready!  "
	@echo "========================================="

clean:
	rm -rf $(BUILD_DIR) doom.elf DOOM.PRG *.o
