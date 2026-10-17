#ifndef OVCRAFT_INVENTORY_H
#define OVCRAFT_INVENTORY_H
// =============================================================================
// inventory.h — инвентарь игрока и его экран (E).
//
// 36 слотов: 0..8 — панель внизу экрана, 9..35 — рюкзак. Предметы
// складываются в стопки до item_max_stack. Экран работает как в Minecraft:
//   ЛКМ        — взять стопку / положить / поменять местами
//   ПКМ        — взять половину / положить по одному
//   Shift+ЛКМ  — перебросить между панелью и рюкзаком
// В творческом режиме вместо рюкзака — палитра всех блоков и предметов:
// щелчок даёт полную стопку, щелчок с предметом в руке его выбрасывает.
//
// Мышь на этом экране остаётся захваченной (у захвата есть все три кнопки),
// курсор рисует сама игра.
// =============================================================================

#include <stdint.h>
#include "game/input.h"
#include "libui/ui.h"

struct Stack {
    uint16_t id = 0;
    int count = 0;
    bool empty() const { return id == 0 || count <= 0; }
    void clear() { id = 0; count = 0; }
};

class Inventory {
public:
    static const int SIZE = 36;
    static const int BAR = 9;
    Stack slots[SIZE];
    int selected = 0;

    int add(uint16_t id, int count);                // сколько не влезло
    Stack& held() { return slots[selected]; }
    const Stack& held() const { return slots[selected]; }
    void take_held(int n);                          // потратить из руки (поставили, съели)
    void clear() { for (int i = 0; i < SIZE; i++) slots[i].clear(); }
    void fill_creative();                           // панель — как HOTBAR из blocks.h
};

class InventoryScreen {
public:
    bool open = false;
    Stack cursor;                                   // что «в руке» у курсора

    void show(int W, int H);
    // Закрыть: предмет у курсора возвращается в инвентарь; что не влезло — в out
    void hide(Inventory& inv, Stack& out);
    // Кадр экрана: out — что выбросили за край окна (иначе пусто)
    void update(const Input& in, Inventory& inv, bool creative, int W, int H, Stack& out);
    void draw(ui_win_t* w, const Inventory& inv, bool creative) const;

private:
    struct Layout {
        int s, slot, x0, y0, w, h;
        int grid_y, bar_y;                          // верх рюкзака/палитры и панели
        int rows;                                   // строк рюкзака/палитры
    };
    Layout layout(int W, int H, bool creative) const;
    // Что под курсором: 0..35 — слот, 100+k — k-й предмет палитры, -1 — пусто внутри окна, -2 — снаружи
    int hit(const Layout& L, bool creative) const;
    void click_slot(Stack& s, bool right);
    void shift_move(Inventory& inv, int from);

    int mx_ = 0, my_ = 0;
    int hover_ = -1;
};

namespace gui {
// Масштаб интерфейса, как «GUI scale» в Minecraft: 2 на небольшом окне, 3 на большом
int scale(int W, int H);
// Предметы палитры творческого режима
int palette_count();
uint16_t palette_item(int i);
}

#endif
