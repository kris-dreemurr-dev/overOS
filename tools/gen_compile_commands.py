#!/usr/bin/env python3
"""Генерация compile_commands.json для редактора (clangd, VS Code C/C++).

Сборкой это не пользуется: файл нужен только подсветке и переходу по коду,
чтобы редактор знал про -I, флаги freestanding и язык файла (C или C++) и не
подчёркивал то, что компилятор собирает без единого слова.

Флаги здесь повторяют Makefile и src/libs/common/app.mk. Каталоги ищутся
сами: новая программа в src/files/progs или библиотека в src/libs попадают
сюда без правки этого файла. Запуск: make compile-commands
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBS = os.path.join(ROOT, "src/libs")
LIB_INC = ["-I" + LIBS, "-I" + os.path.join(LIBS, "common")]

BASE = ["-m64", "-ffreestanding", "-fno-stack-protector", "-mno-red-zone", "-O2", "-Wall", "-Wextra"]
KERNEL = ["-std=c99"] + BASE + ["-fno-pic", "-fno-pie", "-mcmodel=kernel", "-U_FORTIFY_SOURCE",
                                "-fno-strict-aliasing"] + ["-I" + os.path.join(LIBS, "common")]
MODULE = ["-std=c99"] + BASE + ["-fPIE", "-fvisibility=hidden", "-mgeneral-regs-only", "-mno-sse"]
PROG = ["-std=c99"] + BASE + ["-fno-pic", "-fno-pie", "-mcmodel=large", "-fno-builtin"] + LIB_INC
LL_C = ["-std=c99"] + BASE + ["-fPIE", "-fvisibility=hidden", "-fno-plt", "-fno-builtin"] + LIB_INC
CXX_COMMON = ["-std=c++17", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
              "-fno-asynchronous-unwind-tables", "-fno-math-errno"]
PROG_CXX = CXX_COMMON + BASE + ["-fno-pic", "-fno-pie", "-mcmodel=large"] + LIB_INC
LL_CXX = CXX_COMMON + BASE + ["-fPIE", "-fvisibility=hidden", "-fno-plt",
                              "-DOVENGINE_BUILD", "-DCXXRT_ARENA_KB=16384"] + LIB_INC

# Ядро: заголовки лежат рядом с исходниками
KERNEL_DIRS = ["src/kernel", "src/drivers", "src/drivers/usb", "src/fs", "src/memory", "src/font"]
# Каталоги, которые не собираются (Wi-Fi на паузе, см. src/drivers/wifi/readme.md)
SKIP_DIRS = {"src/drivers/wifi"}
# Сторонний код со своими сборками
SKIP_PROGS = {"DOOM", "Quake"}


def entries():
    out = []

    def add(path, flags):
        cpp = path.endswith(".cpp")
        out.append({
            "directory": ROOT,
            "file": path,
            "arguments": ["g++" if cpp else "gcc"] + flags + ["-c", path],
        })

    def sources(d, recursive=False):
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            return
        walk = os.walk(full) if recursive else [(full, [], os.listdir(full))]
        for base, dirs, files in walk:
            dirs[:] = [x for x in dirs if x not in ("build", "dist", "include", "patches")]
            for name in sorted(files):
                if name.endswith(".c") or name.endswith(".cpp"):
                    yield os.path.join(base, name)

    for d in KERNEL_DIRS:
        if d in SKIP_DIRS:
            continue
        for f in sources(d):
            add(f, KERNEL)

    for f in sources("src/modules", recursive=True):
        add(f, MODULE)

    inc = ["-I" + os.path.join(ROOT, "src/drivers"), "-I" + os.path.join(ROOT, "src/kernel")]
    for f in sources("src/programs"):
        add(f, PROG + inc)

    # Библиотеки src/libs: .c собирается как .ll, .cpp — как рантайм программы
    for name in sorted(os.listdir(LIBS)):
        if name == "ffmpeg" or not os.path.isdir(os.path.join(LIBS, name)):
            continue
        for f in sources("src/libs/" + name, recursive=True):
            add(f, PROG_CXX if f.endswith(".cpp") else LL_C)

    # Программы рабочего стола. Внутри папки программы engine/ — её своя .ll
    progs = os.path.join(ROOT, "src/files/progs")
    for app in sorted(os.listdir(progs)):
        full = os.path.join(progs, app)
        if app in SKIP_PROGS or not os.path.isdir(full):
            continue
        app_inc = ["-I" + full]
        for f in sources("src/files/progs/" + app, recursive=True):
            in_ll = os.sep + "engine" + os.sep in f
            if f.endswith(".cpp"):
                add(f, (LL_CXX if in_ll else PROG_CXX) + app_inc)
            else:
                add(f, (LL_C if in_ll else PROG) + app_inc)

    # Загрузчик: 16-битный код, но редактору достаточно обычных флагов
    for f in sources("src/boot"):
        add(f, ["-std=gnu11", "-ffreestanding"])

    return out


db = entries()
with open(os.path.join(ROOT, "compile_commands.json"), "w") as f:
    json.dump(db, f, indent=1)
    f.write("\n")
print(f"compile_commands.json: {len(db)} файлов")
