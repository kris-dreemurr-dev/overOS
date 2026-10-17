#!/usr/bin/env python3
"""Текстуры overCraft (16x16 PNG) в src/files/progs/overCraft/resources.

Рисуются кодом, а не руками: шум на хеше координат, у каждого блока свой узор.
Запуск: python3 tools/ovcraft_textures.py (нужен Pillow). Картинки можно и
перерисовать в любом редакторе — игра читает обычные PNG, размер — степень двойки.
Альфа-канал движок понимает сам: только 0 и 255 — «вырез» (дырки без смешивания),
промежуточные значения — полупрозрачная текстура (вода, стекло).
"""
import os
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "src/files/progs/overCraft/resources")
N = 16


def h(x, y, s):
    v = (x * 374761393 + y * 668265263 + s * 2246822519) & 0xFFFFFFFF
    v = ((v ^ (v >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((v ^ (v >> 16)) & 0xFFFF) / 65535.0


def shade(c, k):
    return tuple(max(0, min(255, int(ch * k))) for ch in c)


def noise_tex(base, var, seed, spots=None):
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            k = 1.0 + (h(x, y, seed) - 0.5) * var
            c = shade(base, k)
            if spots and h(x, y, seed + 7) < spots[0]:
                c = shade(spots[1], 0.9 + h(x, y, seed + 9) * 0.2)
            img.putpixel((x, y), c)
    return img


def grass_side():
    img = noise_tex((134, 96, 67), 0.35, 3, (0.08, (150, 150, 150)))
    for x in range(N):
        depth = 3 + int(h(x, 0, 11) * 3)
        for y in range(depth):
            img.putpixel((x, y), shade((96, 160, 60), 0.85 + h(x, y, 12) * 0.3))
    return img


def log_side():
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            stripe = 0.75 if (x + int(h(x // 3, y // 5, 4) * 2)) % 4 == 0 else 1.0
            img.putpixel((x, y), shade((112, 84, 50), stripe * (0.9 + h(x, y, 5) * 0.2)))
    return img


def log_top():
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            d = max(abs(x - 7.5), abs(y - 7.5))
            if d > 6.5:
                c = shade((112, 84, 50), 0.8 + h(x, y, 6) * 0.2)
            else:
                ring = 0.85 if int(d) % 2 == 0 else 1.0
                c = shade((176, 142, 92), ring * (0.92 + h(x, y, 7) * 0.12))
            img.putpixel((x, y), c)
    return img


def planks():
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            seam = (y % 4 == 3) or (x == (0 if (y // 4) % 2 == 0 else 8))
            k = 0.7 if seam else 0.9 + h(x // 4, y, 8) * 0.2
            img.putpixel((x, y), shade((168, 130, 80), k))
    return img


def cobble():
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            cx, cy = (x + (y // 4) % 2 * 2) // 4, y // 4
            edge = (x + (y // 4) % 2 * 2) % 4 == 0 or y % 4 == 0
            k = 0.55 if edge else 0.8 + h(cx, cy, 9) * 0.35
            img.putpixel((x, y), shade((128, 128, 128), k))
    return img


def brick():
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            off = 4 if (y // 4) % 2 else 0
            mortar = y % 4 == 3 or (x + off) % 8 == 7
            c = (190, 186, 178) if mortar else shade((150, 62, 46), 0.88 + h(x, y, 10) * 0.24)
            img.putpixel((x, y), c)
    return img


def glass():
    # Альфа: рамка непрозрачна, блик полупрозрачный, середина почти прозрачна
    img = Image.new("RGBA", (N, N))
    for y in range(N):
        for x in range(N):
            if x in (0, N - 1) or y in (0, N - 1):
                c = (210, 232, 240, 255)
            elif x == y + 3 or x == y + 4:
                c = (235, 246, 252, 150)
            else:
                c = (170, 210, 228, 36)
            img.putpixel((x, y), c)
    return img


def water():
    img = noise_tex((52, 92, 200), 0.18, 16).convert("RGBA")
    img.putalpha(168)                    # ~65 %: дно видно, цвет воды остаётся
    return img


def leaves():
    img = Image.new("RGB", (N, N))
    for y in range(N):
        for x in range(N):
            v = h(x, y, 13)
            c = shade((58, 120, 40), 0.55 + v * 0.7) if v > 0.18 else (36, 70, 26)
            img.putpixel((x, y), c)
    return img


TEX = {
    "grass_top": lambda: noise_tex((96, 160, 60), 0.35, 1),
    "grass_side": grass_side,
    "dirt": lambda: noise_tex((134, 96, 67), 0.35, 2, (0.06, (150, 150, 150))),
    "stone": lambda: noise_tex((125, 125, 125), 0.28, 14, (0.05, (100, 100, 100))),
    "cobble": cobble,
    "sand": lambda: noise_tex((219, 207, 160), 0.16, 15),
    "log_side": log_side,
    "log_top": log_top,
    "leaves": leaves,
    "planks": planks,
    "water": water,
    "bedrock": lambda: noise_tex((70, 70, 70), 0.9, 17),
    "snow": lambda: noise_tex((240, 246, 250), 0.06, 18),
    "glass": glass,
    "brick": brick,
}

# --------------------------------------------------------------------------
# Развёртки моделей (entities/). Раскладка коробки — как в Minecraft: коробка
# w x h x d с началом (u, v) занимает полосу 2*(d+w) x (d+h):
#     сверху  [ d ][ верх w ][ низ w ]
#     снизу   [ право d ][ перед w ][ лево d ][ зад w ]
# Так же её читает game/model.cpp, поэтому скин Стива из настоящей игры
# (64x64) тоже ляжет на модель как есть.

def box_faces(u, v, w, h, d):
    return {
        "top": (u + d, v, w, d), "bottom": (u + d + w, v, w, d),
        "right": (u, v + d, d, h), "front": (u + d, v + d, w, h),
        "left": (u + d + w, v + d, d, h), "back": (u + 2 * d + w, v + d, w, h),
    }


def paint(img, rect, color, var=0.12, seed=0):
    x0, y0, w, h = rect
    for y in range(h):
        for x in range(w):
            k = 1.0 + (h_(x0 + x, y0 + y, seed) - 0.5) * var
            img.putpixel((x0 + x, y0 + y), shade(color, k) + (255,))


def h_(x, y, s):
    return h(x, y, s)


def px(img, rect, x, y, color):
    img.putpixel((rect[0] + x, rect[1] + y), tuple(color) + (255,))


def paint_box(img, u, v, w, hh, d, color, seed, var=0.12):
    f = box_faces(u, v, w, hh, d)
    for r in f.values():
        paint(img, r, color, var, seed)
    return f


SKIN, HAIR = (196, 140, 106), (58, 36, 20)
SHIRT, PANTS, SHOES = (0, 160, 160), (54, 60, 150), (96, 96, 96)


def steve():
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    # Голова (0,0) 8x8x8: волосы сверху, сзади и по краю лба
    f = paint_box(img, 0, 0, 8, 8, 8, SKIN, 21, 0.08)
    paint(img, f["top"], HAIR, 0.2, 22)
    paint(img, f["back"], HAIR, 0.2, 23)
    for side in ("left", "right"):
        r = f[side]
        for y in range(8):
            for x in range(8):
                if y < 2 or (y < 4 and ((side == "right" and x < 5) or (side == "left" and x > 2))):
                    px(img, r, x, y, shade(HAIR, 0.9 + h(x, y, 24) * 0.2))
    fr = f["front"]
    for x in range(8):
        px(img, fr, x, 0, HAIR)
        px(img, fr, x, 1, HAIR if x in (0, 1, 6, 7) or h(x, 1, 25) > 0.5 else shade(HAIR, 1.2))
    for x in (0, 7):
        px(img, fr, x, 2, HAIR)
    for x, c in ((1, (255, 255, 255)), (2, (80, 60, 160)), (5, (80, 60, 160)), (6, (255, 255, 255))):
        px(img, fr, x, 4, c)
    px(img, fr, 3, 5, shade(SKIN, 0.8)); px(img, fr, 4, 5, shade(SKIN, 0.8))
    for x in range(2, 6):
        px(img, fr, x, 6, (110, 60, 40))
    px(img, fr, 1, 6, shade(SKIN, 0.85)); px(img, fr, 6, 6, shade(SKIN, 0.85))
    # Тело (16,16) 8x12x4 — рубашка
    paint_box(img, 16, 16, 8, 12, 4, SHIRT, 26, 0.12)
    # Руки (40,16) и (32,48) 4x12x4: рукав сверху, дальше кожа
    for (u, v) in ((40, 16), (32, 48)):
        f = paint_box(img, u, v, 4, 12, 4, SKIN, 27, 0.08)
        paint(img, f["top"], SHIRT, 0.12, 28)
        for side in ("front", "back", "left", "right"):
            r = f[side]
            for y in range(4):
                for x in range(r[2]):
                    px(img, r, x, y, shade(SHIRT, 0.92 + h(x, y, 29) * 0.16))
    # Ноги (0,16) и (16,48) 4x12x4: штаны, внизу ботинки
    for (u, v) in ((0, 16), (16, 48)):
        f = paint_box(img, u, v, 4, 12, 4, PANTS, 30, 0.12)
        paint(img, f["bottom"], SHOES, 0.1, 31)
        for side in ("front", "back", "left", "right"):
            r = f[side]
            for y in range(9, 12):
                for x in range(r[2]):
                    px(img, r, x, y, shade(SHOES, 0.9 + h(x, y, 32) * 0.2))
    return img


PINK = (236, 152, 150)


def pig():
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    f = paint_box(img, 0, 0, 8, 8, 8, PINK, 41, 0.1)                 # голова
    fr = f["front"]
    for x, c in ((1, (255, 255, 255)), (2, (20, 20, 20)), (5, (20, 20, 20)), (6, (255, 255, 255))):
        px(img, fr, x, 3, c)
    f = paint_box(img, 32, 0, 4, 3, 1, shade(PINK, 0.85), 42, 0.06)  # пятачок
    px(img, f["front"], 1, 1, (120, 60, 60)); px(img, f["front"], 2, 1, (120, 60, 60))
    f = paint_box(img, 0, 16, 4, 6, 4, PINK, 43, 0.1)                # ноги
    paint(img, f["bottom"], (90, 60, 50), 0.1, 44)
    for side in ("front", "back", "left", "right"):
        r = f[side]
        for x in range(r[2]):
            px(img, r, x, 5, (90, 60, 50))
    paint_box(img, 0, 32, 10, 8, 16, PINK, 45, 0.14)                 # туловище
    return img


def porkchop():
    # Предмет: силуэт на прозрачном фоне (альфа 0/255 — «вырез» в движке)
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    shape = [
        "................",
        "......####......",
        "....##rrrr##....",
        "...#rrrrrrrr#...",
        "..#rrrRRrrrrr#..",
        "..#rrRRRRrrrrf#.",
        ".#rrrRRRrrrrrf#.",
        ".#rrrrrrrrrrff#.",
        ".#frrrrrrrrrf#..",
        ".#ffrrrrrrrff#..",
        "..#ffffrrfff#...",
        "...##fffff##....",
        ".....#####......",
        "................",
        "................",
        "................",
    ]
    col = {"#": (90, 30, 30), "r": (226, 110, 110), "R": (190, 70, 74), "f": (246, 220, 206)}
    for y, row in enumerate(shape):
        for x, ch in enumerate(row):
            if ch in col:
                img.putpixel((x, y), shade(col[ch], 0.92 + h(x, y, 51) * 0.16) + (255,))
    return img


def destroy(stage):
    # Трещины: случайные ломаные, на каждой стадии их больше (как в оригинале)
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    lines = 2 + stage * 2
    for i in range(lines):
        x, y = int(h(i, 1, 61) * 16), int(h(i, 2, 61) * 16)
        for step in range(5 + stage):
            img.putpixel((x % 16, y % 16), (24, 24, 24, 255))
            r = h(i, step, 62)
            if r < 0.35: x += 1
            elif r < 0.7: y += 1
            elif r < 0.85: x -= 1
            else: y -= 1
    return img


os.makedirs(OUT, exist_ok=True)
for name, fn in TEX.items():
    fn().save(os.path.join(OUT, name + ".png"), optimize=True)
extra = {"entities/steve": steve, "entities/pig": pig, "items/porkchop": porkchop}
for k in range(10):
    extra["destroy/destroy_%d" % k] = (lambda k=k: destroy(k))
for name, fn in extra.items():
    path = os.path.join(OUT, name + ".png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fn().save(path, optimize=True)
print("текстур:", len(TEX) + len(extra), "->", OUT)
