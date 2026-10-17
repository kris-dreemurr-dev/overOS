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
             -mcmodel=large -Wall -Wextra -I. -I../../../libs -I../../../libs/common

LDFLAGS   := -m elf_x86_64 -T desktop.ld -no-pie
LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

SRCS      := desktop.c gfx.c settings.c window.c osver.c taskbar.c greeter.c wserver.c \
             appinfo.c notifier.c
# Файлы из src/libs/libgfx, которые собираются вместе с композитором (шрифт, значки)
LIBSRCS   := ttf.c svg.c
OBJS      := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS)) \
             $(patsubst %.c, $(BUILD_DIR)/lib_%.o, $(LIBSRCS))

all: $(TARGET)

$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

# Зависимость от ВСЕХ заголовков: иначе правка window.h/wproto.h не пересобирает часть
# файлов, и они продолжают работать со старой раскладкой структур (ловили такой баг)
HDRS := $(wildcard *.h) $(wildcard ../../../libs/common/*.h) $(wildcard ../../../libs/*/*.h)

$(BUILD_DIR)/%.o: %.c $(HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lib_%.o: ../../../libs/libgfx/%.c $(HDRS)
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