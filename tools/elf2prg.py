#!/usr/bin/env python3
import sys
import struct
import subprocess
import os

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <input.elf> <output.prg>")
    sys.exit(1)

elf_path = sys.argv[1]
prg_path = sys.argv[2]

# 1. Извлекаем сегменты через readelf с флагом -W (без переноса строк)
out = subprocess.check_output(["readelf", "-W", "-l", elf_path]).decode("utf-8")

entry_point = 0
load_vaddr = 0
file_size = 0
mem_size = 0

for line in out.splitlines():
    if "Entry point" in line:
        entry_point = int(line.split()[-1], 16)
    if "LOAD" in line:
        parts = line.split()
        if len(parts) >= 6:
            load_vaddr = int(parts[2], 16)
            file_size = int(parts[4], 16)
            mem_size = int(parts[5], 16)
            break

# Страховочный базовый адрес devOS (256 ГБ)
if load_vaddr == 0:
    load_vaddr = 0x4000000000

bss_size = max(0, mem_size - file_size)

# 2. Извлекаем тело кода и инициализированных данных
tmp_bin = prg_path + ".payload.tmp"
subprocess.run(["objcopy", "-O", "binary", elf_path, tmp_bin], check=True)

with open(tmp_bin, "rb") as f:
    payload = f.read()

if os.path.exists(tmp_bin):
    os.remove(tmp_bin)

# ==============================================================================
# Структура devos_prg_header_t (64 байта):
# char     magic[4]    -> 4s ("DPRG")
# uint32_t version     -> I  (1)
# uint64_t load_vaddr  -> Q  (0x4000000000)
# uint64_t entry_point -> Q  (_start)
# uint64_t code_size   -> Q  (len(payload))
# uint64_t bss_size    -> Q  (mem_size - file_size)
# uint64_t stack_size  -> Q  (512 КБ = 524288)
# uint64_t reserved[2] -> Q, Q (0, 0)
# ==============================================================================
header = struct.pack(
    "<4sIQQQQQQQ",
    b"DPRG",
    1,
    load_vaddr,
    entry_point,
    len(payload),
    bss_size,
    512 * 1024,
    0,
    0
)

# 3. Записываем готовый .PRG: Header + Payload
with open(prg_path, "wb") as f:
    f.write(header)
    f.write(payload)

print(f"[devOS DPRG] {prg_path} successfully generated:")
print(f"  * Load VAddr:  0x{load_vaddr:016X}")
print(f"  * Entry Point: 0x{entry_point:016X}")
print(f"  * Code Size:   {len(payload)} bytes")
print(f"  * BSS Size:    {bss_size} bytes")