# =============================================================================
# Общая сборка оконного приложения overOS на libui.
# Makefile приложения задаёт и подключает этот файл:
#   TARGET   := notepad.prg            # итоговый бинарник
#   SRCS     := notepad.c              # свои исходники
#   LDSCRIPT := notepad.ld
#   APP_NAME := Блокнот                # имя в меню, на панели и в уведомлениях
#   APP_ID   := notepad                # класс программы (тот же, что в ui_app_id)
#   include ../../../libs/common/app.mk
#
# Исходники на C++ (.cpp) ставятся в SRCS наравне с .c: собираются g++ без
# стандартной библиотеки (без исключений и RTTI), а рантайм -- new/delete,
# куча, глобальные конструкторы -- подкладывается из src/libs/cxxrt сам.
# CXXLIBSRCS -- .cpp из src/libs, путь от src/libs (собираются внутрь .prg).
#
# Своя библиотека программы (.ll, лежит в её папке рядом с .prg):
#   LL_TARGET := ovengine.ll
#   LL_SRCS   := engine/api.cpp engine/renderer.cpp    # .c и .cpp
#   LL_ARENA_KB := 16384                                # куча библиотеки
# Библиотека получает свой cxxrt со статической ареной: brk принадлежит
# программе, и куча .ll на нём сломала бы кучу игры.
#
# Значок программы -- icon.svg рядом с makefile: он вшивается в сам .prg
# (ресурс ICON) вместе с именем и классом, и система берёт его оттуда --
# для окна, панели задач, меню, уведомлений и значка файла в overFiles.
#
# Заголовки подключения к разделяемым библиотекам лежат рядом с самими
# библиотеками: #include "libui/libui_ll.h", "ffmpeg/ffmpeg_ll.h" и т.д.
# =============================================================================

LIBS_DIR  := ../../../libs
TOOLS_DIR := ../../../../tools

CC        := gcc
LD        := ld
NASM      := nasm
PYTHON    := python3
BUILD_DIR := build
ELF       := $(TARGET:.prg=.elf)

CFLAGS    := -m64 -std=c99 -O2 -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fno-builtin -mno-red-zone \
             -mcmodel=large -Wall -Wextra -I. -I$(LIBS_DIR) -I$(LIBS_DIR)/common
CXX       := g++
CXXFLAGS  := -m64 -std=c++17 -O2 -ffreestanding -fno-exceptions -fno-rtti \
             -fno-threadsafe-statics -fno-asynchronous-unwind-tables -fno-math-errno \
             -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=large \
             -Wall -Wextra -I. -I$(LIBS_DIR) -I$(LIBS_DIR)/common
LDFLAGS   := -m elf_x86_64 -T $(LDSCRIPT) -no-pie
LIBGCC    := $(shell $(CC) -m64 -print-libgcc-file-name)

LIB_HDRS  := $(wildcard $(LIBS_DIR)/common/*.h) $(wildcard $(LIBS_DIR)/*/*.h)
# LIBSRCS — файлы из src/libs/libgfx, которые собираются вместе с приложением (ttf.c, svg.c)
LIBSRCS   ?=
# USE_LIBUI := 1 — тулкит берётся из /lib/libui.ll в runtime, а не линкуется
USE_LIBUI ?=
UI_OBJ    := $(if $(USE_LIBUI),,$(BUILD_DIR)/ui.o)
C_SRCS    := $(filter %.c,$(SRCS))
CXX_SRCS  := $(filter %.cpp,$(SRCS))
CXXLIBSRCS ?=
# Рантайм C++ нужен, только если в программе есть C++
CXXRT_OBJ := $(if $(CXX_SRCS)$(CXXLIBSRCS),$(BUILD_DIR)/libs/cxxrt/cxxrt.o)
OBJS      := $(BUILD_DIR)/crt0.o $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
             $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CXX_SRCS)) \
             $(patsubst %.c,$(BUILD_DIR)/lib_%.o,$(LIBSRCS)) \
             $(patsubst %.cpp,$(BUILD_DIR)/libs/%.o,$(CXXLIBSRCS)) $(CXXRT_OBJ) $(UI_OBJ)

# --- своя библиотека программы
LL_TARGET   ?=
LL_SRCS     ?=
LL_ARENA_KB ?= 8192
LL_DIR      := $(BUILD_DIR)/ll
LL_ELF      := $(LL_DIR)/$(LL_TARGET:.ll=.elf)
LL_FLAGS    := -ffreestanding -fPIE -fvisibility=hidden -fno-plt -fno-stack-protector \
               -mno-red-zone -O2 -Wall -Wextra -I. -I$(LIBS_DIR) -I$(LIBS_DIR)/common
LL_CFLAGS   := -m64 -std=c99 -fno-builtin $(LL_FLAGS)
LL_CXXFLAGS := -m64 -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
               -fno-asynchronous-unwind-tables -fno-math-errno $(LL_FLAGS) \
               -DOVENGINE_BUILD -DCXXRT_ARENA_KB=$(LL_ARENA_KB)
LL_OBJS     := $(patsubst %.c,$(LL_DIR)/%.o,$(filter %.c,$(LL_SRCS))) \
               $(patsubst %.cpp,$(LL_DIR)/%.o,$(filter %.cpp,$(LL_SRCS))) \
               $(if $(filter %.cpp,$(LL_SRCS)),$(LL_DIR)/cxxrt.o)

all: $(TARGET) $(LL_TARGET)

ifneq ($(LL_TARGET),)
$(LL_DIR)/%.o: %.c $(LIB_HDRS) $(wildcard *.h */*.h)
	@mkdir -p $(dir $@)
	$(CC) $(LL_CFLAGS) -MMD -MP -c $< -o $@

$(LL_DIR)/%.o: %.cpp $(LIB_HDRS) $(wildcard *.h */*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(LL_CXXFLAGS) -MMD -MP -c $< -o $@

# Размер кучи (LL_ARENA_KB) задаёт makefile программы: поменяли — пересобрать.
# Без этой зависимости новый размер молча не попадал в .ll
$(LL_DIR)/cxxrt.o: $(LIBS_DIR)/cxxrt/cxxrt.cpp $(LIB_HDRS) $(firstword $(MAKEFILE_LIST))
	@mkdir -p $(dir $@)
	$(CXX) $(LL_CXXFLAGS) -c $< -o $@

# --no-relax: иначе компоновщик превращает обращения через GOT в абсолютные
# 32-битные адреса, а библиотека живёт выше 4 ГБ (см. src/libs/readme.md)
$(LL_TARGET): $(LL_OBJS) $(LIBS_DIR)/lib.ld $(TOOLS_DIR)/elf2ll.py
	$(LD) -m elf_x86_64 -T $(LIBS_DIR)/lib.ld -z max-page-size=4096 --emit-relocs -no-pie --no-relax \
	      $(LL_OBJS) -o $(LL_ELF)
	@if readelf -S $(LL_ELF) | grep -q '\.init_array'; then \
	    echo "!!! $(LL_TARGET): глобальные объекты с конструкторами в .ll не поддержаны"; exit 1; fi
	$(PYTHON) $(TOOLS_DIR)/elf2ll.py $(LL_ELF) $@
	@echo ">>> $(LL_TARGET) собрана"
endif

$(BUILD_DIR)/crt0.o: ../../../asm/crt0.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/ui.o: $(LIBS_DIR)/libui/ui.c $(LIB_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c $(LIB_HDRS) $(wildcard *.h */*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp $(LIB_HDRS) $(wildcard *.h */*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/libs/%.o: $(LIBS_DIR)/%.cpp $(LIB_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/lib_%.o: $(LIBS_DIR)/libgfx/%.c $(LIB_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/*/*.d $(BUILD_DIR)/libs/*/*.d $(BUILD_DIR)/ll/*/*.d)

$(ELF): $(OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBGCC) -o $@

APP_ICON  := $(wildcard icon.svg)
RES_ARGS  := $(if $(APP_ICON),--icon $(APP_ICON)) $(if $(APP_NAME),--name "$(APP_NAME)") \
             $(if $(APP_ID),--app $(APP_ID))

$(TARGET): $(ELF) $(APP_ICON) makefile
	$(PYTHON) $(TOOLS_DIR)/elf2prg.py $(ELF) $(TARGET) --desktop $(RES_ARGS)
	@echo ">>> $(TARGET) собран (libui, --desktop)"

clean:
	rm -rf $(BUILD_DIR) $(ELF) $(TARGET) $(LL_TARGET)

.PHONY: all clean
