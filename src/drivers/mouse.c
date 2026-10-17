#include "mouse.h"
#include <stdint.h>

extern uint16_t screen_width;
extern uint16_t screen_height;
extern void ps2_hw_service(void);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Единые координаты и состояние
static int g_mouse_x = 512;
static int g_mouse_y = 384;
static int g_last_dx = 0;
static int g_last_dy = 0;
static uint8_t g_mouse_btn = 0;

// Накопленное движение с момента последнего чтения get_mouse_delta() (для mouselook в играх).
// В отличие от g_mouse_x/y, не упирается в края экрана.
static int g_accum_dx = 0;
static int g_accum_dy = 0;

// Чувствительность указателя в сотых долях (100 = как даёт контроллер).
// Остаток копим отдельно: иначе на 50% каждое второе движение терялось бы
// целиком, и курсор дёргался бы вместо того, чтобы идти медленнее.
static int g_sens = 100;
static int g_accel = 1;            // 1 = ускорение по скорости движения, как в X11
static int g_frac_x = 0, g_frac_y = 0;

void mouse_set_speed(int percent, int accel) {
    if (percent >= 25 && percent <= 400) g_sens = percent;
    if (accel == 0 || accel == 1) g_accel = accel;
    g_frac_x = g_frac_y = 0;
}
int mouse_get_speed(void) { return g_sens; }
int mouse_get_accel(void) { return g_accel; }

// Масштабирование шага с накоплением остатка
static int scale_delta(int d, int* frac, int mul) {
    int v = d * mul + *frac;
    int out = v / 100;
    *frac = v - out * 100;
    return out;
}

static uint8_t g_packet[4];
static uint8_t g_cycle = 0;
static uint8_t g_packet_len = 3;   // 4 — мышь с колесом (IntelliMouse, ID 3)
static int     g_accum_wheel = 0;  // щелчки колеса с последнего чтения (+ вниз, - вверх)

static inline uint64_t mouse_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void mouse_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) { if ((inb(0x64) & 1) == 1) return; }
    } else {
        while (timeout--) { if ((inb(0x64) & 2) == 0) return; }
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

// Есть ли контроллер PS/2 вообще. На машине без него (настольный ПК с одними
// USB-устройствами, QEMU с i8042=off) порты читаются как 0xFF, а это значит
// «данные есть»: цикл ниже и ps2_hw_service крутились бы вечно, и загрузка
// вставала на «Starting PS/2 Keyboard and Mouse...»
int g_ps2_present = 1;

void init_ps2_mouse(void) {
    uint8_t status;
    if (inb(0x64) == 0xFF) { g_ps2_present = 0; return; }

    // Включаем порт мыши
    mouse_wait(1);
    outb(0x64, 0xA8);

    // Читаем Command Byte
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    status = inb(0x60);

    // Разрешаем мышь, отключаем IRQ12 во избежание зависания PIC без EOI
    status |= 0x41;
    status &= ~(0x02 | 0x20);

    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    // Сброс и дефолты (0xF6)
    mouse_write(0xF6);
    mouse_read();

    // IntelliMouse: частоты 200, 100, 80 подряд включают колесо (как psmouse в Linux),
    // после чего мышь с колесом отвечает ID 3 и шлёт 4-байтные пакеты
    static const uint8_t magic[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_write(0xF3); mouse_read();
        mouse_write(magic[i]); mouse_read();
    }
    mouse_write(0xF2); mouse_read();          // ACK
    uint8_t id = mouse_read();
    g_packet_len = (id == 3) ? 4 : 3;
    mouse_write(0xF3); mouse_read();          // обычная частота 100 Гц
    mouse_write(100);  mouse_read();

    // Разрешаем поток данных (0xF4)
    mouse_write(0xF4);
    mouse_read();

    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);

    if (screen_width)  g_mouse_x = screen_width / 2;
    if (screen_height) g_mouse_y = screen_height / 2;
    g_cycle = 0;
}

// Приём байта из ps2_hw_service()
void mouse_feed_byte(uint8_t byte) {
    if (g_cycle == 0) {
        // Бит 3 первого байта PS/2 всегда равен 1
        if ((byte & 0x08) == 0) return;
        g_packet[0] = byte;
        g_cycle = 1;
    } else if (g_cycle == 1) {
        g_packet[1] = byte;
        g_cycle = 2;
    } else if (g_cycle == 2 && g_packet_len == 4) {
        g_packet[2] = byte;
        g_cycle = 3;
    } else {
        g_packet[g_cycle] = byte;
        g_cycle = 0;

        if (g_packet_len == 4) {
            int dz = (int8_t)(g_packet[3] << 4) >> 4;   // младшие 4 бита — знаковый щелчок колеса
            uint64_t wf = mouse_irq_save();
            g_accum_wheel += dz;
            if (g_accum_wheel > 1000) g_accum_wheel = 1000;
            if (g_accum_wheel < -1000) g_accum_wheel = -1000;
            mouse_irq_restore(wf);
        }

        // Прямое знаковое приведение байт 1 и 2 (аппаратный int8_t); Y у PS/2 — вверх
        mouse_feed_report((int8_t)g_packet[1], -(int8_t)g_packet[2], 0, g_packet[0] & 0x07);
    }
}

// Одно движение мыши в экранных осях (вправо +, вниз +): от PS/2 (пакет выше)
// или от USB-мыши (drivers/usb/hid.c). wheel — щелчки колеса (+ вниз)
void mouse_feed_report(int dx, int dy, int wheel, uint8_t buttons) {
    if (wheel) {
        uint64_t wf = mouse_irq_save();
        g_accum_wheel += wheel;
        if (g_accum_wheel > 1000) g_accum_wheel = 1000;
        if (g_accum_wheel < -1000) g_accum_wheel = -1000;
        mouse_irq_restore(wf);
    }
    {
        g_mouse_btn = buttons & 0x07;

        g_last_dx = dx;
        g_last_dy = dy;

        // Копим дельты атомарно: читатель (syscall игры) может прервать нас таймером
        uint64_t fl = mouse_irq_save();
        g_accum_dx += dx;
        g_accum_dy += dy;
        if (g_accum_dx >  100000) g_accum_dx =  100000;
        if (g_accum_dx < -100000) g_accum_dx = -100000;
        if (g_accum_dy >  100000) g_accum_dy =  100000;
        if (g_accum_dy < -100000) g_accum_dy = -100000;
        mouse_irq_restore(fl);

        // Чувствительность и ускорение применяем только к указателю: сырые
        // дельты (g_accum_*, их читают игры через сисколл) остаются как есть.
        int mul = g_sens;
        if (g_accel) {
            int speed = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (speed > 24)      mul = mul * 2;
            else if (speed > 10) mul = mul * 3 / 2;
        }
        g_mouse_x += scale_delta(dx, &g_frac_x, mul);
        g_mouse_y += scale_delta(dy, &g_frac_y, mul);

        // Ограничение по экрану
        int max_w = screen_width ? (int)screen_width - 2 : 1022;
        int max_h = screen_height ? (int)screen_height - 2 : 766;

        if (g_mouse_x < 0) g_mouse_x = 0;
        if (g_mouse_x > max_w) g_mouse_x = max_w;
        if (g_mouse_y < 0) g_mouse_y = 0;
        if (g_mouse_y > max_h) g_mouse_y = max_h;
    }
}

void update_mouse_state(void) {
    ps2_hw_service();
}

int get_mouse_x(void)   { return g_mouse_x; }
int get_mouse_y(void)   { return g_mouse_y; }
int get_mouse_btn(void) { return g_mouse_btn & 0x01; }
uint8_t get_mouse_buttons_raw(void) { return g_mouse_btn; }

// Движение мыши (экранные координаты: вправо +, вниз +) накопленное с прошлого вызова,
// и текущие кнопки (бит0 ЛКМ, бит1 ПКМ, бит2 СКМ). После вызова накопитель обнуляется.
void get_mouse_delta(int* dx, int* dy, uint8_t* buttons) {
    uint64_t fl = mouse_irq_save();
    int ax = g_accum_dx, ay = g_accum_dy;
    g_accum_dx = 0;
    g_accum_dy = 0;
    mouse_irq_restore(fl);
    if (dx) *dx = ax;
    if (dy) *dy = ay;
    if (buttons) *buttons = g_mouse_btn;
}

int get_mouse_last_dx(void) { return g_last_dx; }
int get_mouse_last_dy(void) { return g_last_dy; }
// Щелчки колеса с прошлого вызова (+ вниз, - вверх), счётчик обнуляется
int get_mouse_wheel_delta(void) {
    uint64_t fl = mouse_irq_save();
    int dz = g_accum_wheel;
    g_accum_wheel = 0;
    mouse_irq_restore(fl);
    return dz;
}
