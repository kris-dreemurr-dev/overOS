// =============================================================================
// hid.c — USB-клавиатуры и мыши: отчёты -> скан-коды и движения (см. hid.h).
// =============================================================================

#include "hid.h"

extern void kbd_scancode(uint8_t sc);                     // kernel/keyboard.c
extern void mouse_feed_report(int dx, int dy, int wheel, uint8_t buttons);   // drivers/mouse.c
extern void xhci_hid_service(void);                       // xhci-hid.c
extern uint32_t timer_ticks;
#define TICKS (*(volatile uint32_t*)&timer_ticks)

// ------------------------------------------------- коды USB -> набор 1 PS/2
// Индекс — код USB (страница 7), значение — скан-код; 0x100 — с префиксом E0
#define E0 0x100
static const uint16_t s_usb2ps2[0x66] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21,
    [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13,
    [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C,
    [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40,
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = E0 | 0x37, [0x47] = 0x46,
    [0x49] = E0 | 0x52, [0x4A] = E0 | 0x47, [0x4B] = E0 | 0x49, [0x4C] = E0 | 0x53,
    [0x4D] = E0 | 0x4F, [0x4E] = E0 | 0x51, [0x4F] = E0 | 0x4D, [0x50] = E0 | 0x4B,
    [0x51] = E0 | 0x50, [0x52] = E0 | 0x48, [0x53] = 0x45, [0x54] = E0 | 0x35, [0x55] = 0x37,
    [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = E0 | 0x1C, [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,
    [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = E0 | 0x5D,
};
// Модификаторы — биты первого байта отчёта: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI
static const uint16_t s_mod2ps2[8] = { 0x1D, 0x2A, 0x38, E0 | 0x5B, E0 | 0x1D, 0x36, E0 | 0x38, E0 | 0x5C };

// ------------------------------------------------- кольцо до usb_hid_drain
// Один писатель (прерывание таймера), читатель — ps2_hw_service под флагом
// s_draining: его зовут и из простоя ядра, и из сисколлов мыши
typedef struct { uint8_t kind, sc; int8_t wheel; uint8_t buttons; int16_t dx, dy; } hid_ev_t;
#define EV_KEY   1
#define EV_MOUSE 2
#define RING 512
static hid_ev_t s_ring[RING];
static volatile uint32_t s_head = 0, s_tail = 0;
static volatile int s_draining = 0;

static void push(const hid_ev_t* e) {
    uint32_t h = s_head, n = (h + 1) % RING;
    if (n == s_tail) return;                        // переполнилось — теряем самое новое
    s_ring[h] = *e;
    __asm__ volatile("" ::: "memory");
    s_head = n;
}

static void push_code(uint16_t code, int release) {
    hid_ev_t e = { EV_KEY, 0, 0, 0, 0, 0 };
    if (code & E0) { e.sc = 0xE0; push(&e); }
    e.sc = (uint8_t)((code & 0x7F) | (release ? 0x80 : 0));
    push(&e);
}

// ------------------------------------------------- клавиатуры
typedef struct {
    int used;
    uint8_t mods;
    uint8_t keys[6];
} kbd_t;
static kbd_t s_kbd[HID_KBD_MAX];
static int s_mice = 0;

// Повтор: последняя нажатая клавиша через 500 мс начинает повторяться раз в 33 мс
static uint16_t s_rep_code = 0;
static uint32_t s_rep_next = 0;
static int s_rep_kb = -1;

int hid_kbd_alloc(void) {
    for (int i = 0; i < HID_KBD_MAX; i++)
        if (!s_kbd[i].used) {
            s_kbd[i].used = 1;
            s_kbd[i].mods = 0;
            for (int k = 0; k < 6; k++) s_kbd[i].keys[k] = 0;
            return i;
        }
    return -1;
}

void hid_kbd_free(int kb) {
    if (kb < 0 || kb >= HID_KBD_MAX) return;
    static const uint8_t none[8] = { 0 };
    hid_kbd_report(kb, none, 8);                    // всё отпустить: иначе Shift «залипнет»
    s_kbd[kb].used = 0;
}

static int has(const uint8_t* keys, uint8_t k) {
    for (int i = 0; i < 6; i++) if (keys[i] == k) return 1;
    return 0;
}

void hid_kbd_report(int kb, const uint8_t* r, int len) {
    if (kb < 0 || kb >= HID_KBD_MAX || len < 3) return;
    kbd_t* k = &s_kbd[kb];
    uint8_t keys[6] = { 0 };
    for (int i = 0; i < 6 && 2 + i < len; i++) keys[i] = r[2 + i];
    if (keys[0] == 0x01) return;                    // «слишком много клавиш сразу» — пропустить
    uint8_t mods = r[0];
    // Модификаторы: каждый изменившийся бит — нажатие или отпускание
    for (int b = 0; b < 8; b++) {
        int was = (k->mods >> b) & 1, now = (mods >> b) & 1;
        if (was != now) push_code(s_mod2ps2[b], !now);
    }
    // Отпущенные: были в прошлом отчёте, нет в этом
    for (int i = 0; i < 6; i++) {
        uint8_t u = k->keys[i];
        if (u < 0x04 || u >= 0x66 || has(keys, u) || !s_usb2ps2[u]) continue;
        push_code(s_usb2ps2[u], 1);
        if (s_rep_kb == kb && s_rep_code == s_usb2ps2[u]) s_rep_code = 0;
    }
    // Нажатые: есть сейчас, не было раньше
    for (int i = 0; i < 6; i++) {
        uint8_t u = keys[i];
        if (u < 0x04 || u >= 0x66 || has(k->keys, u) || !s_usb2ps2[u]) continue;
        push_code(s_usb2ps2[u], 0);
        s_rep_code = s_usb2ps2[u];
        s_rep_kb = kb;
        s_rep_next = TICKS + 500;
    }
    k->mods = mods;
    for (int i = 0; i < 6; i++) k->keys[i] = keys[i];
}

// ------------------------------------------------- мышь
void hid_mouse_count(int delta) { s_mice += delta; if (s_mice < 0) s_mice = 0; }

void hid_mouse_report(const uint8_t* r, int len) {
    if (len < 3) return;
    hid_ev_t e;
    e.kind = EV_MOUSE;
    e.sc = 0;
    e.buttons = r[0] & 0x07;
    e.dx = (int8_t)r[1];
    e.dy = (int8_t)r[2];                            // у USB вниз — плюс, как на экране
    e.wheel = len >= 4 ? (int8_t)r[3] : 0;
    push(&e);
}

// ------------------------------------------------- точки входа ядра
void usb_hid_tick(void) {
    xhci_hid_service();
    if (s_rep_code && (int32_t)(TICKS - s_rep_next) >= 0) {
        push_code(s_rep_code, 0);                   // повтор — ещё одно нажатие, как у PS/2
        s_rep_next = TICKS + 33;
    }
}

void usb_hid_drain(void) {
    if (__atomic_exchange_n(&s_draining, 1, __ATOMIC_ACQUIRE)) return;   // уже разбирает другой
    while (s_tail != s_head) {
        hid_ev_t e = s_ring[s_tail];
        __asm__ volatile("" ::: "memory");
        s_tail = (s_tail + 1) % RING;
        if (e.kind == EV_KEY) kbd_scancode(e.sc);
        else mouse_feed_report(e.dx, e.dy, -e.wheel, e.buttons);   // колесо: у USB вверх — плюс
    }
    __atomic_store_n(&s_draining, 0, __ATOMIC_RELEASE);
}

void usb_hid_counts(int* kbds, int* mice) {
    int n = 0;
    for (int i = 0; i < HID_KBD_MAX; i++) n += s_kbd[i].used;
    if (kbds) *kbds = n;
    if (mice) *mice = s_mice;
}
