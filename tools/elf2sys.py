#!/usr/bin/env python3
"""elf2sys.py: ELF (линковка с базой 0, --emit-relocs) -> перемещаемый devOS .SYS

Формат файла:
    [образ: image_size байт]      .header .text .rodata .data (без .bss)
    [pad до 8 байт]
    [reloc_count * uint32]        смещения 64-битных ячеек, к которым загрузчик прибавляет базу
    [трейлер 20 байт]             '<4sIIII': 'SREL', image_size, mem_size, reloc_off, reloc_count
mem_size = образ + .bss. Загрузчик берёт трейлер с конца файла.
"""
import sys, struct, subprocess, os

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <input.elf> <output.sys>")
    sys.exit(1)

elf_path, sys_path = sys.argv[1], sys.argv[2]

def die(msg):
    print(f"[elf2sys] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)

# 1. Размеры из program header (LOAD)
out = subprocess.check_output(["readelf", "-W", "-l", elf_path]).decode()
file_size = mem_size = None
for line in out.splitlines():
    if " LOAD " in line:
        p = line.split()
        if int(p[2], 16) != 0:
            die("модуль должен быть слинкован с базой 0 (в module.ld: . = 0;)")
        file_size, mem_size = int(p[4], 16), int(p[5], 16)
        break
if file_size is None:
    die("нет LOAD-сегмента")

# 2. Нет ли GOT (её ячейки не покрываются --emit-relocs)
secs = subprocess.check_output(["readelf", "-W", "-S", elf_path]).decode()
if ".got" in secs:
    die("в образе есть .got — компилируй модули с -fPIE -fvisibility=hidden -fno-plt")

# 3. Релокации
relocs = []
bad = []
for line in subprocess.check_output(["readelf", "-W", "-r", elf_path]).decode().splitlines():
    p = line.split()
    if len(p) < 3 or not p[2].startswith("R_X86_64_"):
        continue
    off, typ = int(p[0], 16), p[2]
    if typ == "R_X86_64_64":
        relocs.append(off)
    elif typ in ("R_X86_64_PC32", "R_X86_64_PLT32", "R_X86_64_PC64"):
        pass                                  # позиционно-независимые
    else:
        bad.append(f"{typ} @0x{off:x}")
if bad:
    die("неподдерживаемые релокации (нужен -fPIE, без -mcmodel=kernel): " + ", ".join(bad[:8]))
relocs = sorted(set(relocs))

# 4. Образ
tmp = sys_path + ".payload.tmp"
subprocess.run(["objcopy", "-O", "binary", elf_path, tmp], check=True)
with open(tmp, "rb") as f:
    payload = f.read()
os.remove(tmp)
# objcopy отбрасывает хвостовое выравнивание перед .bss, поэтому образ может быть чуть короче FileSiz.
# Загрузчик зануляет всё от image_size до mem_size, так что разница попадает в обнуляемую область.
if len(payload) > file_size or len(payload) > mem_size:
    die(f"размер образа {len(payload)} не согласуется с FileSiz {file_size} / MemSiz {mem_size}")
for o in relocs:
    if o + 8 > len(payload):
        die(f"релокация 0x{o:x} вне файловой части образа (указатель в .bss?)")

pad = (-len(payload)) % 8
reloc_off = len(payload) + pad
blob = payload + b"\0" * pad + b"".join(struct.pack("<I", o) for o in relocs)
blob += struct.pack("<4sIIII", b"SREL", len(payload), mem_size, reloc_off, len(relocs))

with open(sys_path, "wb") as f:
    f.write(blob)

print(f"[devOS SYS] {sys_path}: image={len(payload)}B mem={mem_size}B relocs={len(relocs)}")