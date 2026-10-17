#!/usr/bin/env python3
"""elf2ll.py: ELF (линковка с базой 0, PIE, --emit-relocs) -> .ll (linked library)

.ll — разделяемая библиотека overOS, аналог .so/.dll для программ Ring 3.
Программа грузит её сисколлом 119 (ov_lib_open), ищет символ по имени (120) и
вызывает его как обычную функцию. Библиотека перемещаемая: ядро кладёт её по
свободному адресу в адресном пространстве процесса и чинит релокации.

Формат файла (всё little-endian):
    [0]                   заголовок ll_header_t (80 байт)
    [image_off]           образ: .text .rodata .data (без .bss), image_size байт
    [reloc_off]           reloc_count * uint32 — смещения 64-битных ячеек,
                          к которым загрузчик прибавляет базу
    [sym_off]             sym_count * ll_sym_t (name_off:u32, flags:u32, value:u64)
    [str_off]             строки имён, разделённые нулями

Правила для исходника библиотеки:
  * собирается с -fPIE -fvisibility=hidden -fno-plt (как .sys), линкуется
    module-подобным скриптом с базой 0;
  * ни одного неразрешённого символа: библиотека не имеет импорта, всё, что ей
    нужно от системы, делается через int 0x80 (ovos.h — заголовочный, inline);
  * экспортируется каждый глобальный символ (T/D/B/R в nm -g --defined-only).
"""
import sys, struct, subprocess, os

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <input.elf> <output.ll>")
    sys.exit(1)

elf_path, ll_path = sys.argv[1], sys.argv[2]

def die(msg):
    print(f"[elf2ll] ОШИБКА: {msg}", file=sys.stderr)
    sys.exit(1)

# 1. Размеры сегмента LOAD
out = subprocess.check_output(["readelf", "-W", "-l", elf_path]).decode()
file_size = mem_size = None
for line in out.splitlines():
    if " LOAD " in line:
        p = line.split()
        if int(p[2], 16) != 0:
            die("библиотека должна быть слинкована с базой 0 (в lib.ld: . = 0;)")
        file_size, mem_size = int(p[4], 16), int(p[5], 16)
        break
if file_size is None:
    die("нет LOAD-сегмента")

# 2. GOT: её ячейки --emit-relocs не покрывает, но каждая из них — абсолютный
# адрес внутри образа (линковка с базой 0), поэтому загрузчику достаточно
# прибавить к ней базу, как к обычной R_X86_64_64. Собираем их смещения сами.
# Так собирается настоящий FFmpeg: его код обращается к таблицам через GOT, и
# без этого библиотеку пришлось бы линковать с --relax, который в не-PIE
# превращает такие обращения в абсолютные 32-битные (R_X86_64_32S) — а они с
# базой 0x6000_0000_0000 попросту не представимы.
got_cells = []
secs_raw = subprocess.check_output(["readelf", "-W", "-S", elf_path]).decode()
for line in secs_raw.splitlines():
    line = line.strip()
    if "] ." not in line:
        continue
    p = line.split("]", 1)[1].split()
    if len(p) < 5 or p[0] not in (".got", ".got.plt"):
        continue
    addr, off, size = int(p[2], 16), int(p[3], 16), int(p[4], 16)
    got_cells.append((p[0], addr, off, size))

# 3. Релокации
relocs, bad = [], []
for line in subprocess.check_output(["readelf", "-W", "-r", elf_path]).decode().splitlines():
    p = line.split()
    if len(p) < 3 or not p[2].startswith("R_X86_64_"):
        continue
    off, typ = int(p[0], 16), p[2]
    if typ in ("R_X86_64_64", "R_X86_64_RELATIVE"):
        relocs.append(off)
    elif typ in ("R_X86_64_PC32", "R_X86_64_PLT32", "R_X86_64_PC64", "R_X86_64_NONE",
                 "R_X86_64_GOTPCREL", "R_X86_64_GOTPCRELX", "R_X86_64_REX_GOTPCRELX",
                 "R_X86_64_GOTPCRELX64", "R_X86_64_CODE_4_GOTPCRELX",
                 "R_X86_64_CODE_5_GOTPCRELX", "R_X86_64_CODE_6_GOTPCRELX"):
        pass                                   # позиционно-независимые (GOT — ниже)
    else:
        bad.append(f"{typ} @0x{off:x}")
if bad:
    die("неподдерживаемые релокации: " + ", ".join(bad[:8]))
relocs = sorted(set(relocs))

# 4. Неразрешённых символов быть не должно
undef = []
for line in subprocess.check_output(["nm", "-u", elf_path]).decode().splitlines():
    name = line.split()[-1]
    if name:
        undef.append(name)
if undef:
    die("в библиотеке есть импорт, которого формат не поддерживает: " + ", ".join(undef[:8]))

# 5. Экспорт: все глобальные определённые символы
syms = []
for line in subprocess.check_output(["nm", "-g", "--defined-only", elf_path]).decode().splitlines():
    p = line.split()
    if len(p) != 3:
        continue
    value, kind, name = int(p[0], 16), p[1], p[2]
    if kind in ("T", "t"):   flags = 1        # функция
    elif kind in ("D", "d", "B", "b"): flags = 2   # данные
    elif kind in ("R", "r"): flags = 3        # константы
    else:                    continue
    if name.startswith("_"):                  # служебные (_edata, _end, __bss_start)
        continue
    syms.append((name, flags, value))
if not syms:
    die("библиотека ничего не экспортирует")
syms.sort(key=lambda s: s[0])

# 6. Образ
tmp = ll_path + ".payload.tmp"
subprocess.run(["objcopy", "-O", "binary", elf_path, tmp], check=True)
with open(tmp, "rb") as f:
    payload = f.read()
os.remove(tmp)
if len(payload) > mem_size:
    die(f"образ {len(payload)} больше MemSiz {mem_size}")
# Ненулевые ячейки GOT — такие же адреса внутри образа: правим их базой.
got_added = 0
for name, addr, off, size in got_cells:
    if addr + size > len(payload):
        die(f"{name} вне файловой части образа — проверь порядок секций в lib.ld")
    for c in range(addr, addr + size, 8):
        if int.from_bytes(payload[c:c + 8], "little") != 0:
            relocs.append(c)
            got_added += 1
relocs = sorted(set(relocs))

for o in relocs:
    if o + 8 > len(payload):
        die(f"релокация 0x{o:x} вне файловой части образа (указатель в .bss?)")

HDR = 80
image_off = HDR
reloc_off = image_off + len(payload)
reloc_off += (-reloc_off) % 8
sym_off = reloc_off + 4 * len(relocs)
sym_off += (-sym_off) % 8

strtab = bytearray()
sym_blob = bytearray()
for name, flags, value in syms:
    name_off = len(strtab)
    strtab += name.encode() + b"\0"
    sym_blob += struct.pack("<IIQ", name_off, flags, value)
str_off = sym_off + len(sym_blob)

header = struct.pack("<4sIIIQQQQQQQQ",
                     b"DLIB", 1, HDR, len(relocs),
                     len(payload), mem_size,
                     reloc_off, sym_off, len(syms),
                     str_off, len(strtab), 0)
header += b"\0" * (HDR - len(header))

with open(ll_path, "wb") as f:
    f.write(header)
    f.write(payload)
    f.write(b"\0" * (reloc_off - image_off - len(payload)))
    f.write(b"".join(struct.pack("<I", o) for o in relocs))
    f.write(b"\0" * (sym_off - reloc_off - 4 * len(relocs)))
    f.write(sym_blob)
    f.write(strtab)

print(f"[overOS LL] {ll_path}: образ {len(payload)} Б, память {mem_size} Б, "
      f"релокаций {len(relocs)} (из них GOT {got_added}), экспорт {len(syms)}")
