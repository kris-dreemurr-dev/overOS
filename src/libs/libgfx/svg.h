#ifndef SVG_H
#define SVG_H
// svg.h — рисование значков в формате SVG (подмножество) в overOS.
//
// Значки интерфейса перестали быть монохромными картинками 16x16: теперь это
// обычные .svg в /sys/icons, которые система разбирает и растеризует сама —
// заменил файл, и значок стал другим, без пересборки.
//
// Поддерживается то, из чего состоят значки интерфейса:
//   <svg viewBox>, <g>, <path d>, <rect>, <circle>, <ellipse>, <line>,
//   <polygon>, <polyline>;
//   команды пути M L H V C Q S T A Z (абсолютные и относительные);
//   fill (#rgb, #rrggbb, none, currentColor), fill-opacity, opacity,
//   stroke, stroke-width, stroke-opacity;
//   transform: translate / scale / matrix.
// Заливка — по правилу ненулевого обхода со сглаживанием (супервыборка 4x4),
// как и у шрифта.
//
// Цвет currentColor берётся из параметра tint: один и тот же значок трея можно
// рисовать обычным, подсвеченным или приглушённым.

#include <stdint.h>

// Нарисовать значок в буфер ARGB (0xAARRGGBB, альфа 0 — прозрачно) размером w*h.
// Буфер очищается. Возвращает 0, если получилось.
int svg_render(const uint8_t* data, uint32_t len, uint32_t* out, int w, int h, uint32_t tint);

#endif
