# ==============================================================================
# overOS Master Makefile
# Архитектура: x86_64 Bare-Metal (64-bit Long Mode, Two-Stage MBR + FAT32, ядро грузится ФАЙЛОМ /sys/KERNEL.BIN)
# ==============================================================================

BUILD_DIR    := build
SRC_DIR      := src

CC    := gcc
LD    := ld
NASM  := nasm

# Флаги компиляции 64-битного ядра и программ overOS
CFLAGS       := -m64 -std=c99 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                -mno-red-zone -mcmodel=kernel -U_FORTIFY_SOURCE -O2 -Wall -Wextra \
                -mno-sse -mno-sse2 -mno-mmx
# Модули перемещаемые (PIE): грузятся по любому адресу, без -mcmodel=kernel
CFLAGS_MOD   := -m64 -std=c99 -ffreestanding -fPIE -fvisibility=hidden -fno-plt \
                -fno-stack-protector -mno-red-zone -U_FORTIFY_SOURCE -O2 -Wall -Wextra \
                -mgeneral-regs-only -mno-sse -mno-sse2 -mno-mmx
LDFLAGS_MOD  := -m elf_x86_64 -T $(SRC_DIR)/modules/module.ld -z max-page-size=4096 --emit-relocs -no-pie
CFLAGS_PROG  := -m64 -std=c99 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                -mno-red-zone -mcmodel=large -O2 -Wall -Wextra \
                -I$(SRC_DIR)/drivers -I$(SRC_DIR)/kernel

LDFLAGS      := -m elf_x86_64 -T linker.ld --oformat binary -no-pie
PROG_LDFLAGS := -m elf_x86_64 -T user.ld --oformat binary -no-pie

DISK_IMG     := $(BUILD_DIR)/os.img
PART_IMG     := $(BUILD_DIR)/fat32_part.img
PART_SIZE_MB := 512

# ==============================================================================
# СИСТЕМНЫЕ МОДУЛИ (.SYS, Ring 0)
# ==============================================================================
MODULES     := mc redactor memedit stress gpu cpu hwinfo
MODULE_BINS := $(patsubst %, $(BUILD_DIR)/%.sys, $(MODULES))

# ==============================================================================
# ПОЛЬЗОВАТЕЛЬСКИЕ ПРОГРАММЫ (.PRG, Ring 3)
# ==============================================================================
PROG_DIR    := $(SRC_DIR)/programs
PROG_SRCS   := $(wildcard $(PROG_DIR)/*.c)
PROG_BINS   := $(patsubst $(PROG_DIR)/%.c, $(BUILD_DIR)/%.prg, $(PROG_SRCS))

# Пути к DOOM
DOOM_DIR    := $(SRC_DIR)/files/progs/DOOM
DOOM_PRG    := $(DOOM_DIR)/DOOM.PRG
DOOM_WAD    := $(DOOM_DIR)/DOOM1.WAD

# ==============================================================================
# ИСХОДНЫЕ ФАЙЛЫ ЯДРА
# ==============================================================================
C_SRC   := $(SRC_DIR)/kernel/kernel.c \
           $(SRC_DIR)/kernel/main.c \
           $(SRC_DIR)/kernel/tty.c \
           $(SRC_DIR)/kernel/sched.c \
           $(SRC_DIR)/kernel/user_mode.c \
           $(SRC_DIR)/kernel/sys_loader.c \
           $(SRC_DIR)/kernel/prog_loader.c \
           $(SRC_DIR)/kernel/bsod.c \
           $(SRC_DIR)/kernel/keyboard.c \
           $(SRC_DIR)/kernel/mouse.c \
           $(SRC_DIR)/memory/pmm.c \
           $(SRC_DIR)/memory/vmm.c \
           $(SRC_DIR)/font/fontdata_ru_8x16.c \
           $(SRC_DIR)/drivers/acpi.c \
           $(SRC_DIR)/drivers/display.c \
           $(SRC_DIR)/drivers/pci.c \
           $(SRC_DIR)/drivers/usb/ehci.c \
           $(SRC_DIR)/drivers/usb/ehci-msc.c \
           $(SRC_DIR)/fs/fat16.c \
           $(SRC_DIR)/fs/fat32.c \
           $(SRC_DIR)/fs/fs.c

OBJS    := $(BUILD_DIR)/entry_kernel.o \
           $(BUILD_DIR)/switch.o \
           $(BUILD_DIR)/smp_trampoline.o \
           $(BUILD_DIR)/interrupts.o \
           $(filter %.o, $(C_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o))

# ==============================================================================
# ГЛАВНЫЕ ЦЕЛИ
# ==============================================================================
all: $(DISK_IMG)

modules: $(MODULE_BINS)

progs: $(PROG_BINS)

# ------------------------------------------------------------------------------
# Ассемблерные файлы загрузчика и точки входа (папка src/boot)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/entry_kernel.o: $(SRC_DIR)/boot/entry_kernel.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/boot_stage1.bin: $(SRC_DIR)/boot/boot_stage1.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

# --- Stage 2 = asm-часть (вход, FAT32-загрузчик ядра) + C-часть (меню VBE) в одном образе на 32 сектора ---
# C-часть компилируется в 16-битный код (gcc -m16: 32-битные инструкции с префиксами 0x66/0x67),
# линкуется на 0x2000 и вшивается в boot_stage2.bin через incbin (см. boot_stage2.asm).
CFLAGS_BOOT16 := -m16 -march=i386 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie \
                 -fno-stack-protector -fno-asynchronous-unwind-tables -fcf-protection=none \
                 -fomit-frame-pointer -mno-sse -mno-mmx -fno-tree-loop-distribute-patterns \
                 -Os -Wall -Wextra

$(BUILD_DIR)/boot_menu.o: $(SRC_DIR)/boot/vbe_menu.c $(SRC_DIR)/boot/bios.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BOOT16) -c $< -o $@

$(BUILD_DIR)/boot_menu.bin: $(BUILD_DIR)/boot_menu.o $(SRC_DIR)/boot/stage2_menu.ld
	$(LD) -m elf_i386 -T $(SRC_DIR)/boot/stage2_menu.ld -o $@ $<

$(BUILD_DIR)/boot_stage2.bin: $(SRC_DIR)/boot/boot_stage2.asm $(SRC_DIR)/boot/boot_fat32.asm $(BUILD_DIR)/boot_menu.bin
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f bin -I $(SRC_DIR)/boot/ -I $(BUILD_DIR)/ $< -o $@
	@# Stage 2 = ровно 32 сектора (16 384 байт, LBA 1..32). boot_stage2.asm сам добивает файл до 16 КиБ
	@# директивой times, и NASM падает с ошибкой, если код не влезает; truncate оставлен как страховка.
	@truncate -s 16384 $@

# --- проверка логики меню на хосте (обычный gcc, без QEMU и BIOS): make test-menu ---
test-menu: tools/test_menu.c $(SRC_DIR)/boot/vbe_menu.c $(SRC_DIR)/boot/bios.h
	@mkdir -p $(BUILD_DIR)
	gcc -DHOST_TEST -std=gnu11 -Wall -Wextra -o $(BUILD_DIR)/test_menu tools/test_menu.c
	@for i in 0 1 2 3; do $(BUILD_DIR)/test_menu $$i || exit 1; done
	@$(BUILD_DIR)/test_menu 4 >/dev/null; test $$? -eq 42 && echo "no VBE: stops with a message - ok"
	@$(BUILD_DIR)/test_menu 5 >/dev/null; test $$? -eq 42 && echo "no 32-bit modes: stops with a message - ok"

# ------------------------------------------------------------------------------
# Прочие ассемблерные файлы ядра (папка src/asm)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/switch.o: $(SRC_DIR)/asm/switch.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/smp_trampoline.o: $(SRC_DIR)/asm/smp_trampoline.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/interrupts.o: $(SRC_DIR)/asm/interrupts.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/crt0.o: $(SRC_DIR)/asm/crt0.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

# ------------------------------------------------------------------------------
# Компиляция C-файлов ядра в 64 бита
# ------------------------------------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------
# Двухпроходная сборка ядра с автоматической таблицей символов (ksyms)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/kernel_stage1.elf: $(OBJS)
	$(LD) -m elf_x86_64 -T linker.ld --defsym=auto_ksyms=0 -no-pie -o $@ $^

$(BUILD_DIR)/ksyms.c: $(BUILD_DIR)/kernel_stage1.elf
	@echo "/* Auto-generated Kernel Symbols (x86_64) */" > $@
	@echo '#include <stdint.h>' >> $@
	@echo 'typedef struct { const char* name; uint64_t addr; } ksym_t;' >> $@
	@echo 'const ksym_t auto_ksyms[] = {' >> $@
	@nm $< | grep -E ' [Tt] ' | awk '{print "    {\"" $$3 "\", 0x" $$1 "},"}' >> $@
	@echo '    {0, 0}' >> $@
	@echo '};' >> $@

$(BUILD_DIR)/ksyms.o: $(BUILD_DIR)/ksyms.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.bin: $(OBJS) $(BUILD_DIR)/ksyms.o
	$(LD) $(LDFLAGS) -o $@ $^
	@ls -l $@

# ------------------------------------------------------------------------------
# Сборка системных модулей .SYS (Ring 0)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/%.sys: $(SRC_DIR)/modules/%_module.c $(SRC_DIR)/modules/module.ld tools/elf2sys.py
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_MOD) -c $< -o $(BUILD_DIR)/$*_module.o
	$(LD) $(LDFLAGS_MOD) $(BUILD_DIR)/$*_module.o -o $(BUILD_DIR)/$*.elf
	python3 tools/elf2sys.py $(BUILD_DIR)/$*.elf $@
	@echo ">>> Модуль $@ успешно собран."

# ------------------------------------------------------------------------------
# Сборка составного модуля REDACTOR.SYS (Ring 0, x86_64)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/redactor.sys: $(SRC_DIR)/modules/redactor_module.c \
                          $(SRC_DIR)/modules/redactor/editor.c \
                          $(SRC_DIR)/modules/redactor/compiler.c \
                          $(SRC_DIR)/modules/module.ld tools/elf2sys.py
	@mkdir -p $(BUILD_DIR)/redactor
	$(CC) $(CFLAGS_MOD) -c $(SRC_DIR)/modules/redactor_module.c -o $(BUILD_DIR)/redactor/redactor_module.o
	$(CC) $(CFLAGS_MOD) -c $(SRC_DIR)/modules/redactor/editor.c -o $(BUILD_DIR)/redactor/editor.o
	$(CC) $(CFLAGS_MOD) -c $(SRC_DIR)/modules/redactor/compiler.c -o $(BUILD_DIR)/redactor/compiler.o
	$(LD) $(LDFLAGS_MOD) $(BUILD_DIR)/redactor/redactor_module.o \
	    $(BUILD_DIR)/redactor/editor.o \
	    $(BUILD_DIR)/redactor/compiler.o -o $(BUILD_DIR)/redactor.elf
	python3 tools/elf2sys.py $(BUILD_DIR)/redactor.elf $@
	@echo ">>> Модуль $@ успешно собран."

# ------------------------------------------------------------------------------
# Сборка пользовательских программ .PRG (Ring 3)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/%.prg: $(PROG_DIR)/%.c $(BUILD_DIR)/crt0.o user.ld
	@mkdir -p $(BUILD_DIR)/programs
	$(CC) $(CFLAGS_PROG) -c $< -o $(BUILD_DIR)/programs/$*.o
	$(LD) -m elf_x86_64 -T user.ld -no-pie $(BUILD_DIR)/crt0.o $(BUILD_DIR)/programs/$*.o -o $(BUILD_DIR)/programs/$*.elf
	python3 tools/elf2prg.py $(BUILD_DIR)/programs/$*.elf $@
	@echo ">>> Программа $@ успешно собрана в формате DPRG."

# Автосборка DOOM.PRG через clean + make
$(DOOM_PRG):
	@if [ -d "$(DOOM_DIR)" ]; then \
		echo ">>> Полная пересборка DOOM (clean + make)..."; \
		$(MAKE) -C $(DOOM_DIR) clean && $(MAKE) -C $(DOOM_DIR); \
	fi

# -----------------------------------------------------------------------------
# Создание раздела FAT32 и упаковка софта
# -----------------------------------------------------------------------------
$(PART_IMG): $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/boot_stage1.bin $(MODULE_BINS) $(PROG_BINS) $(DOOM_PRG)
	@mkdir -p $(BUILD_DIR)
	@echo ">>> Создаем раздел FAT32 на $(PART_SIZE_MB) МБ..."
	@rm -f $@
	@truncate -s $(PART_SIZE_MB)M $@
	@mkfs.vfat -F 32 -s 8 -n "OVER_OS" $@ >/dev/null
	@if command -v mcopy >/dev/null 2>&1; then \
	    mmd -i $@ ::sys 2>/dev/null || true; \
	    mmd -i $@ ::utils 2>/dev/null || true; \
	    mmd -i $@ ::programs 2>/dev/null || true; \
	    mmd -i $@ ::programs/DOOM 2>/dev/null || true; \
	    mmd -i $@ ::Code 2>/dev/null || true; \
	    mcopy -i $@ $(BUILD_DIR)/kernel.bin ::sys/KERNEL.BIN; \
	    if [ -f "$(SRC_DIR)/files/image/logo.bmp" ]; then \
	        mcopy -i $@ $(SRC_DIR)/files/image/logo.bmp ::sys/logo.bmp; \
	    fi; \
	    for mod in $(MODULE_BINS); do \
	        fname=$$(basename $$mod); \
	        mcopy -i $@ $$mod ::utils/$$fname; \
	    done; \
	    for prg in $(PROG_BINS); do \
	        [ -f "$$prg" ] || continue; \
	        fname=$$(basename $$prg); \
	        mcopy -i $@ $$prg ::programs/$$fname; \
	    done; \
	    if [ -f "$(DOOM_PRG)" ]; then \
	        mcopy -i $@ $(DOOM_PRG) ::programs/DOOM/DOOM.PRG; \
	        echo ">>> Скопирован DOOM.PRG в ::programs/DOOM/"; \
	    fi; \
	    if [ -f "$(DOOM_WAD)" ]; then \
	        mcopy -i $@ $(DOOM_WAD) ::programs/DOOM/DOOM1.WAD; \
	        echo ">>> Скопирован DOOM1.WAD в ::programs/DOOM/"; \
	    fi; \
	fi

# ------------------------------------------------------------------------------
# Сборка финального RAW-образа диска
# ------------------------------------------------------------------------------
$(DISK_IMG): $(BUILD_DIR)/boot_stage1.bin $(BUILD_DIR)/boot_stage2.bin $(BUILD_DIR)/kernel.bin $(PART_IMG)
	@echo ">>> Собираем полный образ диска с Two-Stage Boot (FAT32)..."
	@rm -f $@
	@# 1. Записываем Stage 1 (LBA 0, 512 байт)
	@cp $(BUILD_DIR)/boot_stage1.bin $@
	@# 2. Выделяем ровно 1 МБ (2048 секторов) под загрузочную область
	@truncate -s 1M $@
	@# 3. Записываем Stage 2 на LBA 1 (занимает ровно 32 сектора: 1..32)
	@dd if=$(BUILD_DIR)/boot_stage2.bin of=$@ bs=512 seek=1 conv=notrunc status=none
	@# 4. Ядро в загрузочную область БОЛЬШЕ НЕ пишется: Stage 2 читает его как ФАЙЛ /sys/KERNEL.BIN
	@#    из раздела FAT32 (копируется в $(PART_IMG) ниже, "boot from file"). Размер ядра до 14 МиБ.
	@# 5. Прописываем стандартную таблицу MBR (Partition 1: Boot RAW, Partition 2: FAT32 с LBA 2048)
	@python3 -c "import struct; f=open('$@','r+b'); \
	    f.seek(446); \
	    p1 = struct.pack('<BBBBBBBBII', 0x80, 0x00, 0x02, 0x00, 0x7F, 0x20, 0x20, 0x00, 1, 2047); \
	    p2 = struct.pack('<BBBBBBBBII', 0x00, 0x20, 0x21, 0x00, 0x0C, 0xFE, 0xFF, 0xFF, 2048, $(PART_SIZE_MB)*2048); \
	    f.write(p1 + p2 + b'\x00'*32 + b'\x55\xAA')"
	@# 6. Дописываем раздел FAT32 (начиная ровно с 1 МБ / LBA 2048)
	@cat $(PART_IMG) >> $@
	@echo ">>> Готово: $(DISK_IMG) (overOS Two-Stage FAT32) успешно собран."

# ------------------------------------------------------------------------------
# Запуск в QEMU x86_64
# ------------------------------------------------------------------------------
run: all
	qemu-system-x86_64 \
	    -m 1G \
	    -cpu max \
	    -serial stdio \
	    -d int,cpu_reset,guest_errors \
	    -D qemu_error.log \
	    -no-reboot \
	    -no-shutdown \
	    -device usb-ehci,id=ehci \
	    -drive if=none,id=usb_drive,file=$(DISK_IMG),format=raw \
	    -device usb-storage,bus=ehci.0,drive=usb_drive \
        -vga std -global VGA.vgamem_mb=2

run-usb: all
	qemu-system-x86_64 \
	    -m 1G \
	    -cpu max \
	    -serial stdio \
	    -d int,cpu_reset,guest_errors \
	    -D qemu_error.log \
	    -no-reboot \
	    -no-shutdown \
	    -device usb-ehci,id=ehci \
	    -drive if=none,id=usb_drive,file=/dev/sdc,format=raw \
	    -device usb-storage,bus=ehci.0,drive=usb_drive \
        -vga std -global VGA.vgamem_mb=2

# ------------------------------------------------------------------------------
# Быстрое обновление модулей и программ на уже размеченной флешке
# ------------------------------------------------------------------------------
install-modules: $(MODULE_BINS) $(PROG_BINS)
	@test -n "$(DEV)" || { echo "!! Укажи диск: make install-modules DEV=/dev/sdX"; exit 1; }
	@for mod in $(MODULE_BINS); do \
	    fname=$$(basename $$mod | tr 'a-z' 'A-Z'); \
	    sudo mcopy -o -i $(DEV)2 $$mod ::$$fname; \
	done
	@for prg in $(PROG_BINS); do \
	    [ -f "$$prg" ] || continue; \
	    fname=$$(basename $$prg | tr 'a-z' 'A-Z'); \
	    sudo mcopy -o -i $(DEV)2 $$prg ::$$fname; \
	done
	@if [ -f "DOOM1.WAD" ]; then \
	    sudo mcopy -o -i $(DEV)2 DOOM1.WAD ::DOOM1.WAD; \
	fi
	sync
	@echo ">>> Все модули и программы успешно обновлены на $(DEV)2"

# ------------------------------------------------------------------------------
# Полная прошивка USB-флешки с автоматическим поиском накопителя
# ------------------------------------------------------------------------------
install: all
ifdef DEV
	@lsblk $(DEV) >/dev/null 2>&1 || { echo "!! $(DEV) ne существует"; exit 1; }
	@$(MAKE) --no-print-directory _write_disk TARGET_DEV=$(DEV)
else
	@CANDIDATES=$$(lsblk -dn -o NAME,TRAN,RM,TYPE | awk '$$2=="usb" && $$3=="1" && $$4=="disk"{print $$1}'); \
	COUNT=$$(echo "$$CANDIDATES" | grep -c .); \
	if [ "$$COUNT" -eq 0 ]; then \
	    echo "!! Съёмных USB-дисков не найдено."; \
	    exit 1; \
	elif [ "$$COUNT" -gt 1 ]; then \
	    echo "!! Найдено несколько USB-дисков — укажи явно: make install DEV=/dev/sdX"; \
	    exit 1; \
	fi; \
	DEV=/dev/$$CANDIDATES; \
	echo ">>> Найден съемный накопитель: $$DEV"; \
	$(MAKE) --no-print-directory _write_disk TARGET_DEV=$$DEV
endif

_write_disk:
	@for part in $(TARGET_DEV)*; do \
	    pmp=$$(findmnt -n -o TARGET "$$part" 2>/dev/null); \
	    if [ -n "$$pmp" ]; then \
	        sudo umount "$$part" 2>/dev/null; \
	    fi; \
	done
	@echo ">>> Записываем систему и раздел на $(TARGET_DEV)..."
	sudo dd if=$(DISK_IMG) of=$(TARGET_DEV) bs=1M status=progress conv=fsync
	sync
	@echo ">>> Оповещаем ядро Linux о новой таблице разделов..."
	-sudo partprobe $(TARGET_DEV) 2>/dev/null || sudo blockdev --rereadpt $(TARGET_DEV) 2>/dev/null || true
	@echo ">>> Флешка $(TARGET_DEV) успешно прошита и готова к загрузке."

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all modules progs run install install-modules _write_disk clean run-usb test-menu
