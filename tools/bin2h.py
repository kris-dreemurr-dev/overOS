#!/usr/bin/env python3
"""Плоский бинарник -> C-массив (заголовок для модуля).

Нужен stress.sys: код для дополнительных ядер пишется на ассемблере
(src/modules/asm/ap_burn.asm), а не набирается опкодами руками.
Запуск: python3 tools/bin2h.py вход.bin выход.h имя_массива
"""
import os
import sys

src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()

os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
with open(dst, "w") as f:
    f.write("// Сгенерировано tools/bin2h.py из %s — не править руками\n" % os.path.basename(src))
    f.write("static const uint8_t %s[] = {\n" % name)
    for i in range(0, len(data), 12):
        row = ", ".join("0x%02X" % b for b in data[i:i + 12])
        f.write("    %s,\n" % row)
    f.write("};\n")
print("%s: %d байт" % (dst, len(data)))
