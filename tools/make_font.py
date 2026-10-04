import os
import re
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

# Проверяем путь к файлу шрифта (в корне проекта или текущей папке)
font_file = "fontdata_ru_8x16.c"
if not os.path.exists(font_file) and os.path.exists("../fontdata_ru_8x16.c"):
  font_file = "../fontdata_ru_8x16.c"

# 1. Загружаем и парсим оригинальный C-массив шрифта из ядра[cite: 15]
with open(font_file, "r", encoding="utf-8") as f:
  content = f.read()

hex_values = re.findall(r"0x[0-9a-fA-F]+", content)
font_bytes = [int(x, 16) for x in hex_values]

CHAR_W = 8
CHAR_H = 16
num_chars = len(font_bytes) // CHAR_H

# 2. Инициализируем сборщик шрифта
fb = FontBuilder(unitsPerEm=1000)

glyph_order = [".notdef"]
h_metrics = {".notdef": (500, 0)}
glyphs = {}

# Создаем пустой глиф по умолчанию
pen = TTGlyphPen(None)
pen.moveTo((0, 0))
pen.lineTo((400, 0))
pen.lineTo((400, 800))
pen.lineTo((0, 800))
pen.closePath()
glyphs[".notdef"] = pen.glyph()

pixel_size = 60
advance_width = CHAR_W * pixel_size  # Ширина символа

# 3. Превращаем каждый пиксель растра в векторный контур для моноширинного шрифта
advance_width = CHAR_W * pixel_size  # Фиксированная ширина ячейки для каждого символа

for char_idx in range(min(num_chars, 256)):
  glyph_name = f"uni{char_idx:04X}"
  glyph_order.append(glyph_name)

  # Для моноширинного терминального шрифта LSB всегда 0, а ширина у всех одинаковая
  h_metrics[glyph_name] = (advance_width, 0)

  pen = TTGlyphPen(None)
  char_data = font_bytes[char_idx * CHAR_H : (char_idx + 1) * CHAR_H]

  for row_idx, row_byte in enumerate(char_data):
    y_top = 800 - row_idx * pixel_size
    y_bottom = y_top - pixel_size

    for bit_idx in range(CHAR_W):
      if (row_byte >> (7 - bit_idx)) & 1:
        x_left = bit_idx * pixel_size
        x_right = x_left + pixel_size

        # Рисуем пиксель внутри фиксированной сетки ячейки
        pen.moveTo((x_left, y_bottom))
        pen.lineTo((x_right, y_bottom))
        pen.lineTo((x_right, y_top))
        pen.lineTo((x_left, y_top))
        pen.closePath()

  glyphs[glyph_name] = pen.glyph()

# 4. Настраиваем таблицы TrueType
fb.setupGlyphOrder(glyph_order)
fb.setupCharacterMap({i: f"uni{i:04X}" for i in range(256)})
fb.setupGlyf(glyphs)
fb.setupHorizontalMetrics(h_metrics)

# Исправленные метрики для четкого позиционирования строки без «укатывания»
ascent = 750    # Высота над базовой линией
descent = -250  # Глубина под базовой линией

fb.setupHorizontalHeader(ascent=ascent, descent=descent)
fb.setupOS2(
    sTypoAscender=ascent,
    sTypoDescender=descent,
    usWinAscent=ascent,
    usWinDescent=abs(descent),
)
fb.setupNameTable({
    "familyName": "devOS Terminal",
    "styleName": "Regular",
    "fullName": "devOS Terminal",
    "psName": "devOSTerminal",
})
fb.setupPost()

# 5. Сохраняем готовый файл
output_filename = "devOS_Terminal.ttf"
fb.save(output_filename)
print(f"Готово! Шрифт успешно скомпилирован в файл '{output_filename}'.")
