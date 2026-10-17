// =============================================================================
// inventory.cpp — инвентарь и его экран (см. inventory.h).
// =============================================================================

#include "inventory.h"
#include "items.h"
#include "game/icons.h"
#include "libui/libui_ll.h"

// --------------------------------------------------------------- инвентарь
int Inventory::add(uint16_t id, int count) {
    if (!item_valid(id)) return count;
    const int max = item_max_stack(id);
    // Сначала доложить в начатые стопки, потом — в пустые слоты (панель первой)
    for (int pass = 0; pass < 2 && count > 0; pass++)
        for (int i = 0; i < SIZE && count > 0; i++) {
            Stack& s = slots[i];
            if (pass == 0 ? (s.empty() || s.id != id) : !s.empty()) continue;
            if (pass == 1) { s.id = id; s.count = 0; }
            int room = max - s.count;
            int put = count < room ? count : room;
            s.count += put;
            count -= put;
        }
    return count;
}

void Inventory::take_held(int n) {
    Stack& s = held();
    s.count -= n;
    if (s.count <= 0) s.clear();
}

void Inventory::fill_creative() {
    for (int i = 0; i < BAR; i++) { slots[i].id = HOTBAR[i]; slots[i].count = 1; }
}

// --------------------------------------------------------------- палитра
namespace gui {

int scale(int W, int H) {
    int s = W / 400 < H / 260 ? W / 400 : H / 260;
    return s < 2 ? 2 : (s > 4 ? 4 : s);
}

int palette_count() {
    int n = 0;
    for (int b = 1; b < BLOCK_COUNT; b++) if (BLOCKS[b].placeable) n++;
    return n + ITEM_COUNT;
}

uint16_t palette_item(int i) {
    for (int b = 1; b < BLOCK_COUNT; b++)
        if (BLOCKS[b].placeable && i-- == 0) return (uint16_t)b;
    return i < ITEM_COUNT ? (uint16_t)(ITEM_FIRST + i) : 0;
}

} // namespace gui

// --------------------------------------------------------------- экран
InventoryScreen::Layout InventoryScreen::layout(int W, int H, bool creative) const {
    Layout L;
    L.s = gui::scale(W, H);
    L.slot = 18 * L.s;
    L.rows = creative ? (gui::palette_count() + 8) / 9 : 3;
    if (L.rows < 1) L.rows = 1;
    L.w = 9 * L.slot + 14 * L.s;
    L.h = (L.rows + 1) * L.slot + 34 * L.s;
    L.x0 = (W - L.w) / 2;
    L.y0 = (H - L.h) / 2;
    L.grid_y = L.y0 + 18 * L.s;
    L.bar_y = L.grid_y + L.rows * L.slot + 6 * L.s;
    return L;
}

int InventoryScreen::hit(const Layout& L, bool creative) const {
    int gx = L.x0 + 7 * L.s;
    if (mx_ < L.x0 || my_ < L.y0 || mx_ >= L.x0 + L.w || my_ >= L.y0 + L.h) return -2;
    if (mx_ < gx || mx_ >= gx + 9 * L.slot) return -1;
    int col = (mx_ - gx) / L.slot;
    if (my_ >= L.bar_y && my_ < L.bar_y + L.slot) return col;
    if (my_ >= L.grid_y && my_ < L.grid_y + L.rows * L.slot) {
        int row = (my_ - L.grid_y) / L.slot;
        if (creative) {
            int k = row * 9 + col;
            return k < gui::palette_count() ? 100 + k : -1;
        }
        return 9 + row * 9 + col;
    }
    return -1;
}

void InventoryScreen::show(int W, int H) {
    open = true;
    mx_ = W / 2;
    my_ = H / 2;
    cursor.clear();
}

void InventoryScreen::hide(Inventory& inv, Stack& out) {
    open = false;
    out.clear();
    if (cursor.empty()) return;
    int left = inv.add(cursor.id, cursor.count);
    if (left) { out.id = cursor.id; out.count = left; }
    cursor.clear();
}

// Щелчок по слоту с предметом у курсора или без
void InventoryScreen::click_slot(Stack& s, bool right) {
    if (!right) {
        if (cursor.empty() || s.empty() || cursor.id != s.id) {
            Stack t = s; s = cursor; cursor = t;                 // взять, положить или поменять
        } else {
            int room = item_max_stack(s.id) - s.count;           // та же вещь — доложить
            int put = cursor.count < room ? cursor.count : room;
            s.count += put;
            cursor.count -= put;
            if (cursor.count <= 0) cursor.clear();
        }
    } else if (cursor.empty()) {
        if (s.empty()) return;
        cursor.id = s.id;                                        // половину, большую — в руку
        cursor.count = (s.count + 1) / 2;
        s.count -= cursor.count;
        if (s.count <= 0) s.clear();
    } else if (s.empty() || (s.id == cursor.id && s.count < item_max_stack(s.id))) {
        if (s.empty()) { s.id = cursor.id; s.count = 0; }        // по одному
        s.count++;
        if (--cursor.count <= 0) cursor.clear();
    }
}

// Shift+щелчок: панель <-> рюкзак, сначала в начатые стопки
void InventoryScreen::shift_move(Inventory& inv, int from) {
    Stack& s = inv.slots[from];
    if (s.empty()) return;
    int a = from < Inventory::BAR ? Inventory::BAR : 0, b = from < Inventory::BAR ? Inventory::SIZE : Inventory::BAR;
    const int max = item_max_stack(s.id);
    for (int pass = 0; pass < 2 && !s.empty(); pass++)
        for (int i = a; i < b && !s.empty(); i++) {
            Stack& d = inv.slots[i];
            if (pass == 0 ? (d.empty() || d.id != s.id) : !d.empty()) continue;
            if (pass == 1) { d.id = s.id; d.count = 0; }
            int put = s.count < max - d.count ? s.count : max - d.count;
            d.count += put;
            s.count -= put;
            if (s.count <= 0) s.clear();
        }
}

void InventoryScreen::update(const Input& in, Inventory& inv, bool creative, int W, int H, Stack& out) {
    out.clear();
    mx_ += in.dx();
    my_ += in.dy();
    if (mx_ < 0) mx_ = 0;
    if (my_ < 0) my_ = 0;
    if (mx_ >= W) mx_ = W - 1;
    if (my_ >= H) my_ = H - 1;
    Layout L = layout(W, H, creative);
    hover_ = hit(L, creative);
    const bool shift = in.down(key::SHIFT);
    for (int b = 0; b < 2; b++) {
        if (!in.clicked(b)) continue;
        bool right = b == 1;
        if (hover_ == -2) {                                      // за окном — выбросить
            if (cursor.empty()) continue;
            out = cursor;
            if (right) { out.count = 1; if (--cursor.count <= 0) cursor.clear(); }
            else cursor.clear();
        } else if (hover_ >= 100) {                              // палитра
            uint16_t id = gui::palette_item(hover_ - 100);
            if (!cursor.empty() && !right) { cursor.clear(); continue; }
            if (shift) { inv.add(id, item_max_stack(id)); continue; }
            if (!cursor.empty() && cursor.id == id) {
                if (cursor.count < item_max_stack(id)) cursor.count++;
            } else {
                cursor.id = id;
                cursor.count = right ? 1 : item_max_stack(id);
            }
        } else if (hover_ >= 0) {
            if (shift && !right && !creative) shift_move(inv, hover_);
            else click_slot(inv.slots[hover_], right);
        }
    }
}

namespace {

// Рамка в духе Minecraft: светлый верх-лево, тёмный низ-право
void bevel(ui_win_t* w, int x, int y, int ww, int hh, int s, uint32_t face, uint32_t light, uint32_t dark) {
    ui_fill(w, x, y, ww, hh, face);
    ui_fill(w, x, y, ww - s, s, light);
    ui_fill(w, x, y, s, hh - s, light);
    ui_fill(w, x + s, y + hh - s, ww - s, s, dark);
    ui_fill(w, x + ww - s, y + s, s, hh - s, dark);
}

void slot_box(ui_win_t* w, int x, int y, int slot, int s) {
    // Слот вдавлен: тёмный верх-лево, светлый низ-право
    bevel(w, x, y, slot, slot, s > 1 ? s / 2 : 1, 0x8B8B8B, 0x373737, 0xFFFFFF);
}

// Курсор — стрелка 1-битной картинкой, как и в композиторе
const char* const ARROW[12] = {
    "#.........", "##........", "#o#.......", "#oo#......", "#ooo#.....", "#oooo#....",
    "#ooooo#...", "#oooooo#..", "#ooo####..", "#o#o#.....", "##.#o#....", "....##....",
};

void arrow(ui_win_t* w, int x, int y, int s) {
    for (int j = 0; j < 12; j++)
        for (int i = 0; i < 10; i++) {
            char c = ARROW[j][i];
            if (c == '#') ui_fill(w, x + i * s, y + j * s, s, s, 0x101010);
            else if (c == 'o') ui_fill(w, x + i * s, y + j * s, s, s, 0xFFFFFF);
        }
}

} // namespace

void InventoryScreen::draw(ui_win_t* w, const Inventory& inv, bool creative) const {
    const int W = ui_width(w), H = ui_height(w);
    Layout L = layout(W, H, creative);
    const int s = L.s, icon = 16 * s, pad = s;
    // Затемнение мира за окном
    uint32_t* px = ui_pixels(w);
    for (int i = 0; i < W * H; i++) px[i] = (px[i] >> 1) & 0x7F7F7F;
    bevel(w, L.x0, L.y0, L.w, L.h, 2 * s, 0xC6C6C6, 0xFFFFFF, 0x555555);
    ui_text(w, L.x0 + 8 * s, L.y0 + 5 * s, creative ? "Творческий режим" : "Инвентарь", 0x404040);
    const int gx = L.x0 + 7 * s;
    for (int row = 0; row < L.rows; row++)
        for (int col = 0; col < 9; col++) {
            int x = gx + col * L.slot, y = L.grid_y + row * L.slot;
            slot_box(w, x, y, L.slot, s);
            if (creative) {
                int k = row * 9 + col;
                if (k < gui::palette_count()) icons::draw(w, x + pad, y + pad, icon, gui::palette_item(k));
            } else {
                const Stack& st = inv.slots[9 + row * 9 + col];
                icons::slot(w, x + pad, y + pad, icon, st.id, st.count);
            }
        }
    for (int col = 0; col < 9; col++) {
        int x = gx + col * L.slot;
        slot_box(w, x, L.bar_y, L.slot, s);
        const Stack& st = inv.slots[col];
        icons::slot(w, x + pad, L.bar_y + pad, icon, st.id, creative ? 0 : st.count);
        if (creative) icons::draw(w, x + pad, L.bar_y + pad, icon, st.id);
    }
    // Подсветка слота под курсором и подпись предмета
    int hx = -1, hy = -1;
    uint16_t hid = 0;
    if (hover_ >= 100) {
        int k = hover_ - 100;
        hx = gx + (k % 9) * L.slot; hy = L.grid_y + (k / 9) * L.slot;
        hid = gui::palette_item(k);
    } else if (hover_ >= 0) {
        hx = gx + (hover_ % 9) * L.slot;
        hy = hover_ < 9 ? L.bar_y : L.grid_y + (hover_ / 9 - 1) * L.slot;
        hid = inv.slots[hover_].empty() ? 0 : inv.slots[hover_].id;
    }
    if (hx >= 0) {
        for (int j = pad; j < L.slot - pad; j++) {
            if (hy + j < 0 || hy + j >= H) continue;
            uint32_t* row = px + (hy + j) * W;
            for (int i = pad; i < L.slot - pad; i++)
                if (hx + i >= 0 && hx + i < W) {
                    uint32_t c = row[hx + i];
                    row[hx + i] = ((c >> 1) & 0x7F7F7F) + 0x606060;
                }
        }
    }
    if (!cursor.empty()) {
        icons::slot(w, mx_ - icon / 2, my_ - icon / 2, icon, cursor.id, cursor.count);
    } else if (hid) {
        const char* name = item_name(hid);
        int tw = ui_text_width(name);
        int tx = mx_ + 12 * s / 2, ty = my_ - 12 * s / 2 - UI_FONT_H;
        ui_fill(w, tx - 4, ty - 3, tw + 8, UI_FONT_H + 6, 0x100010);
        ui_frame(w, tx - 4, ty - 3, tw + 8, UI_FONT_H + 6, 0x2A0A5A);
        ui_text(w, tx, ty, name, 0xFFFFFF);
    }
    arrow(w, mx_, my_, s > 2 ? 2 : 1);
}
