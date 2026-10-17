#ifndef OVCRAFT_ICONS_H
#define OVCRAFT_ICONS_H
// =============================================================================
// icons.h — значки в слотах инвентаря и панели.
//
// Блок — кубик под углом, как в Minecraft (ортографическая проекция с
// поворотом на 45° и наклоном на 30°): верх и две стороны с текстурами
// блока, стороны темнее. Предмет — его плоская картинка. Значок строится
// один раз на каждый размер и кешируется; рисуется прямо в кадр окна.
// =============================================================================

#include <stdint.h>
#include "engine/ovengine_ll.h"
#include "libui/ui.h"

namespace icons {

void init(ov::Renderer* r);
// size — сторона значка в пикселях
void draw(ui_win_t* w, int x, int y, int size, uint16_t id);
// Предмет в слоте: значок и число (если больше одного)
void slot(ui_win_t* w, int x, int y, int size, uint16_t id, int count);

} // namespace icons

#endif
