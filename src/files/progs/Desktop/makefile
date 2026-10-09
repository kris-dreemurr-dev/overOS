CC        := gcc
LD        := ld
NASM      := nasm
PYTHON    := python3

TARGET    := desktop.prg
ELF       := desktop.elf
BUILD_DIR := build

# Автопоиск пути к elf2prg.py относительно папки программы
ELF2PRG   := $(firstword $(wildcard tools/elf2prg.py \
                                   ../tools/elf2prg.py \
                                   ../../tools/elf2prg.py \
                                   ../../../tools/elf2prg.py \
                                   ../../../../tools/elf2prg.py))

# Автопоиск исходника crt0.asm в проекте
CRT0_SRC  := $(firstword $(wildcard crt0.asm \
                                   ../crt0.asm \
                                   ../../crt0.asm \
                                   ../../../asm/crt0.asm \
                                   ../../../../src/asm/crt0.asm \
                                   ../../../../asm/crt0.asm))

# Запасной поиск уже скомпилированного crt0.o из корневого build/
CRT0_OBJ_PREBUILT := $(firstword $(wildcard ../../build/crt0.o \
                                            ../../../build/crt0.o \
                                            ../../../../build/crt0.o))

# Флаги компиляции под 64-битный режим Ring 3 ядра devOS
CFLAGS    := -m64 -std=c99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mno-red-zone \
             -mcmodel=large -Wall -Wextra -I.

LDFLAGS   := -m elf_x86_64 -T desktop.ld -no-pie
LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

# Определяем, откуда брать crt0.o
ifeq ($(CRT0_SRC),)
    CRT0_OBJ := $(CRT0_OBJ_PREBUILT)
else
    CRT0_OBJ := $(BUILD_DIR)/crt0.o
endif

all: $(TARGET)

# Сборка стартового рантайма crt0.o (если найден исходник)
$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

# Компиляция основного кода desktop.c
$(BUILD_DIR)/desktop.o: desktop.c icons.h font_8x8.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c desktop.c -o $@

# Линковка в ELF64 со скриптом desktop.ld
$(ELF): $(CRT0_OBJ) $(BUILD_DIR)/desktop.o
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(BUILD_DIR)/desktop.o $(LIBGCC) -o $@

# Конвертация ELF в нативный исполняемый формат devOS (DPRG)
$(TARGET): $(ELF)
	@if [ -z "$(ELF2PRG)" ]; then \
		echo "[-] ОШИБКА: elf2prg.py не найден!"; exit 1; \
	fi
	$(PYTHON) $(ELF2PRG) $(ELF) $(TARGET)
	@echo "================================================="
	@echo "  SUCCESS: devOS Plasma Desktop ($(TARGET)) готов! "
	@echo "================================================="

clean:
	rm -rf $(BUILD_DIR) $(ELF) $(TARGET) *.o

.PHONY: all clean