#ifndef OVCRAFT_SHAPES_H
#define OVCRAFT_SHAPES_H
// =============================================================================
// shapes.h — готовые сетки для всего, что не мир и не модель моба:
//   block(b)   — кубик блока 1x1x1 с центром в нуле (выпавший блок, блок в руке)
//   item(id)   — значок предмета, выдавленный на пиксель в толщину, как в
//                Minecraft: лицо, оборот и бортики по краю непрозрачных
//                пикселей; 1x1 по X и Y, центр в нуле, лицом к +Z
//   held(id)   — что из двух подходит этому номеру
//   crack(k)   — куб чуть больше блока с трещинами стадии k (0..9)
// Строятся один раз в init, после того как движок загрузил все текстуры.
// =============================================================================

#include "engine/ovengine_ll.h"

namespace shapes {

bool init(ov::Renderer& r, int crack_tex0);
const ov::Mesh& block(uint8_t b);
const ov::Mesh& item(uint16_t id);
const ov::Mesh& held(uint16_t id);
const ov::Mesh& crack(int stage);

} // namespace shapes

#endif
