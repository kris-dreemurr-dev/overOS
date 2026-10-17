# ==============================================================================
# overOS Master Makefile
# Архитектура: x86_64 Bare-Metal (64-bit Long Mode, Two-Stage MBR + FAT32, ядро грузится ФАЙЛОМ /sys/KERNEL.BIN)
# ==============================================================================

BUILD_DIR    := build
SRC_DIR      := src

# Параллельная сборка по умолчанию, на всех ядрах. "make -j1" по-прежнему
# работает, если понадобится разобрать ошибку по порядку. Каждое приложение
# рабочего стола собирается в свой каталог build/, так что их сборки друг
# другу не мешают.
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 1)
endif

CC    := gcc
LD    := ld
NASM  := nasm

# Флаги компиляции 64-битного ядра и программ overOS
CFLAGS       := -m64 -std=c99 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                -mno-red-zone -mcmodel=kernel -U_FORTIFY_SOURCE -O2 -fno-strict-aliasing -Wall -Wextra
CFLAGS       += -I$(SRC_DIR)/libs/common
# Модули перемещаемые (PIE): грузятся по любому адресу, без -mcmodel=kernel
CFLAGS_MOD   := -m64 -std=c99 -ffreestanding -fPIE -fvisibility=hidden -fno-plt \
                -fno-stack-protector -mno-red-zone -U_FORTIFY_SOURCE -O2 -Wall -Wextra \
                -mgeneral-regs-only -mno-sse -mno-sse2 -mno-mmx
LDFLAGS_MOD  := -m elf_x86_64 -T $(SRC_DIR)/modules/module.ld -z max-page-size=4096 --emit-relocs -no-pie
# Разделяемые библиотеки .ll: как модули (PIE, база 0), но для Ring 3 и с SSE —
# libm считает в double, а .prg-программам плавающая точка разрешена
CFLAGS_LL    := -m64 -std=c99 -ffreestanding -fPIE -fvisibility=hidden -fno-plt -fno-builtin \
                -fno-stack-protector -mno-red-zone -U_FORTIFY_SOURCE -O2 -Wall -Wextra \
                -I$(SRC_DIR)/libs -I$(SRC_DIR)/libs/common
LDFLAGS_LL   := -m elf_x86_64 -T $(SRC_DIR)/libs/lib.ld -z max-page-size=4096 --emit-relocs -no-pie
CFLAGS_PROG  := -m64 -std=c99 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                -mno-red-zone -mcmodel=large -O2 -Wall -Wextra \
                -I$(SRC_DIR)/drivers -I$(SRC_DIR)/kernel -I$(SRC_DIR)/libs -I$(SRC_DIR)/libs/common

LDFLAGS      := -m elf_x86_64 -T linker.ld --oformat binary -no-pie
PROG_LDFLAGS := -m elf_x86_64 -T user.ld --oformat binary -no-pie

DISK_IMG     := $(BUILD_DIR)/os.img
PART_IMG     := $(BUILD_DIR)/fat32_part.img
# Картинки образа: раздел пересобирается, если они изменились
IMAGE_ASSETS := $(wildcard $(SRC_DIR)/files/icons/*.svg $(SRC_DIR)/files/image/*.bmp $(SRC_DIR)/files/image/*.png \
                        $(SRC_DIR)/files/sound/*.wav $(SRC_DIR)/files/sound/*.mp3 \
                        $(SRC_DIR)/files/video/*.mp4)
# Параметры раздела и комплектации (переопределяются установщиком)
PART_SIZE_MB    ?= 512
INSTALL_MODULES ?= $(MODULES)
# login.prg и sh.prg ставятся в /sys (вход и оболочка), остальные — в /programs
INSTALL_PROGS   ?= $(filter-out sh login,$(patsubst $(PROG_DIR)/%.c,%,$(PROG_SRCS)))
WITH_DOOM       ?= 1
WITH_QUAKE      ?= 1
# Учётные записи образа (/etc/passwd, /etc/shadow).
# OS_USER пустой — в образе только root, при первом запуске откроется первоначальная настройка (OOBE).
# Задать пользователя сразу (без OOBE): make OS_USER=kris OS_PASS=... — он будет администратором (wheel, sudo).
# ROOT_PASS пустой — root заблокирован, как в Ubuntu/CachyOS; права root — через sudo.
ROOT_PASS       ?=
OS_USER         ?=
OS_PASS         ?=

# Подсказки редактору (clangd / VS Code): флаги и -I для каждого файла.
# На сборку не влияет, но без него редактор не находит "ui.h" и подчёркивает код.
compile-commands:
	@python3 tools/gen_compile_commands.py

# Цель по умолчанию задаём явно: ниже объявлены служебные правила, и первое из них
# иначе стало бы целью по умолчанию (make собирал только его)
.DEFAULT_GOAL := all

# Отпечаток настроек образа: меняем ROOT_PASS/OS_USER/OS_PASS, размер тома или
# набор модулей/программ — раздел пересобирается. Иначе make видел образ
# "свежим" и в нём оставалось содержимое прошлой сборки; из-за этого установщик
# раньше начинал с "make clean" и каждый раз собирал заново даже FFmpeg.
ACCOUNTS_STAMP := $(BUILD_DIR)/.accounts

$(ACCOUNTS_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@printf '%s|%s|%s|%s|%s|%s|%s|%s|%s\n' "$(ROOT_PASS)" "$(OS_USER)" "$(OS_PASS)" \
	    "$(PART_SIZE_MB)" "$(WITH_DOOM)" "$(WITH_QUAKE)" "$(WITH_DESKTOP)" \
	    "$(INSTALL_MODULES)" "$(INSTALL_PROGS)" > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null || mv $@.tmp $@
	@rm -f $@.tmp

# ==============================================================================
# СИСТЕМНЫЕ МОДУЛИ (.SYS, Ring 0)
# ==============================================================================
MODULES     := mc redactor memedit stress gpu cpu hwinfo cloak
# ------------------------------------------------------------------------------
# Разделяемые библиотеки .ll (подробно: src/libs/readme.md)
# Каждая лежит в своём каталоге src/libs/<имя>: исходники библиотеки и заголовок
# подключения <имя>_ll.h, который включает в себя программа.
# ------------------------------------------------------------------------------
LIBS        := libc libm ffmpeg libgfx libui
# Шрифт интерфейса: любой .ttf, попадает в образ как /sys/fonts/ui.ttf и
# разбирается прямо в системе (src/libs/libgfx/ttf.c). Поменять: make UI_FONT_TTF=...
UI_FONT_TTF ?= $(SRC_DIR)/font/Inter_18pt-Bold.ttf

LIBS_DIR     := $(SRC_DIR)/libs
# Однофайловые библиотеки
LIBC_SRC     := $(LIBS_DIR)/libc/libc.c
LIBM_SRC     := $(LIBS_DIR)/libm/libm.c
# Многофайловые: к своему исходнику добавляются общие из src/lib
LIBGFX_SRC   := $(LIBS_DIR)/libgfx/libgfx.c $(LIBS_DIR)/libgfx/ttf.c $(LIBS_DIR)/libgfx/svg.c
# Импортов между .ll нет, поэтому тулкит несёт свои копии шрифта и значков
LIBUI_SRC    := $(LIBS_DIR)/libui/libui.c $(LIBS_DIR)/libui/ui.c $(LIBS_DIR)/libgfx/ttf.c $(LIBS_DIR)/libgfx/svg.c
# /lib/ffmpeg.ll — настоящий FFmpeg, собранный под overOS (src/libs/ffmpeg):
# сисрут, компилятор-обёртка и прослойка av_*. Сборку делает скрипт, исходники
# FFmpeg берутся из архива src/libs/ffmpeg/dist (качается один раз, если его нет).
FFMPEG_DIR   := $(LIBS_DIR)/ffmpeg
FFMPEG_SRC   := $(FFMPEG_DIR)/avshim.c $(FFMPEG_DIR)/ffsys.c $(FFMPEG_DIR)/ovcc \
                $(FFMPEG_DIR)/av.h $(FFMPEG_DIR)/limiter.h \
                $(wildcard $(FFMPEG_DIR)/include/*.h) \
                $(wildcard $(FFMPEG_DIR)/include/sys/*.h) \
                $(wildcard $(FFMPEG_DIR)/patches/*.patch) \
                $(LIBC_SRC) $(LIBM_SRC)
LIB_BINS    := $(patsubst %, $(BUILD_DIR)/%.ll, $(LIBS))
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

# Пути к QUAKE
QUAKE_DIR   := $(SRC_DIR)/files/progs/Quake
QUAKE_PRG   := $(QUAKE_DIR)/QUAKE.PRG
QUAKE_PAK   := $(QUAKE_DIR)/pak0.pak

# Пути к DESKTOP
DESKTOP_DIR := $(SRC_DIR)/files/progs/Desktop
DESKTOP_PRG := $(DESKTOP_DIR)/desktop.prg
WITH_DESKTOP ?= 1

OVERFILES_DIR := $(SRC_DIR)/files/progs/overFiles
OVERFILES_PRG := $(OVERFILES_DIR)/ovfiles.prg

NOTEPAD_DIR := $(SRC_DIR)/files/progs/notepad
NOTEPAD_PRG := $(NOTEPAD_DIR)/notepad.prg

OVERIMAGES_DIR := $(SRC_DIR)/files/progs/overImages
OVERIMAGES_PRG := $(OVERIMAGES_DIR)/ovimage.prg

OVPLAYER_DIR := $(SRC_DIR)/files/progs/overPlayer
OVPLAYER_PRG := $(OVPLAYER_DIR)/ovplayer.prg

OVTASK_DIR := $(SRC_DIR)/files/progs/overTask
OVTASK_PRG := $(OVTASK_DIR)/ovtask.prg

OVMARK_DIR := $(SRC_DIR)/files/progs/overMark
OVMARK_PRG := $(OVMARK_DIR)/overmark.prg

OVTERM_DIR := $(SRC_DIR)/files/progs/overTerm
OVTERM_PRG := $(OVTERM_DIR)/ovterm.prg

OVSET_DIR := $(SRC_DIR)/files/progs/overSettings
OVSET_PRG := $(OVSET_DIR)/ovsettings.prg

# overCraft: воксельная игра на C++ одной папкой -- ovcraft.prg, свой движок
# ovengine.ll и resources/ с текстурами. В образе -- /desprogs/overCraft/ (меню
# находит .prg в папке)
OVCRAFT_DIR := $(SRC_DIR)/files/progs/overCraft
OVCRAFT_PRG := $(OVCRAFT_DIR)/ovcraft.prg
OVCRAFT_LL  := $(OVCRAFT_DIR)/ovengine.ll
# Ресурсы с подпапками (models/, entities/, items/): образ зависит от каждого файла
OVCRAFT_RES := $(shell find $(OVCRAFT_DIR)/resources -type f 2>/dev/null)

# ==============================================================================
# ИСХОДНЫЕ ФАЙЛЫ ЯДРА
# ==============================================================================
C_SRC   := $(SRC_DIR)/kernel/kernel.c \
           $(SRC_DIR)/kernel/loader_kernel.c \
           $(SRC_DIR)/kernel/main.c \
           $(SRC_DIR)/kernel/cpu.c \
           $(SRC_DIR)/kernel/smp.c \
           $(SRC_DIR)/kernel/tty.c \
           $(SRC_DIR)/kernel/sched.c \
           $(SRC_DIR)/kernel/user_mode.c \
           $(SRC_DIR)/kernel/sys_loader.c \
           $(SRC_DIR)/kernel/prog_loader.c \
           $(SRC_DIR)/kernel/bsod.c \
           $(SRC_DIR)/kernel/keyboard.c \
           $(SRC_DIR)/kernel/sha256.c \
           $(SRC_DIR)/kernel/users.c \
           $(SRC_DIR)/kernel/login.c \
           $(SRC_DIR)/kernel/ipc.c \
           $(SRC_DIR)/kernel/lib_loader.c \
           $(SRC_DIR)/kernel/notify.c \
           $(SRC_DIR)/memory/pmm.c \
           $(SRC_DIR)/memory/vmm.c \
           $(SRC_DIR)/font/fontdata_ru_8x16.c \
           $(SRC_DIR)/drivers/acpi.c \
           $(SRC_DIR)/drivers/display.c \
           $(SRC_DIR)/drivers/pci.c \
           $(SRC_DIR)/drivers/ethernet.c \
		   $(SRC_DIR)/drivers/hda.c \
           $(SRC_DIR)/drivers/mouse.c \
           $(SRC_DIR)/drivers/blk.c \
           $(SRC_DIR)/drivers/usb/usb.c \
           $(SRC_DIR)/drivers/usb/ehci.c \
           $(SRC_DIR)/drivers/usb/ehci-msc.c \
           $(SRC_DIR)/drivers/usb/xhci.c \
           $(SRC_DIR)/drivers/usb/xhci-msc.c \
           $(SRC_DIR)/drivers/usb/xhci-hid.c \
           $(SRC_DIR)/drivers/usb/hid.c \
           $(SRC_DIR)/fs/fat32.c \
           $(SRC_DIR)/fs/mount.c \
           $(SRC_DIR)/fs/vfs.c \
           $(SRC_DIR)/fs/devfs.c \
           $(SRC_DIR)/fs/fs.c

OBJS    := $(BUILD_DIR)/entry_kernel.o \
           $(BUILD_DIR)/switch.o \
           $(BUILD_DIR)/interrupts.o \
           $(BUILD_DIR)/ap_boot.o \
           $(filter %.o, $(C_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o))

# ==============================================================================
# ГЛАВНЫЕ ЦЕЛИ
# ==============================================================================
# compile_commands.json обновляется при каждой сборке: устаревший файл (без
# новых программ и без C++) и был причиной «ошибок» в редакторе при чистой сборке
all: $(DISK_IMG) compile_commands.json

compile_commands.json: tools/gen_compile_commands.py FORCE
	@python3 tools/gen_compile_commands.py >/dev/null

modules: $(MODULE_BINS)

libs: $(LIB_BINS)

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
	@truncate -s 16384 $@

# ------------------------------------------------------------------------------
# Прочие ассемблерные файлы ядра (папка src/asm)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/switch.o: $(SRC_DIR)/asm/switch.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/interrupts.o: $(SRC_DIR)/asm/interrupts.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

# Трамплин рабочих ядер: копируется на 0x8000 и исполняется там (smp.c)
$(BUILD_DIR)/ap_boot.o: $(SRC_DIR)/asm/ap_boot.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

# Обработчики на рабочих ядрах не должны трогать xmm программы
$(BUILD_DIR)/kernel/smp.o: CFLAGS += -mgeneral-regs-only

$(BUILD_DIR)/crt0.o: $(SRC_DIR)/asm/crt0.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

# ------------------------------------------------------------------------------
# Компиляция C-файлов ядра в 64 бита
# ------------------------------------------------------------------------------
# -MMD -MP: компилятор сам записывает, от каких заголовков зависит объектный файл.
# Без этого правка структуры в заголовке (например task_t в sched.h) пересобирала
# только изменённый .c, а остальное ядро оставалось собранным по СТАРОЙ раскладке
# полей — процессы стартовали с мусорными регистрами, и это выглядело как случайный BSOD.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

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

# Файлы зависимостей от -MMD (их нет при первой сборке — minus подавляет ошибку)
-include $(OBJS:.o=.d)
-include $(patsubst %.prg,$(BUILD_DIR)/programs/%.d,$(notdir $(PROG_BINS)))

$(BUILD_DIR)/kernel.bin: $(OBJS) $(BUILD_DIR)/ksyms.o
	$(LD) $(LDFLAGS) -o $@ $^
	@ls -l $@

# ------------------------------------------------------------------------------
# Сборка системных модулей .SYS (Ring 0)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/%.sys: $(SRC_DIR)/modules/%_module.c $(SRC_DIR)/modules/module.ld tools/elf2sys.py
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_MOD) -MMD -MP -c $< -o $(BUILD_DIR)/$*_module.o
	$(LD) $(LDFLAGS_MOD) $(BUILD_DIR)/$*_module.o -o $(BUILD_DIR)/$*.elf
	python3 tools/elf2sys.py $(BUILD_DIR)/$*.elf $@
	@echo ">>> Модуль $@ успешно собран."

# ------------------------------------------------------------------------------
# STRESS.SYS: код для дополнительных ядер собирается из ассемблера, а не
# набирается опкодами в C (см. src/modules/asm/ap_burn.asm)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/ap_burn.h: $(SRC_DIR)/modules/asm/ap_burn.asm tools/bin2h.py
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f bin $< -o $(BUILD_DIR)/ap_burn.bin
	python3 tools/bin2h.py $(BUILD_DIR)/ap_burn.bin $@ ap_trampoline_bin

$(BUILD_DIR)/stress.sys: $(SRC_DIR)/modules/stress_module.c $(SRC_DIR)/modules/module.ld \
                         tools/elf2sys.py $(BUILD_DIR)/ap_burn.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_MOD) -I$(BUILD_DIR) -c $< -o $(BUILD_DIR)/stress_module.o
	$(LD) $(LDFLAGS_MOD) $(BUILD_DIR)/stress_module.o -o $(BUILD_DIR)/stress.elf
	python3 tools/elf2sys.py $(BUILD_DIR)/stress.elf $@
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
# Сборка разделяемых библиотек .LL (Ring 3, грузятся сисколлом 119)
# ------------------------------------------------------------------------------
# Общее правило для библиотеки из нескольких исходников:
# $(1) — имя, $(2) — список .c
define LL_RULE
$(BUILD_DIR)/$(1).ll: $(2) $(wildcard $(LIBS_DIR)/common/*.h) $(wildcard $(LIBS_DIR)/*/*.h) \
                      $(LIBS_DIR)/lib.ld tools/elf2ll.py
	@mkdir -p $(BUILD_DIR)/libs
	@for f in $(2); do \
	    $(CC) $(CFLAGS_LL) -c $$$$f -o $(BUILD_DIR)/libs/$(1)_`basename $$$$f .c`.o || exit 1; \
	done
	$(LD) $(LDFLAGS_LL) $(patsubst %.c,$(BUILD_DIR)/libs/$(1)_%.o,$(notdir $(2))) \
	      -o $(BUILD_DIR)/$(1)_ll.elf
	python3 tools/elf2ll.py $(BUILD_DIR)/$(1)_ll.elf $$@
	@echo ">>> Библиотека $$@ собрана."
endef

$(eval $(call LL_RULE,libc,$(LIBC_SRC)))
$(eval $(call LL_RULE,libm,$(LIBM_SRC)))
$(eval $(call LL_RULE,libgfx,$(LIBGFX_SRC)))
$(eval $(call LL_RULE,libui,$(LIBUI_SRC)))

# ffmpeg.ll собирается скриптом: распаковка исходников FFmpeg, заплаты,
# кросс-конфигурация, сборка и упаковка в .ll (tools/build_ffmpeg.sh)
$(BUILD_DIR)/ffmpeg.ll: $(FFMPEG_SRC) tools/build_ffmpeg.sh tools/elf2ll.py $(LIBS_DIR)/lib.ld
	@mkdir -p $(BUILD_DIR)
	OUT=$@ ./tools/build_ffmpeg.sh
	@echo ">>> Библиотека $@ собрана (настоящий FFmpeg)."


# ------------------------------------------------------------------------------
# Сборка пользовательских программ .PRG (Ring 3)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/%.prg: $(PROG_DIR)/%.c $(BUILD_DIR)/crt0.o user.ld
	@mkdir -p $(BUILD_DIR)/programs
	$(CC) $(CFLAGS_PROG) -MMD -MP -c $< -o $(BUILD_DIR)/programs/$*.o
	$(LD) -m elf_x86_64 -T user.ld -no-pie $(BUILD_DIR)/crt0.o $(BUILD_DIR)/programs/$*.o -o $(BUILD_DIR)/programs/$*.elf
	python3 tools/elf2prg.py $(BUILD_DIR)/programs/$*.elf $@
	@echo ">>> Программа $@ успешно собрана в формате DPRG."

# Автосборка DOOM.PRG через clean + make
$(DOOM_PRG):
	@if [ -d "$(DOOM_DIR)" ]; then \
		echo ">>> Полная пересборка DOOM (clean + make)..."; \
		$(MAKE) -C $(DOOM_DIR) clean && $(MAKE) -C $(DOOM_DIR); \
	fi

# Автосборка QUAKE.PRG через clean + make
$(QUAKE_PRG):
	@if [ -d "$(QUAKE_DIR)" ]; then \
		echo ">>> Полная пересборка QUAKE (clean + make)..."; \
		$(MAKE) -C $(QUAKE_DIR) clean && $(MAKE) -C $(QUAKE_DIR); \
	fi

# Автосборка desktop.prg через clean + make
$(DESKTOP_PRG): FORCE
	@if [ -d "$(DESKTOP_DIR)" ]; then \
		echo ">>> Сборка Desktop Environment..."; \
		$(MAKE) -C $(DESKTOP_DIR); \
	fi

$(OVERFILES_PRG): FORCE
	@if [ -d "$(OVERFILES_DIR)" ]; then \
		$(MAKE) -C $(OVERFILES_DIR); \
	fi

$(NOTEPAD_PRG): FORCE
	@if [ -d "$(NOTEPAD_DIR)" ]; then \
		$(MAKE) -C $(NOTEPAD_DIR); \
	fi

$(OVERIMAGES_PRG): FORCE
	@if [ -d "$(OVERIMAGES_DIR)" ]; then \
		$(MAKE) -C $(OVERIMAGES_DIR); \
	fi

$(OVPLAYER_PRG): FORCE
	@if [ -d "$(OVPLAYER_DIR)" ]; then \
		$(MAKE) -C $(OVPLAYER_DIR); \
	fi

$(OVTASK_PRG): FORCE
	@if [ -d "$(OVTASK_DIR)" ]; then \
		$(MAKE) -C $(OVTASK_DIR); \
	fi

$(OVMARK_PRG): FORCE
	@if [ -d "$(OVMARK_DIR)" ]; then \
		$(MAKE) -C $(OVMARK_DIR); \
	fi

$(OVTERM_PRG): FORCE
	@if [ -d "$(OVTERM_DIR)" ]; then \
		$(MAKE) -C $(OVTERM_DIR); \
	fi

$(OVSET_PRG): FORCE
	@if [ -d "$(OVSET_DIR)" ]; then \
		$(MAKE) -C $(OVSET_DIR); \
	fi

$(OVCRAFT_PRG): FORCE
	@if [ -d "$(OVCRAFT_DIR)" ]; then \
		$(MAKE) -C $(OVCRAFT_DIR); \
	fi
# Библиотеку собирает тот же вызов make в папке игры; пустой рецепт -- чтобы
# make перечитал её время и пересобрал раздел, если она изменилась
$(OVCRAFT_LL): $(OVCRAFT_PRG) ;
# -----------------------------------------------------------------------------
# Создание раздела FAT32 и упаковка софта
# -----------------------------------------------------------------------------
$(PART_IMG): $(UI_FONT_TTF) $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/boot_stage1.bin $(MODULE_BINS) $(LIB_BINS) $(PROG_BINS) $(DOOM_PRG) $(QUAKE_PRG) $(DESKTOP_PRG) $(OVERFILES_PRG) $(NOTEPAD_PRG) $(OVERIMAGES_PRG) $(OVPLAYER_PRG) $(OVTASK_PRG) $(OVMARK_PRG) $(OVTERM_PRG) $(OVSET_PRG) $(OVCRAFT_PRG) $(OVCRAFT_LL) $(OVCRAFT_RES) tools/mkusers.py $(ACCOUNTS_STAMP) $(IMAGE_ASSETS)
	@mkdir -p $(BUILD_DIR)
	@python3 tools/mkusers.py $(BUILD_DIR)/etc "$(ROOT_PASS)" "$(OS_USER)" "$(OS_PASS)"
	@echo ">>> Создаем раздел FAT32 на $(PART_SIZE_MB) МБ..."
	@rm -f $@
	@truncate -s $(PART_SIZE_MB)M $@
	@mkfs.vfat -F 32 -s 8 -n "OVER_OS" $@ >/dev/null
	@if command -v mcopy >/dev/null 2>&1; then \
	    mmd -i $@ ::sys 2>/dev/null || true; \
	    mmd -i $@ ::utils 2>/dev/null || true; \
	    mmd -i $@ ::lib 2>/dev/null || true; \
	    mmd -i $@ ::programs 2>/dev/null || true; \
	    mmd -i $@ ::programs/DOOM 2>/dev/null || true; \
	    mmd -i $@ ::programs/Quake 2>/dev/null || true; \
	    mmd -i $@ ::configs 2>/dev/null || true; \
	    mmd -i $@ ::desprogs 2>/dev/null || true; \
	    mmd -i $@ ::autostart 2>/dev/null || true; \
	    mmd -i $@ ::etc 2>/dev/null || true; \
	    mmd -i $@ ::sys/fonts 2>/dev/null || true; \
	    mmd -i $@ ::sys/icons 2>/dev/null || true; \
	    mmd -i $@ ::root 2>/dev/null || true; \
	    mmd -i $@ ::home 2>/dev/null || true; \
	    mmd -i $@ ::media 2>/dev/null || true; \
	    if [ -n "$(OS_USER)" ]; then mmd -i $@ ::home/$(OS_USER) 2>/dev/null || true; fi; \
	    for f in passwd group shadow perms oobe locale.conf timezone vconsole.conf modules; do \
	        mcopy -i $@ $(BUILD_DIR)/etc/$$f ::etc/$$f; \
	    done; \
	    mcopy -i $@ $(BUILD_DIR)/kernel.bin ::sys/KERNEL.BIN; \
	    mcopy -i $@ $(BUILD_DIR)/login.prg ::sys/login.prg; \
	    mcopy -i $@ $(BUILD_DIR)/sh.prg ::sys/sh.prg; \
	    if [ -f "$(SRC_DIR)/files/image/logo.bmp" ]; then \
	        mcopy -i $@ $(SRC_DIR)/files/image/logo.bmp ::sys/logo.bmp; \
	    fi; \
	    if [ -f "$(SRC_DIR)/files/image/wallpaper.bmp" ]; then \
	        mcopy -i $@ $(SRC_DIR)/files/image/wallpaper.bmp ::sys/wallpaper.bmp; \
	    elif [ -f "wallpaper.bmp" ]; then \
	        mcopy -i $@ wallpaper.bmp ::sys/wallpaper.bmp; \
	    fi; \
	    if [ -f "$(UI_FONT_TTF)" ]; then \
	        mcopy -i $@ $(UI_FONT_TTF) ::sys/fonts/ui.ttf; \
	    fi; \
	    for ico in $(SRC_DIR)/files/icons/*.svg; do \
	        [ -f "$$ico" ] && mcopy -i $@ $$ico ::sys/icons/`basename $$ico`; \
	    done; \
	    for img in $(SRC_DIR)/files/image/*.png; do \
	        [ -f "$$img" ] && mcopy -i $@ $$img ::sys/`basename $$img`; \
	    done; \
	    for lib in $(LIBS); do \
	        if [ -f "$(BUILD_DIR)/$$lib.ll" ]; then \
	            mcopy -i $@ $(BUILD_DIR)/$$lib.ll ::lib/$$lib.ll; \
	        fi; \
	    done; \
	    for mod in $(INSTALL_MODULES); do \
	        if [ -f "$(BUILD_DIR)/$$mod.sys" ]; then \
	            mcopy -i $@ $(BUILD_DIR)/$$mod.sys ::utils/$$mod.sys; \
	        fi; \
	    done; \
	    for prg in $(INSTALL_PROGS); do \
	        if [ -f "$(BUILD_DIR)/$$prg.prg" ]; then \
	            mcopy -i $@ $(BUILD_DIR)/$$prg.prg ::programs/$$prg.prg; \
	        fi; \
	    done; \
	    if [ "$(WITH_DOOM)" = "1" ] && [ -f "$(DOOM_PRG)" ]; then \
	        mcopy -i $@ $(DOOM_PRG) ::programs/DOOM/DOOM.PRG; \
	        if [ -f "$(DOOM_WAD)" ]; then \
	            mcopy -i $@ $(DOOM_WAD) ::programs/DOOM/DOOM1.WAD; \
	        fi; \
	    fi; \
	    if [ "$(WITH_QUAKE)" = "1" ] && [ -f "$(QUAKE_PRG)" ]; then \
	        mcopy -i $@ $(QUAKE_PRG) ::programs/Quake/QUAKE.PRG; \
	        if [ -f "$(QUAKE_PAK)" ]; then \
	            mcopy -i $@ $(QUAKE_PAK) ::programs/Quake/pak0.pak; \
	        fi; \
	    fi; \
	    if [ "$(WITH_DESKTOP)" = "1" ] && [ -f "$(DESKTOP_PRG)" ]; then \
	        mcopy -i $@ $(DESKTOP_PRG) ::autostart/desktop.prg; \
	    fi; \
	    if [ -f "$(OVERFILES_PRG)" ]; then \
	        mcopy -i $@ $(OVERFILES_PRG) ::desprogs/ovfiles.prg; \
	    fi; \
		if [ -f "$(NOTEPAD_PRG)" ]; then \
	        mcopy -i $@ $(NOTEPAD_PRG) ::desprogs/notepad.prg; \
	    fi; \
		if [ -f "$(OVERIMAGES_PRG)" ]; then \
			mcopy -i $@ $(OVERIMAGES_PRG) ::desprogs/ovimage.prg; \
	    fi; \
		if [ -f "$(OVPLAYER_PRG)" ]; then \
	    	mcopy -i $@ $(OVPLAYER_PRG) ::desprogs/ovplayer.prg; \
		fi; \
		if [ -f "$(OVTASK_PRG)" ]; then \
	    	mcopy -i $@ $(OVTASK_PRG) ::desprogs/ovtask.prg; \
		fi; \
		if [ -f "$(OVMARK_PRG)" ]; then \
	    	mcopy -i $@ $(OVMARK_PRG) ::desprogs/overmark.prg; \
		fi; \
		if [ -f "$(OVTERM_PRG)" ]; then \
	    	mcopy -i $@ $(OVTERM_PRG) ::desprogs/ovterm.prg; \
		fi; \
		if [ -f "$(OVSET_PRG)" ]; then \
	    	mcopy -i $@ $(OVSET_PRG) ::desprogs/ovsettings.prg; \
		fi; \
		if [ -f "$(OVCRAFT_PRG)" ] && [ -f "$(OVCRAFT_LL)" ]; then \
	    	mmd -i $@ ::desprogs/overCraft; \
	    	mcopy -i $@ $(OVCRAFT_PRG) ::desprogs/overCraft/ovcraft.prg; \
	    	mcopy -i $@ $(OVCRAFT_LL) ::desprogs/overCraft/ovengine.ll; \
	    	mcopy -s -i $@ $(OVCRAFT_DIR)/resources ::desprogs/overCraft/; \
		fi; \
		for snd in $(SRC_DIR)/files/sound/*.mp3; do \
		    [ -f "$$snd" ] && mcopy -i $@ $$snd ::`basename $$snd`; \
		done; \
		for vid in $(SRC_DIR)/files/video/*.mp4; do \
		    [ -f "$$vid" ] || continue; \
		    nm=`basename "$$vid" | tr -d '()' | tr ' ' '_'`; \
		    mcopy -i $@ "$$vid" "::$$nm"; \
		done; \
		if [ -f "speech.wav" ]; then \
	    	mcopy -i $@ speech.wav ::speech.wav; \
		elif [ -f "$(SRC_DIR)/files/sound/speech.wav" ]; then \
		    mcopy -i $@ $(SRC_DIR)/files/sound/speech.wav ::speech.wav; \
		fi; \
	    printf '{\n  "timezone": 3,\n  "langlayout": 1,\n  "brightness": 100,\n  "wallpaper": "/sys/wallpaper.bmp"\n}\n' > $(BUILD_DIR)/desktop.json; \
	    mcopy -i $@ $(BUILD_DIR)/desktop.json ::configs/desktop.json; \
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
	@# 4. Прописываем стандартную таблицу MBR (Partition 1: Boot RAW, Partition 2: FAT32 с LBA 2048)
	@python3 -c "import struct; f=open('$@','r+b'); \
	    f.seek(446); \
	    p1 = struct.pack('<BBBBBBBBII', 0x80, 0x00, 0x02, 0x00, 0x7F, 0x20, 0x20, 0x00, 1, 2047); \
	    p2 = struct.pack('<BBBBBBBBII', 0x00, 0x20, 0x21, 0x00, 0x0C, 0xFE, 0xFF, 0xFF, 2048, $(PART_SIZE_MB)*2048); \
	    f.write(p1 + p2 + b'\x00'*32 + b'\x55\xAA')"
	@# 5. Дописываем раздел FAT32 (начиная ровно с 1 МБ / LBA 2048)
	@cat $(PART_IMG) >> $@
	@echo ">>> Готово: $(DISK_IMG) (overOS Two-Stage FAT32) успешно собран."

# ------------------------------------------------------------------------------
# Запуск в QEMU x86_64
# ------------------------------------------------------------------------------
run: all
	GDK_BACKEND=x11 qemu-system-x86_64 \
		-m 4G \
		-machine accel=kvm:tcg \
		-cpu max \
		-serial stdio \
		-d cpu_reset,guest_errors \
		-D qemu_error.log \
		-no-reboot \
		-no-shutdown \
		-device usb-ehci,id=ehci \
		-drive if=none,id=usb_drive,file=$(DISK_IMG),format=raw \
		-device usb-storage,bus=ehci.0,drive=usb_drive \
		-audiodev pa,id=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-vga std -global VGA.vgamem_mb=16

run-debug: all
	GDK_BACKEND=x11 qemu-system-x86_64 \
		-m 4G \
		-accel tcg \
		-cpu max \
		-serial stdio \
		-d int,cpu_reset,guest_errors \
		-D qemu_error.log \
		-no-reboot \
		-no-shutdown \
		-device usb-ehci,id=ehci \
		-drive if=none,id=usb_drive,file=$(DISK_IMG),format=raw \
		-device usb-storage,bus=ehci.0,drive=usb_drive \
		-audiodev pa,id=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-vga std -global VGA.vgamem_mb=16

run-usb: all
	GDK_BACKEND=x11 qemu-system-x86_64 \
		-m 4G \
		-cpu max \
		-serial stdio \
		-d int,cpu_reset,guest_errors \
		-D qemu_error.log \
		-no-reboot \
		-no-shutdown \
		-device usb-ehci,id=ehci \
		-drive if=none,id=usb_drive,file=/dev/sdc,format=raw \
		-device usb-storage,bus=ehci.0,drive=usb_drive \
		-vga std -global VGA.vgamem_mb=16

# ------------------------------------------------------------------------------
# Быстрое обновление модулей и программ на уже размеченной флешке
# ------------------------------------------------------------------------------
install-modules: $(MODULE_BINS) $(LIB_BINS) $(PROG_BINS)
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
	@if [ -f "$(SRC_DIR)/files/image/wallpaper.bmp" ]; then \
	    sudo mcopy -o -i $(DEV)2 $(SRC_DIR)/files/image/wallpaper.bmp ::sys/wallpaper.bmp; \
	elif [ -f "wallpaper.bmp" ]; then \
	    sudo mcopy -o -i $(DEV)2 wallpaper.bmp ::sys/wallpaper.bmp; \
	fi
	sync
	@echo ">>> Все модули, программы и обои успешно обновлены на $(DEV)2"

# ------------------------------------------------------------------------------
# Центр управления сборкой и установкой overOS
# ------------------------------------------------------------------------------
install:
	@sudo python3 tools/installer_gui.py

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
	@if [ -d "$(DESKTOP_DIR)" ]; then $(MAKE) -C $(DESKTOP_DIR) clean; fi

# Программы desktop собираются своими makefile: вызываем их всегда, а они сами решают,
# что пересобрать (раньше правка исходников не попадала в образ, пока .prg уже существовал)
FORCE:

.PHONY: FORCE compile-commands all modules progs run run-debug install install-modules clean run-usb _write_disk