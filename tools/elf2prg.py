#!/usr/bin/env python3
import sys
import struct
import subprocess
import os

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <input.elf> <output.prg> [--desktop]")
    sys.exit(1)

elf_path = sys.argv[1]
prg_path = sys.argv[2]
is_desktop = 1 if "--desktop" in sys.argv else 0

# 1. Извлекаем ВСЕ сегменты LOAD через readelf
out = subprocess.check_output(["readelf", "-W", "-l", elf_path]).decode("utf-8")

entry_point = 0
min_vaddr = None
max_mem_vaddr = 0

for line in out.splitlines():
    if "Entry point" in line:
        entry_point = int(line.split()[-1], 16)
    if "LOAD" in line:
        parts = line.split()
        if len(parts) >= 6:
            vaddr = int(parts[2], 16)
            msize = int(parts[5], 16)
            if min_vaddr is None or vaddr < min_vaddr:
                min_vaddr = vaddr
            if vaddr + msize > max_mem_vaddr:
                max_mem_vaddr = vaddr + msize

load_vaddr = min_vaddr if min_vaddr is not None else 0x4000000000

# 2. Извлекаем бинарный образ (код + данные)
tmp_bin = prg_path + ".payload.tmp"
subprocess.run(["objcopy", "-O", "binary", elf_path, tmp_bin], check=True)

with open(tmp_bin, "rb") as f:
    payload = f.read()

if os.path.exists(tmp_bin):
    os.remove(tmp_bin)

code_size = len(payload)

# Реальный размер BSS — расстояние от конца бинарника до конца адресного пространства
total_mem = max_mem_vaddr - load_vaddr
bss_size = max(0, total_mem - code_size)

# Страховочный запас под кучу/BSS (минимум 64 КБ)
if bss_size < 64 * 1024:
    bss_size = 64 * 1024

# 3. Упаковываем 64-байтный заголовок DPRG
header = struct.pack(
    "<4sIQQQQQQQ",
    b"DPRG",
    1,
    load_vaddr,
    entry_point,
    code_size,
    bss_size,
    512 * 1024,
    is_desktop,
    0
)

with open(prg_path, "wb") as f:
    f.write(header)
    f.write(payload)

print(f"[devOS DPRG] {prg_path} успешно собран:")
print(f"  * Базовый адрес: 0x{load_vaddr:016X}")
print(f"  * Точка входа:   0x{entry_point:016X}")
print(f"  * Размер кода:   {code_size} байт")
print(f"  * Размер BSS:    {bss_size} байт (выделено страниц: {(code_size + bss_size + 4095)//4096})")