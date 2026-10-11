CC        := gcc
LD        := ld
NASM      := nasm
PYTHON    := python3

TARGET    := desktop.prg
ELF       := desktop.elf
BUILD_DIR := build

ELF2PRG   := $(firstword $(wildcard tools/elf2prg.py \
                                   ../tools/elf2prg.py \
                                   ../../tools/elf2prg.py \
                                   ../../../tools/elf2prg.py \
                                   ../../../../tools/elf2prg.py))

CRT0_SRC  := $(firstword $(wildcard crt0.asm \
                                   ../crt0.asm \
                                   ../../crt0.asm \
                                   ../../../asm/crt0.asm \
                                   ../../../../src/asm/crt0.asm))

CFLAGS    := -m64 -std=c99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mno-red-zone \
             -mcmodel=large -Wall -Wextra -I.

LDFLAGS   := -m elf_x86_64 -T desktop.ld -no-pie
LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

SRCS      := desktop.c gfx.c settings.c window.c osver.c taskbar.c
OBJS      := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))

all: $(TARGET)

$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(BUILD_DIR)/crt0.o $(OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBGCC) -o $@

$(TARGET): $(ELF)
	$(PYTHON) $(ELF2PRG) $(ELF) $(TARGET) --desktop
	@echo ">>> Модульный Desktop Environment собран!"

clean:
	rm -rf $(BUILD_DIR) $(ELF) $(TARGET) *.o

.PHONY: all clean