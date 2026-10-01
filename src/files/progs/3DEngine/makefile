CC        := gcc
LD        := ld
NASM      := nasm
OBJCOPY   := objcopy

BUILD_DIR := build
ROOT_DIR  := $(abspath ../../../..)
CRT0_SRC  := $(ROOT_DIR)/src/asm/crt0.asm

CFLAGS    := -m64 -std=gnu99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mcmodel=large -Wall -Wextra -I.

SRCS      := main.c mini_libc.c
OBJS      := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))
ALL_OBJS  := $(BUILD_DIR)/crt0.o $(OBJS)

LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

all: CUBE.PRG

# Сборка точки входа с использованием абсолютного пути к crt0.asm
$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

# Компиляция C-файлов движка
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

engine.elf: $(ALL_OBJS)
	$(LD) -m elf_x86_64 -T engine.ld $(ALL_OBJS) $(LIBGCC) -o engine.elf

CUBE.PRG: engine.elf
	python3 $(ROOT_DIR)/tools/elf2prg.py engine.elf CUBE.PRG
	@echo "========================================="
	@echo "  SUCCESS: devOS 3D Wireframe CUBE.PRG!  "
	@echo "========================================="

clean:
	rm -rf $(BUILD_DIR) engine.elf CUBE.PRG *.o

.PHONY: cube3d
cube3d:
	@echo ">>> Пересборка 3D Engine (3DEngine)..."
	$(MAKE) -C $(CUBE_DIR)