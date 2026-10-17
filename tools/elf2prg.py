#!/usr/bin/env python3
import sys
import struct
import subprocess
import os

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <input.elf> <output.prg> [--desktop] [--icon icon.svg] [--name Имя] [--app класс]")
    sys.exit(1)

elf_path = sys.argv[1]
prg_path = sys.argv[2]
is_desktop = 1 if "--desktop" in sys.argv else 0

def opt(name):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return None

# Ресурсы программы (как ресурсы PE в Windows или .desktop + значок в Linux,
# только внутри самого файла): значок SVG, человеческое имя и класс программы.
# Их читают рабочий стол (окно, панель, меню, уведомления) и overFiles (значок
# файла .prg). Лежат после кода, загрузчик ядра их не трогает.
resources = []
icon_path = opt("--icon")
if icon_path:
    with open(icon_path, "rb") as f:
        resources.append((b"ICON", f.read()))
if opt("--name"):
    resources.append((b"NAME", opt("--name").encode("utf-8") + b"\0"))
if opt("--app"):
    resources.append((b"APPI", opt("--app").encode("utf-8") + b"\0"))

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

# Каталог ресурсов: "DRES", число записей, записи {тип[4], смещение, размер}
# (смещения от начала файла), затем сами данные. Смещение каталога -- в
# последнем поле заголовка (раньше резерв, 0 -- ресурсов нет).
res_blob = b""
res_off = 0
if resources:
    res_off = 64 + len(payload)
    res_off += (-res_off) % 8
    table = b"DRES" + struct.pack("<I", len(resources))
    data_off = res_off + len(table) + 12 * len(resources)
    entries = b""
    data = b""
    for typ, blob in resources:
        entries += typ + struct.pack("<II", data_off + len(data), len(blob))
        data += blob
    res_blob = table + entries + data

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
    res_off
)

with open(prg_path, "wb") as f:
    f.write(header)
    f.write(payload)
    if res_blob:
        f.write(b"\0" * (res_off - 64 - len(payload)))
        f.write(res_blob)

print(f"[devOS DPRG] {prg_path} успешно собран:")
print(f"  * Базовый адрес: 0x{load_vaddr:016X}")
print(f"  * Точка входа:   0x{entry_point:016X}")
print(f"  * Размер кода:   {code_size} байт")
print(f"  * Размер BSS:    {bss_size} байт (выделено страниц: {(code_size + bss_size + 4095)//4096})")
if resources:
    print("  * Ресурсы:       " + ", ".join(f"{t.decode()} {len(b)} байт" for t, b in resources))