CC        := gcc
LD        := ld
NASM      := nasm
PYTHON    := python3

TARGET    := ovfiles.prg
ELF       := ovfiles.elf
BUILD_DIR := build

ELF2PRG   := ../../../../tools/elf2prg.py
CRT0_SRC  := ../../../../src/asm/crt0.asm

CFLAGS    := -m64 -std=c99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mno-red-zone \
             -mcmodel=large -Wall -Wextra -I.

LDFLAGS   := -m elf_x86_64 -T overfiles.ld -no-pie
LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

all: $(TARGET)

$(BUILD_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/ovfiles.o: overfiles.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(BUILD_DIR)/crt0.o $(BUILD_DIR)/ovfiles.o
	$(LD) $(LDFLAGS) $^ $(LIBGCC) -o $@

# Собираем СТРОГО с меткой --desktop!
$(TARGET): $(ELF)
	$(PYTHON) $(ELF2PRG) $(ELF) $(TARGET) --desktop
	@echo ">>> overFiles успешно собран для /desprogs с флагом --desktop!"

clean:
	rm -rf $(BUILD_DIR) $(ELF) $(TARGET)

.PHONY: all clean