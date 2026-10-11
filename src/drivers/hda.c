// Универсальный драйвер Intel HDA.
// Ничего не зашито под конкретный кодек: контроллеры, кодеки, виджеты и
// маршруты "пин -> ... -> ЦАП" определяются автоматически.
//
// hda_init():
//   1. перебирает ВСЕ HDA-контроллеры (класс 04:03) и все их кодеки;
//   2. составляет инвентарь выходов: встроенные динамики, разъём наушников,
//      line-out, цифровые порты HDMI/DisplayPort, SPDIF;
//   3. выбирает кодек с аналоговым выходом и настраивает его (питание, mute,
//      селекторы, пины, EAPD, ЦАП, формат);
//   4. если аналоговых выходов нет (например, ПК только с HDMI), возвращает -3,
//      но инвентарь остаётся доступен через check_audio_outputs().
//
// check_audio_outputs() сообщает, что доступно, и перечитывает "живое" состояние:
// воткнуты ли наушники и подключён ли дисплей к HDMI.
//
// Вывод звука: аналоговые динамики/наушники/line-out ИЛИ HDMI/DisplayPort
// (hda_set_output: HDA_OUT_AUTO / SPEAKERS / HEADPHONES / HDMI).
// Драйвер не потокобезопасен: не вызывай его из двух задач одновременно.

#include "hda.h"
#include "pci.h"
#include <stddef.h>
#include <stdint.h>

extern void kputs(const char* str, uint32_t color);

// ---------------------------------------------------------------- настройки
#define HDA_STREAM_TAG   1
#define HDA_GPIO_MASK    0   // Если динамик молчит при идущем DMA: попробуй 0x01 (GPIO0 -> high).

// ---------------------------------------------------------------- регистры
#define R_GCAP      0x00
#define R_GCTL      0x08
#define R_STATESTS  0x0E
#define R_INTCTL    0x20
#define R_CORBLBASE 0x40
#define R_CORBUBASE 0x44
#define R_CORBWP    0x48
#define R_CORBRP    0x4A
#define R_CORBCTL   0x4C
#define R_CORBSIZE  0x4E
#define R_RIRBLBASE 0x50
#define R_RIRBUBASE 0x54
#define R_RIRBWP    0x58
#define R_RINTCNT   0x5A
#define R_RIRBCTL   0x5C
#define R_RIRBSTS   0x5D
#define R_RIRBSIZE  0x5E
#define R_ICO       0x60
#define R_ICI       0x64
#define R_ICS       0x68

#define SD_CTL    0x00
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDLPL  0x18
#define SD_BDLPU  0x1C

#define W_DAC 0
#define W_ADC 1
#define W_MIX 2
#define W_SEL 3
#define W_PIN 4

#pragma pack(push, 1)
typedef struct {
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t length;
    uint32_t flags;
} hda_bdl_entry_t;
#pragma pack(pop)

#define MAX_CONN 32
typedef struct {
    uint8_t  type;
    uint8_t  nconn;
    uint16_t conn[MAX_CONN];
    uint32_t caps;     // Audio Widget Capabilities
    uint32_t pincfg;   // Pin Default Config (только для пинов)
    uint32_t pincap;   // Pin Capabilities   (только для пинов)
} widget_t;

typedef struct {
    pci_device_t pci;
    uint64_t     base;
} hda_ctrl_t;

// ---------------------------------------------------------------- состояние
// У каждого контроллера свой контекст (MMIO, CORB/RIRB), активный лежит в глобальных переменных.
#define MAX_CTRL 6
typedef struct {
    uintptr_t mmio;
    uint32_t  out_off;
    int       corb_n, rirb_n;
    uint16_t  rirb_rp;
    int       use_imm;
} hda_ctx_t;

static hda_ctx_t s_ctx[MAX_CTRL];
static volatile uint32_t s_corb_mem[MAX_CTRL][256] __attribute__((aligned(128)));
static volatile uint64_t s_rirb_mem[MAX_CTRL][256] __attribute__((aligned(128)));

static volatile uint32_t* s_corb = s_corb_mem[0];
static volatile uint64_t* s_rirb = s_rirb_mem[0];
static uintptr_t s_hda_mmio = 0;
static uint32_t  s_out_stream_offset = 0x100;
static int       s_corb_n = 256, s_rirb_n = 256;
static uint16_t  s_rirb_rp = 0;
static int       s_use_imm = 0;
static int       s_cur = -1;       // индекс контроллера, чей контекст сейчас в глобальных переменных
static int       s_active = -1;    // контроллер, через который играем

static int s_hda_status = HDA_STATUS_NOT_FOUND;

static widget_t s_w[256];
static int s_cad, s_wstart, s_wend;
static uint32_t s_afg_out_caps, s_afg_in_caps, s_afg_pcm;

// найденные аналоговые выходы активного кодека (для переключения динамики/наушники)
#define MAX_OUT 8
static uint8_t s_out_pins[MAX_OUT];
static int     s_out_n = 0;
static int     s_out_mode = HDA_OUT_AUTO;
static int     s_hp_plugged = 0;

// цифровые порты (HDMI/DisplayPort) на всех контроллерах - для опроса подключения дисплея
#define MAX_HDMI 16
typedef struct {
    uint8_t  ctrl, cad, afg, nid;   // где живёт пин
    uint8_t  conv, conv_idx;        // цифровой конвертер (ЦАП) и его номер в списке подключений пина (conv=0: нет)
    uint32_t pincap;
} hdmi_pin_t;
static hdmi_pin_t s_hdmi[MAX_HDMI];
static uint8_t    s_hdmi_pres[MAX_HDMI];   // дисплей подключён к пину (по последнему опросу)
static int s_hdmi_n = 0;
static int s_hd_connected = 0;             // сколько дисплеев подключено (по последнему hda_jack_poll)
static int s_hd_sel = -1;                  // выбранный HDMI-пин (индекс в s_hdmi)

#define TGT_NONE   0
#define TGT_ANALOG 1
#define TGT_HDMI   2
static int s_target = TGT_NONE;            // куда сейчас идёт звук
static int s_play_ctrl = -1;               // контроллер, на котором запущен стрим

static hda_outputs_t s_inv;
static int s_inv_valid = 0;

#define TONE_FRAMES_PER_BLOCK 480   // 480 кадров = ровно 10 периодов 1 кГц при 48 кГц
#define NUM_BLOCKS 2
static hda_bdl_entry_t s_bdl[NUM_BLOCKS] __attribute__((aligned(128)));
static int16_t s_pcm_buf[NUM_BLOCKS][TONE_FRAMES_PER_BLOCK * 2] __attribute__((aligned(128)));

// ---------------------------------------------------------------- низкий уровень
static inline uint8_t  hda_read8(uint32_t r)  { return *(volatile uint8_t*)(s_hda_mmio + r); }
static inline uint16_t hda_read16(uint32_t r) { return *(volatile uint16_t*)(s_hda_mmio + r); }
static inline uint32_t hda_read32(uint32_t r) { return *(volatile uint32_t*)(s_hda_mmio + r); }
static inline void hda_write8(uint32_t r, uint8_t v)   { *(volatile uint8_t*)(s_hda_mmio + r) = v; }
static inline void hda_write16(uint32_t r, uint16_t v) { *(volatile uint16_t*)(s_hda_mmio + r) = v; }
static inline void hda_write32(uint32_t r, uint32_t v) { *(volatile uint32_t*)(s_hda_mmio + r) = v; }

static inline uintptr_t hda_virt_to_phys(void* vaddr) {
    uintptr_t a = (uintptr_t)vaddr;
    if (a >= 0xFFFFFFFF80000000ULL) return a - 0xFFFFFFFF80000000ULL;
    return a;
}

// ~1 мкс на итерацию: чтение порта 0x80 не зависит от частоты CPU
static inline uint8_t inb_(uint16_t p) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}
static void udelay(uint32_t us) { while (us--) inb_(0x80); }

static uint32_t rd(uint32_t reg, int w) {
    return w == 1 ? hda_read8(reg) : (w == 2 ? hda_read16(reg) : hda_read32(reg));
}
static int wait_for(uint32_t reg, int w, uint32_t mask, uint32_t want, uint32_t us) {
    while (us--) {
        if ((rd(reg, w) & mask) == want) return 1;
        udelay(1);
    }
    return 0;
}

static void zero(volatile void* p, size_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    while (n--) *b++ = 0;
}

static void put_hex(uint32_t v, uint32_t color) {
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF];
    b[10] = 0;
    kputs(b, color);
}

// Переключение на контекст другого контроллера (состояние текущего сохраняется)
static void ctx_select(int i) {
    if (i < 0 || i >= MAX_CTRL || s_cur == i) return;
    if (s_cur >= 0) {
        hda_ctx_t* o = &s_ctx[s_cur];
        o->mmio = s_hda_mmio;  o->out_off = s_out_stream_offset;
        o->corb_n = s_corb_n;  o->rirb_n = s_rirb_n;
        o->rirb_rp = s_rirb_rp; o->use_imm = s_use_imm;
    }
    hda_ctx_t* n = &s_ctx[i];
    s_hda_mmio = n->mmio;  s_out_stream_offset = n->out_off;
    s_corb_n = n->corb_n ? n->corb_n : 256;
    s_rirb_n = n->rirb_n ? n->rirb_n : 256;
    s_rirb_rp = n->rirb_rp; s_use_imm = n->use_imm;
    s_corb = s_corb_mem[i];
    s_rirb = s_rirb_mem[i];
    s_cur = i;
}

// ---------------------------------------------------------------- обмен с кодеком
static int imm_xfer(uint32_t verb, uint32_t* resp) {
    if (!wait_for(R_ICS, 2, 1, 0, 10000)) return -1;
    hda_write16(R_ICS, 2);              // сброс IRV (write-1-to-clear)
    hda_write32(R_ICO, verb);
    hda_write16(R_ICS, 1);              // ICB: запуск
    if (!wait_for(R_ICS, 2, 3, 2, 50000)) return -1;   // ICB=0, IRV=1
    *resp = hda_read32(R_ICI);
    hda_write16(R_ICS, 2);
    return 0;
}

static int corb_rirb_init(void) {
    hda_write8(R_CORBCTL, 0);
    hda_write8(R_RIRBCTL, 0);
    wait_for(R_CORBCTL, 1, 2, 0, 10000);
    wait_for(R_RIRBCTL, 1, 2, 0, 10000);

    uint8_t sz = hda_read8(R_CORBSIZE);
    if      (sz & 0x40) { hda_write8(R_CORBSIZE, (sz & ~3) | 2); s_corb_n = 256; }
    else if (sz & 0x20) { hda_write8(R_CORBSIZE, (sz & ~3) | 1); s_corb_n = 16;  }
    else                { hda_write8(R_CORBSIZE, (sz & ~3) | 0); s_corb_n = 2;   }

    sz = hda_read8(R_RIRBSIZE);
    if      (sz & 0x40) { hda_write8(R_RIRBSIZE, (sz & ~3) | 2); s_rirb_n = 256; }
    else if (sz & 0x20) { hda_write8(R_RIRBSIZE, (sz & ~3) | 1); s_rirb_n = 16;  }
    else                { hda_write8(R_RIRBSIZE, (sz & ~3) | 0); s_rirb_n = 2;   }

    zero(s_corb, 256 * sizeof(uint32_t));
    zero(s_rirb, 256 * sizeof(uint64_t));

    uint64_t cp = hda_virt_to_phys((void*)s_corb);
    uint64_t rp = hda_virt_to_phys((void*)s_rirb);
    hda_write32(R_CORBLBASE, (uint32_t)cp);
    hda_write32(R_CORBUBASE, (uint32_t)(cp >> 32));
    hda_write32(R_RIRBLBASE, (uint32_t)rp);
    hda_write32(R_RIRBUBASE, (uint32_t)(rp >> 32));

    hda_write16(R_CORBRP, 0x8000);
    wait_for(R_CORBRP, 2, 0x8000, 0x8000, 10000);
    hda_write16(R_CORBRP, 0);
    wait_for(R_CORBRP, 2, 0x8000, 0, 10000);
    hda_write16(R_CORBWP, 0);

    hda_write16(R_RIRBWP, 0x8000);
    hda_write16(R_RINTCNT, 1);
    hda_write8(R_RIRBSTS, 0x05);
    s_rirb_rp = 0;

    hda_write8(R_RIRBCTL, 0x02);   // RIRB DMA enable
    hda_write8(R_CORBCTL, 0x02);   // CORB DMA enable
    return (hda_read8(R_CORBCTL) & 2) ? 0 : -1;
}

static int hda_xfer(uint32_t verb, uint32_t* resp) {
    if (s_use_imm) return imm_xfer(verb, resp);

    uint16_t wp = (uint16_t)((hda_read16(R_CORBWP) + 1) % s_corb_n);
    s_corb[wp] = verb;
    __asm__ volatile("mfence" ::: "memory");
    hda_write16(R_CORBWP, wp);

    for (uint32_t t = 0; t < 50000; t++) {
        uint16_t rwp = hda_read16(R_RIRBWP) & 0xFF;
        while (s_rirb_rp != rwp) {
            s_rirb_rp = (uint16_t)((s_rirb_rp + 1) % s_rirb_n);
            uint64_t e = s_rirb[s_rirb_rp];
            if ((e >> 32) & 0x10) continue;      // unsolicited - пропускаем
            *resp = (uint32_t)e;
            return 0;
        }
        udelay(1);
    }
    return -1;
}

static uint32_t cmd12(int cad, int nid, uint32_t verb, uint32_t p) {
    return ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | ((verb & 0xFFF) << 8) | (p & 0xFF);
}
static uint32_t cmd4(int cad, int nid, uint32_t verb, uint32_t p) {
    return ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | ((verb & 0xF) << 16) | (p & 0xFFFF);
}
static uint32_t hda_get(int cad, int nid, uint32_t verb, uint32_t p) {
    uint32_t r = 0;
    if (hda_xfer(cmd12(cad, nid, verb, p), &r) < 0) return 0;
    return r;
}
static void hda_set12(int cad, int nid, uint32_t verb, uint32_t p) {
    uint32_t r;
    hda_xfer(cmd12(cad, nid, verb, p), &r);
}
static void hda_set4(int cad, int nid, uint32_t verb, uint32_t p) {
    uint32_t r;
    hda_xfer(cmd4(cad, nid, verb, p), &r);
}

// ---------------------------------------------------------------- виджеты
static void read_conn(int nid) {
    widget_t* w = &s_w[nid];
    uint32_t lp = hda_get(s_cad, nid, 0xF00, 0x0E);
    int n = lp & 0x7F;
    int longform = (lp >> 7) & 1;
    int per = longform ? 2 : 4;
    int bits = longform ? 16 : 8;
    int prev = 0;
    w->nconn = 0;
    for (int i = 0; i < n; i += per) {
        uint32_t r = hda_get(s_cad, nid, 0xF02, i);
        for (int j = 0; j < per && i + j < n; j++) {
            uint32_t e = (r >> (j * bits)) & ((1u << bits) - 1);
            int range = (e >> (bits - 1)) & 1;
            int id = e & ((1u << (bits - 1)) - 1);
            if (range && prev) {
                for (int k = prev + 1; k <= id && w->nconn < MAX_CONN; k++) w->conn[w->nconn++] = (uint16_t)k;
            } else if (w->nconn < MAX_CONN) {
                w->conn[w->nconn++] = (uint16_t)id;
            }
            prev = id;
        }
    }
}

// DFS: от пина вниз по списку подключений до ЦАП (аналогового)
static int find_path(int nid, int depth, uint8_t* path, uint8_t* sel, int* len) {
    path[depth] = (uint8_t)nid;
    widget_t* w = &s_w[nid];
    if (w->type == W_DAC) {
        if (w->caps & (1u << 9)) return 0;      // цифровой ЦАП (SPDIF/HDMI) не берём
        *len = depth + 1;
        return 1;
    }
    if (depth >= 6) return 0;
    if (w->type != W_PIN && w->type != W_MIX && w->type != W_SEL) return 0;
    for (int i = 0; i < w->nconn; i++) {
        int c = w->conn[i];
        if (c < s_wstart || c >= s_wend) continue;
        sel[depth] = (uint8_t)i;
        if (find_path(c, depth + 1, path, sel, len)) return 1;
    }
    return 0;
}

static int pick_gain(uint32_t caps) {
    int steps = (caps >> 8) & 0x7F;
    int ofs = caps & 0x7F;
    int g = ofs ? ofs : steps / 2;     // 0 дБ, а если его нет - середина шкалы
    if (g > steps) g = steps;
    return g;
}
static uint32_t amp_caps(int nid, int in) {
    uint32_t c = 0;
    if (s_w[nid].caps & (1u << 3)) c = hda_get(s_cad, nid, 0xF00, in ? 0x0D : 0x12);
    if (!c) c = in ? s_afg_in_caps : s_afg_out_caps;
    return c;
}
static void unmute_out_amp(int nid) {
    int g = pick_gain(amp_caps(nid, 0));
    hda_set4(s_cad, nid, 0x3, 0xB000 | g);                    // output, L+R, mute=0
}
static void unmute_in_amp(int nid, int idx) {
    int g = pick_gain(amp_caps(nid, 1));
    hda_set4(s_cad, nid, 0x3, 0x7000 | ((idx & 0xF) << 8) | g); // input idx, L+R, mute=0
}

static void setup_path(int pin, const uint8_t* path, const uint8_t* sel, int len) {
    int dev = (s_w[pin].pincfg >> 20) & 0xF;
    for (int i = 0; i < len; i++) {
        int nid = path[i];
        widget_t* w = &s_w[nid];

        hda_set12(s_cad, nid, 0x705, 0);                       // D0
        if (i < len - 1) {
            if ((w->type == W_SEL || w->type == W_PIN) && w->nconn > 1)
                hda_set12(s_cad, nid, 0x701, sel[i]);          // выбор входа
            if (w->caps & (1u << 1)) unmute_in_amp(nid, sel[i]);
        }
        if (w->caps & (1u << 2)) unmute_out_amp(nid);

        if (w->type == W_PIN) {
            hda_set12(s_cad, nid, 0x707, dev == 2 ? 0xC0 : 0x40);   // OUT_EN (+HP_EN)
            if (w->pincap & (1u << 16)) hda_set12(s_cad, nid, 0x70C, 0x02); // EAPD on
        }
        if (w->type == W_DAC) {
            hda_set12(s_cad, nid, 0x706, (HDA_STREAM_TAG << 4) | 0);  // stream tag, канал 0
            hda_set4(s_cad, nid, 0x2, 0x0011);                        // 48 кГц, 16 бит, стерео
        }
    }
}

// Только чтение: питание AFG в D0, параметры и дерево виджетов (без настройки маршрутов)
static void scan_widgets(int cad, int afg) {
    s_cad = cad;
    hda_set12(cad, afg, 0x705, 0);
    udelay(10000);

    s_afg_out_caps = hda_get(cad, afg, 0xF00, 0x12);
    s_afg_in_caps  = hda_get(cad, afg, 0xF00, 0x0D);
    s_afg_pcm      = hda_get(cad, afg, 0xF00, 0x0A);

    uint32_t sub = hda_get(cad, afg, 0xF00, 0x04);
    s_wstart = (sub >> 16) & 0xFF;
    s_wend = s_wstart + (sub & 0xFF);
    if (s_wend > 256) s_wend = 256;

    zero((volatile void*)s_w, sizeof(s_w));
    for (int nid = s_wstart; nid < s_wend; nid++) {
        widget_t* w = &s_w[nid];
        w->caps = hda_get(cad, nid, 0xF00, 0x09);
        w->type = (w->caps >> 20) & 0xF;
        if (w->type == W_PIN) {
            w->pincfg = hda_get(cad, nid, 0xF1C, 0);
            w->pincap = hda_get(cad, nid, 0xF00, 0x0C);
        }
        if (w->caps & (1u << 8)) read_conn(nid);
    }
}

// Presence Detect пина (воткнут штекер / подключён дисплей)
static uint32_t sense_raw(int cad, int nid, uint32_t pincap) {
    if (!(pincap & (1u << 2))) return 0;                     // пин не умеет определять подключение
    if (pincap & (1u << 1)) {                                // нужен триггер перед чтением
        hda_set12(cad, nid, 0x709, 0);
        udelay(1000);
    }
    return hda_get(cad, nid, 0xF09, 0);                      // бит 31 = presence, бит 30 = ELD valid
}
static int sense_pin(int cad, int nid, uint32_t pincap) {
    return (sense_raw(cad, nid, pincap) >> 31) & 1;
}

static int setup_afg(int cad, int afg) {
    scan_widgets(cad, afg);
    s_out_n = 0;

    if (HDA_GPIO_MASK) {
        hda_set12(cad, afg, 0x716, HDA_GPIO_MASK);   // enable
        hda_set12(cad, afg, 0x717, HDA_GPIO_MASK);   // direction = out
        hda_set12(cad, afg, 0x715, HDA_GPIO_MASK);   // data = 1
    }

    if (!((s_afg_pcm >> 6) & 1) || !((s_afg_pcm >> 17) & 1))
        kputs("[HDA] Внимание: кодек не заявляет 48 кГц/16 бит\n", 0xFFFF55);

    int ok = 0;
    for (int pass = 0; pass < 2 && !ok; pass++) {
        for (int nid = s_wstart; nid < s_wend; nid++) {
            widget_t* w = &s_w[nid];
            if (w->type != W_PIN) continue;
            if (!(w->pincap & (1u << 4))) continue;             // не умеет выход
            int dev  = (w->pincfg >> 20) & 0xF;                 // 0 line, 1 speaker, 2 HP
            int conn = (w->pincfg >> 30) & 3;                   // 1 = не подключён
            if (dev > 2) continue;
            if (pass == 0 && conn == 1) continue;
            if (!w->nconn) continue;

            uint8_t path[8], sel[8];
            int len = 0;
            if (!find_path(nid, 0, path, sel, &len)) continue;

            setup_path(nid, path, sel, len);
            kputs("[HDA] выход: пин ", 0xAAAAAA); put_hex(nid, 0xFFFFFF);
            kputs(" cfg ", 0xAAAAAA);             put_hex(w->pincfg, 0xFFFFFF);
            kputs(dev == 1 ? " (динамик)" : (dev == 2 ? " (наушники)" : " (line-out)"), 0x55FF55);
            kputs(" -> ЦАП ", 0xAAAAAA);          put_hex(path[len - 1], 0xFFFFFF);
            kputs("\n", 0);
            if (s_out_n < MAX_OUT) s_out_pins[s_out_n++] = (uint8_t)nid;
            ok++;
        }
    }
    return ok > 0 ? 0 : -1;
}

// ---------------------------------------------------------------- инвентаризация выходов
// Считает выходы одной функциональной группы в s_inv. Ничего не настраивает.
// *strict  - число аналоговых выходов, которые BIOS объявил подключёнными (и до них есть путь к ЦАП)
// *relaxed - то же, но без учёта поля "не подключён" (запасной вариант для кривых BIOS)
static void inventory_afg(int ci, int cad, int afg, int* strict, int* relaxed) {
    scan_widgets(cad, afg);
    *strict = 0;
    *relaxed = 0;
    uint8_t used[256];                                           // какие конвертеры уже розданы пинам
    zero((volatile void*)used, sizeof(used));

    for (int nid = s_wstart; nid < s_wend; nid++) {
        widget_t* w = &s_w[nid];
        if (w->type != W_PIN) continue;
        if (!(w->pincap & (1u << 4))) continue;                 // не умеет выход
        int dev  = (w->pincfg >> 20) & 0xF;
        int conn = (w->pincfg >> 30) & 3;

        if (dev <= 2) {                                          // аналоговый выход
            uint8_t path[8], sel[8];
            int len = 0;
            if (!w->nconn || !find_path(nid, 0, path, sel, &len)) continue;
            (*relaxed)++;
            if (conn == 1) continue;
            (*strict)++;
            if (dev == 1)      s_inv.has_speakers = 1;
            else if (dev == 2) s_inv.has_headphones = 1;
            else               s_inv.has_lineout = 1;
        } else if (dev == 4 || dev == 5) {                       // цифровой выход
            if (conn == 1) continue;
            int hdmi = (dev == 5) || (w->pincap & (1u << 7)) || (w->pincap & (1u << 24));
            if (hdmi) {
                s_inv.has_hdmi++;
                if (s_hdmi_n < MAX_HDMI) {
                    hda_set12(cad, nid, 0x705, 0);               // пин в D0, чтобы корректно отвечал sense
                    int conv = 0, cidx = 0, found = 0;           // ищем цифровой конвертер, лучше свободный
                    for (int k = 0; k < w->nconn; k++) {
                        int c = w->conn[k];
                        if (c < s_wstart || c >= s_wend || s_w[c].type != W_DAC) continue;
                        if (!found) { conv = c; cidx = k; found = 1; }
                        if (!used[c]) { conv = c; cidx = k; break; }
                    }
                    if (conv) used[conv] = 1;
                    hdmi_pin_t* h = &s_hdmi[s_hdmi_n++];
                    h->ctrl = (uint8_t)ci;  h->cad = (uint8_t)cad;
                    h->afg = (uint8_t)afg;  h->nid = (uint8_t)nid;
                    h->conv = (uint8_t)conv; h->conv_idx = (uint8_t)cidx;
                    h->pincap = w->pincap;
                }
            } else {
                s_inv.has_spdif = 1;
            }
        }
    }
}

// ---------------------------------------------------------------- контроллеры и кодеки
static int codec_probe(int cad, uint32_t* vid) {
    *vid = 0;
    if (hda_xfer(cmd12(cad, 0, 0xF00, 0x00), vid) < 0 || *vid == 0) {
        s_use_imm = 1;                                           // CORB/RIRB молчит - Immediate Command
        if (imm_xfer(cmd12(cad, 0, 0xF00, 0x00), vid) < 0 || *vid == 0) {
            s_use_imm = 0;
            return 0;
        }
    }
    return 1;
}

typedef struct { int ctrl, cad, afg; } afg_ref_t;

// Просматривает кодек; запоминает первый подходящий аналоговый AFG.
static void scan_codec(int ci, int cad, afg_ref_t* best, afg_ref_t* fallback) {
    uint32_t vid;
    if (!codec_probe(cad, &vid)) return;
    s_inv.codecs++;
    kputs("[HDA] кодек ", 0xAAAAAA); put_hex(cad, 0xFFFFFF);
    kputs(" vendor/device ", 0xAAAAAA); put_hex(vid, 0x55FFFF); kputs("\n", 0);

    uint32_t root = hda_get(cad, 0, 0xF00, 0x04);
    int fstart = (root >> 16) & 0xFF;
    int fcount = root & 0xFF;
    for (int fg = fstart; fg < fstart + fcount && fg < 256; fg++) {
        if ((hda_get(cad, fg, 0xF00, 0x05) & 0xFF) != 1) continue;   // только Audio Function Group
        int strict, relaxed;
        inventory_afg(ci, cad, fg, &strict, &relaxed);
        if (strict > 0 && best->ctrl < 0)         { best->ctrl = ci;     best->cad = cad;     best->afg = fg; }
        if (relaxed > 0 && fallback->ctrl < 0)    { fallback->ctrl = ci; fallback->cad = cad; fallback->afg = fg; }
    }
}

static int find_controllers(hda_ctrl_t* out, int max) {
    int n = 0;
    for (uint16_t bus = 0; bus < 256 && n < max; bus++)
    for (uint8_t slot = 0; slot < 32 && n < max; slot++)
    for (uint8_t func = 0; func < 8 && n < max; func++) {
        uint32_t id = pci_config_read((uint8_t)bus, slot, func, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cls = pci_config_read((uint8_t)bus, slot, func, 0x08);
        if (((cls >> 24) & 0xFF) != 0x04 || ((cls >> 16) & 0xFF) != 0x03) continue;

        uint32_t bar = pci_config_read((uint8_t)bus, slot, func, 0x10);
        if (bar & 1) continue;                                   // не MMIO
        uint64_t base = bar & ~0xFULL;
        if (((bar >> 1) & 3) == 2)                               // 64-битный BAR
            base |= (uint64_t)pci_config_read((uint8_t)bus, slot, func, 0x14) << 32;
        if (!base) continue;

        hda_ctrl_t* c = &out[n++];
        c->pci.bus = (uint8_t)bus; c->pci.slot = slot; c->pci.func = func;
        c->pci.base_class = 0x04;  c->pci.subclass = 0x03;
        c->pci.prog_if   = (cls >> 8) & 0xFF;
        c->pci.vendor_id = id & 0xFFFF;
        c->pci.device_id = (id >> 16) & 0xFFFF;
        c->pci.bar0      = (uint32_t)(base & 0xFFFFFFFF);
        c->base          = base;
    }
    return n;
}

static void intel_quirks(const pci_device_t* d) {
    if (d->vendor_id != 0x8086) return;
    // TCSEL = 0 (Traffic Class 0), как делает Linux
    uint32_t v = pci_config_read(d->bus, d->slot, d->func, 0x44);
    pci_config_write(d->bus, d->slot, d->func, 0x44, v & ~7u);
    // сбросить Enable No Snoop (DEVC bit 11); старшие 16 бит (DEVSTS, W1C) пишем нулями
    v = pci_config_read(d->bus, d->slot, d->func, 0x78);
    pci_config_write(d->bus, d->slot, d->func, 0x78, v & 0x0000F7FFu);
}

// Поднимает контроллер: reset, ожидание кодеков, CORB/RIRB. Возвращает маску STATESTS (0 = неудача).
static uint16_t ctrl_bringup(int idx, hda_ctrl_t* c) {
    ctx_select(idx);

    kputs("[HDA] контроллер ", 0xAAAAAA);
    put_hex(c->pci.vendor_id, 0xFFFFFF); kputs(":", 0xAAAAAA);
    put_hex(c->pci.device_id, 0xFFFFFF); kputs(" bus/slot ", 0xAAAAAA);
    put_hex(c->pci.bus, 0xFFFFFF); kputs("/", 0xAAAAAA);
    put_hex(c->pci.slot, 0xFFFFFF); kputs(" BAR ", 0xAAAAAA);
    put_hex((uint32_t)c->base, 0xFFFFFF); kputs("\n", 0);

    pci_enable_bus_master(&c->pci);
    intel_quirks(&c->pci);

    s_hda_mmio = (uintptr_t)c->base;
    s_use_imm = 0;

    uint16_t gcap = hda_read16(R_GCAP);
    if (gcap == 0 || gcap == 0xFFFF) {
        kputs("[HDA]   MMIO не отвечает (проверь маппинг BAR как uncacheable)\n", 0xFF5555);
        return 0;
    }

    hda_write32(R_INTCTL, 0);
    hda_write32(R_GCTL, hda_read32(R_GCTL) & ~1u);
    wait_for(R_GCTL, 4, 1, 0, 100000);
    udelay(200);
    hda_write32(R_GCTL, hda_read32(R_GCTL) | 1u);
    if (!wait_for(R_GCTL, 4, 1, 1, 100000)) {
        kputs("[HDA]   контроллер не вышел из reset\n", 0xFF5555);
        return 0;
    }

    uint16_t st = 0;
    for (int t = 0; t < 300 && !st; t++) {          // кодеки поднимаются не мгновенно
        st = hda_read16(R_STATESTS) & 0x7FFF;
        if (!st) udelay(1000);
    }
    udelay(5000);
    st = hda_read16(R_STATESTS) & 0x7FFF;
    if (!st) {
        kputs("[HDA]   кодеки не обнаружены\n", 0xFF5555);
        return 0;
    }

    uint8_t num_iss = (gcap >> 8) & 0x0F;
    uint8_t num_oss = (gcap >> 12) & 0x0F;
    if (!num_oss) {
        kputs("[HDA]   нет выходных стримов\n", 0xFF5555);
        return 0;
    }
    s_out_stream_offset = 0x80 + num_iss * 0x20;

    if (corb_rirb_init() < 0) s_use_imm = 1;
    return st;
}

// ---------------------------------------------------------------- HDMI
static int hdmi_available(void) {
    for (int i = 0; i < s_hdmi_n; i++) if (s_hdmi[i].conv) return 1;
    return 0;
}

// Опрашивает все HDMI-пины (на любых контроллерах). Возвращает число подключённых дисплеев.
static int hdmi_refresh(void) {
    int prev = s_cur, cnt = 0;
    for (int i = 0; i < s_hdmi_n; i++) {
        ctx_select(s_hdmi[i].ctrl);
        s_hdmi_pres[i] = (uint8_t)sense_pin(s_hdmi[i].cad, s_hdmi[i].nid, s_hdmi[i].pincap);
        cnt += s_hdmi_pres[i];
    }
    if (prev >= 0) ctx_select(prev);
    return cnt;
}

// Пин с подключённым дисплеем, иначе первый пригодный
static int hdmi_pick(void) {
    int first = -1;
    for (int i = 0; i < s_hdmi_n; i++) {
        if (!s_hdmi[i].conv) continue;
        if (first < 0) first = i;
        if (s_hdmi_pres[i]) return i;
    }
    return first;
}

static void hdmi_unmute(int cad, int afg, int nid, int in_idx) {
    uint32_t caps = hda_get(cad, nid, 0xF00, 0x09);
    if (caps & (1u << 2)) {
        uint32_t oc = (caps & (1u << 3)) ? hda_get(cad, nid, 0xF00, 0x12) : 0;
        if (!oc) oc = hda_get(cad, afg, 0xF00, 0x12);
        hda_set4(cad, nid, 0x3, 0xB000 | pick_gain(oc));
    }
    if (in_idx >= 0 && (caps & (1u << 1))) {
        uint32_t ic = (caps & (1u << 3)) ? hda_get(cad, nid, 0xF00, 0x0D) : 0;
        if (!ic) ic = hda_get(cad, afg, 0xF00, 0x0D);
        hda_set4(cad, nid, 0x3, 0x7000 | ((in_idx & 0xF) << 8) | pick_gain(ic));
    }
}

// Audio InfoFrame (2 канала, формат берётся из потока): без него многие мониторы молчат
static void hdmi_send_infoframe(int cad, int pin) {
    uint8_t f[14] = { 0x84, 0x01, 0x0A, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // PB1: CC = 2 канала
    uint8_t sum = 0;
    for (int i = 0; i < 14; i++) sum = (uint8_t)(sum + f[i]);
    f[3] = (uint8_t)(0x100 - sum);                          // контрольная сумма
    hda_set12(cad, pin, 0x732, 0x00);                       // остановить передачу
    hda_set12(cad, pin, 0x730, 0x00);                       // индекс: пакет 0 (Audio InfoFrame), байт 0
    for (int i = 0; i < 14; i++) hda_set12(cad, pin, 0x731, f[i]);
    hda_set12(cad, pin, 0x732, 0xC0);                       // передавать постоянно
}

// Настройка кодека HDMI: пин -> цифровой конвертер -> стрим
static int hdmi_prepare(int ei) {
    if (ei < 0 || ei >= s_hdmi_n || !s_hdmi[ei].conv) return -1;
    hdmi_pin_t* h = &s_hdmi[ei];
    int prev = s_cur;
    ctx_select(h->ctrl);
    int cad = h->cad;

    uint32_t sense = sense_raw(cad, h->nid, h->pincap);
    kputs("[HDA] HDMI: пин ", 0xAAAAAA);        put_hex(h->nid, 0xFFFFFF);
    kputs(" -> конвертер ", 0xAAAAAA);          put_hex(h->conv, 0xFFFFFF);
    kputs((sense >> 31) & 1 ? ", дисплей: да" : ", дисплей: НЕТ", (sense >> 31) & 1 ? 0x55FF55 : 0xFF5555);
    kputs((sense >> 30) & 1 ? ", ELD: да\n" : ", ELD: нет\n", 0xAAAAAA);

    hda_set12(cad, h->afg, 0x705, 0);                // D0 для AFG, конвертера и пина
    hda_set12(cad, h->conv, 0x705, 0);
    hda_set12(cad, h->nid, 0x705, 0);
    hda_set12(cad, h->nid, 0x701, h->conv_idx);      // пин берёт звук с этого конвертера
    hdmi_unmute(cad, h->afg, h->conv, -1);
    hdmi_unmute(cad, h->afg, h->nid, h->conv_idx);

    hda_set12(cad, h->conv, 0x70D, 0x01);            // Digital Enable (обычный PCM, не "non-audio")
    hda_set12(cad, h->conv, 0x72D, 0x01);            // число каналов - 1
    hda_set12(cad, h->conv, 0x706, (HDA_STREAM_TAG << 4) | 0);
    hda_set4(cad, h->conv, 0x2, 0x0011);             // 48 кГц, 16 бит, стерео
    for (int ch = 0; ch < 8; ch++)                   // прямое соответствие каналов и слотов
        hda_set12(cad, h->nid, 0x734, (ch << 4) | ch);
    hda_set12(cad, h->nid, 0x707, 0x40);             // OUT_EN
    hdmi_send_infoframe(cad, h->nid);

    if (prev >= 0) ctx_select(prev);
    return 0;
}

// ---------------------------------------------------------------- динамики / наушники
// dev из Pin Default Config: 1 = встроенный динамик, 0/2 = line-out/наушники (разъём)
static int pin_dev(int nid) { return (s_w[nid].pincfg >> 20) & 0xF; }

static void pin_enable(int nid, int on) {
    widget_t* w = &s_w[nid];
    hda_set12(s_cad, nid, 0x707, on ? (pin_dev(nid) == 2 ? 0xC0 : 0x40) : 0x00);
    if (w->pincap & (1u << 16)) hda_set12(s_cad, nid, 0x70C, on ? 0x02 : 0x00);   // EAPD (усилитель)
}

static int pin_plugged(int nid) { return sense_pin(s_cad, nid, s_w[nid].pincap); }

static void apply_routing(void) {
    if (s_active < 0) return;
    ctx_select(s_active);
    int spk_on, jack_on;
    if (s_out_mode == HDA_OUT_SPEAKERS)        { spk_on = 1; jack_on = 0; }
    else if (s_out_mode == HDA_OUT_HEADPHONES) { spk_on = 0; jack_on = 1; }
    else                                       { spk_on = !s_hp_plugged; jack_on = 1; }

    for (int i = 0; i < s_out_n; i++) {
        int nid = s_out_pins[i];
        pin_enable(nid, pin_dev(nid) == 1 ? spk_on : jack_on);
    }
}

// ---------------------------------------------------------------- выбор цели вывода
static int resolve_target(void) {
    int an = (s_out_n > 0), hd = hdmi_available();
    switch (s_out_mode) {
    case HDA_OUT_HDMI:       return hd ? TGT_HDMI : TGT_NONE;
    case HDA_OUT_SPEAKERS:
    case HDA_OUT_HEADPHONES: return an ? TGT_ANALOG : TGT_NONE;
    default:                                          // AUTO
        if (an && (s_inv.has_speakers || s_hp_plugged)) return TGT_ANALOG;   // ноутбук или воткнутые наушники
        if (hd && s_hd_connected > 0) return TGT_HDMI;                      // ПК: дисплей подключён
        if (an) return TGT_ANALOG;
        if (hd) return TGT_HDMI;
        return TGT_NONE;
    }
}

// Применить выбор: переключить цель, перенести играющий стрим
static void apply_target(void) {
    int t = resolve_target();
    int changed = (t != s_target);
    int was_playing = (s_hda_status == HDA_STATUS_PLAYING);

    if (changed && was_playing) hda_stop();
    s_target = t;
    if (t == TGT_ANALOG) apply_routing();
    else if (t == TGT_HDMI) s_hd_sel = hdmi_pick();
    if (changed && was_playing) hda_play_test_tone();
}

// Опрос разъёмов и HDMI. Вызывай периодически (главный цикл/таймер).
// Возвращает 1, если наушники воткнуты. В режиме AUTO сам переключает вывод.
int hda_jack_poll(void) {
    if (s_hda_status == HDA_STATUS_NOT_FOUND) return 0;
    int hp = 0, changed = 0;
    if (s_out_n && s_active >= 0) {
        ctx_select(s_active);
        for (int i = 0; i < s_out_n; i++) {
            int nid = s_out_pins[i];
            if (pin_dev(nid) != 1 && pin_plugged(nid)) hp = 1;
        }
    }
    int hd = hdmi_refresh();

    if (hp != s_hp_plugged) {
        s_hp_plugged = hp;
        kputs(hp ? "[HDA] наушники подключены\n" : "[HDA] наушники отключены\n", 0xFFFF55);
        changed = 1;
    }
    if (hd != s_hd_connected) {
        s_hd_connected = hd;
        kputs(hd ? "[HDA] HDMI: дисплей подключён\n" : "[HDA] HDMI: дисплей отключён\n", 0xFFFF55);
        changed = 1;
    }
    if (changed) apply_target();
    return hp;
}

// HDA_OUT_AUTO / HDA_OUT_SPEAKERS / HDA_OUT_HEADPHONES / HDA_OUT_HDMI
// 0 - успех, -1 - такого выхода нет, -2 - неверный режим
int hda_set_output(int mode) {
    if (s_hda_status == HDA_STATUS_NOT_FOUND) return -1;
    if (mode < HDA_OUT_AUTO || mode > HDA_OUT_HDMI) return -2;
    if ((mode == HDA_OUT_SPEAKERS || mode == HDA_OUT_HEADPHONES) && !s_out_n) return -1;
    if (mode == HDA_OUT_HDMI && !hdmi_available()) return -1;

    s_out_mode = mode;
    s_hp_plugged = 0;
    s_hd_connected = 0;
    hda_jack_poll();                  // перечитать состояние (в AUTO сам вызовет apply_target)
    apply_target();
    return 0;
}

int hda_get_output(void) { return s_out_mode; }
int hda_headphones_plugged(void) { return s_hp_plugged; }

// ---------------------------------------------------------------- что доступно
// Заполняет *out: какие выходы есть (динамики, наушники, line-out, HDMI, SPDIF)
// и их текущее состояние (штекер воткнут, дисплей подключён к HDMI).
// Можно вызывать и после hda_init() == -3. Возврат: 0 - успех, -1 - hda_init() не вызывался, -2 - out == NULL.
int check_audio_outputs(hda_outputs_t* out) {
    if (!out) return -2;
    if (!s_inv_valid) return -1;

    hda_outputs_t r = s_inv;
    r.headphones_plugged = 0;
    r.lineout_plugged = 0;

    if (s_hda_status != HDA_STATUS_NOT_FOUND && s_active >= 0) {
        ctx_select(s_active);
        for (int i = 0; i < s_out_n; i++) {
            int nid = s_out_pins[i];
            int dev = pin_dev(nid);
            if (dev == 1) continue;
            if (pin_plugged(nid)) {
                if (dev == 2) r.headphones_plugged = 1;
                else          r.lineout_plugged = 1;
            }
        }
    }
    r.hdmi_connected = hdmi_refresh();          // HDMI может сидеть на другом контроллере
    r.analog_playback = (s_active >= 0 && s_out_n > 0);
    r.hdmi_playback = hdmi_available();
    *out = r;
    return 0;
}

static const char* yn(int v) { return v ? "да" : "нет"; }

void hda_print_outputs(void) {
    hda_outputs_t o;
    if (check_audio_outputs(&o) < 0) {
        kputs("[HDA] инвентарь недоступен: сначала hda_init()\n", 0xFF5555);
        return;
    }
    kputs("Аудио-выходы:\n", 0x55FFFF);
    kputs("  встроенные динамики: ", 0xAAAAAA); kputs(yn(o.has_speakers), 0xFFFFFF); kputs("\n", 0);
    kputs("  разъём наушников:    ", 0xAAAAAA); kputs(yn(o.has_headphones), 0xFFFFFF);
    if (o.has_headphones) { kputs(o.headphones_plugged ? " (воткнуты)" : " (пусто)", 0xFFFF55); }
    kputs("\n", 0);
    kputs("  линейный выход:      ", 0xAAAAAA); kputs(yn(o.has_lineout), 0xFFFFFF);
    if (o.has_lineout) { kputs(o.lineout_plugged ? " (воткнут)" : " (пусто)", 0xFFFF55); }
    kputs("\n", 0);
    kputs("  HDMI/DisplayPort:    ", 0xAAAAAA); kputs(yn(o.has_hdmi > 0), 0xFFFFFF);
    if (o.has_hdmi) {
        kputs(" (портов: ", 0xFFFF55); put_hex(o.has_hdmi, 0xFFFF55);
        kputs(", с дисплеем: ", 0xFFFF55); put_hex(o.hdmi_connected, 0xFFFF55); kputs(")", 0xFFFF55);
    }
    kputs("\n", 0);
    kputs("  SPDIF:               ", 0xAAAAAA); kputs(yn(o.has_spdif), 0xFFFFFF); kputs("\n", 0);
    kputs("  сейчас звук идёт на: ", 0xAAAAAA);
    kputs(s_target == TGT_HDMI ? "HDMI" : (s_target == TGT_ANALOG ? "аналоговый выход" : "никуда"), 0x55FF55);
    kputs("\n", 0);
}

// ---------------------------------------------------------------- стрим
static void start_stream(int ctrl) {
    ctx_select(ctrl);

    uint32_t block_bytes = TONE_FRAMES_PER_BLOCK * 2 * sizeof(int16_t);
    for (int b = 0; b < NUM_BLOCKS; b++) {
        uintptr_t p = hda_virt_to_phys(s_pcm_buf[b]);
        s_bdl[b].addr_low  = (uint32_t)(p & 0xFFFFFFFF);
        s_bdl[b].addr_high = (uint32_t)((uint64_t)p >> 32);
        s_bdl[b].length    = block_bytes;
        s_bdl[b].flags     = 0;
    }

    uint32_t sreg = s_out_stream_offset;

    hda_write32(sreg + SD_CTL, 0);
    wait_for(sreg + SD_CTL, 4, 2, 0, 10000);
    hda_write32(sreg + SD_CTL, 1);                         // SRST
    wait_for(sreg + SD_CTL, 4, 1, 1, 10000);
    hda_write32(sreg + SD_CTL, 0);
    wait_for(sreg + SD_CTL, 4, 1, 0, 10000);
    hda_write8(sreg + SD_STS, 0x1C);

    uintptr_t bp = hda_virt_to_phys(s_bdl);
    hda_write32(sreg + SD_BDLPL, (uint32_t)(bp & 0xFFFFFFFF));
    hda_write32(sreg + SD_BDLPU, (uint32_t)((uint64_t)bp >> 32));
    hda_write32(sreg + SD_CBL, block_bytes * NUM_BLOCKS);
    hda_write16(sreg + SD_LVI, NUM_BLOCKS - 1);
    hda_write16(sreg + SD_FMT, 0x0011);

    hda_write32(sreg + SD_CTL, ((uint32_t)HDA_STREAM_TAG << 20) | (1u << 1));   // RUN
    s_play_ctrl = ctrl;

    // Диагностика: идёт ли DMA?
    uint32_t p1 = hda_read32(sreg + SD_LPIB);
    udelay(20000);
    uint32_t p2 = hda_read32(sreg + SD_LPIB);
    kputs("[HDA] LPIB: ", 0xAAAAAA); put_hex(p1, 0xFFFFFF);
    kputs(" -> ", 0xAAAAAA);         put_hex(p2, 0xFFFFFF);
    if (p1 != p2) kputs("  DMA идёт\n", 0x55FF55);
    else          kputs("  DMA СТОИТ (BAR/маппинг/TCSEL/выбор контроллера)\n", 0xFF5555);
}

// ---------------------------------------------------------------- API
int hda_init(void) {
    hda_ctrl_t ctrls[MAX_CTRL];

    zero((volatile void*)&s_inv, sizeof(s_inv));
    s_hdmi_n = 0;
    s_out_n = 0;
    s_active = -1;
    s_target = TGT_NONE;
    s_hd_sel = -1;
    s_hd_connected = 0;
    s_hp_plugged = 0;
    s_play_ctrl = -1;
    s_out_mode = HDA_OUT_AUTO;
    s_hda_status = HDA_STATUS_NOT_FOUND;
    s_inv_valid = 1;                       // инвентарь доступен даже если играть некуда

    int n = find_controllers(ctrls, MAX_CTRL);
    s_inv.controllers = n;
    if (n == 0) return -1;

    afg_ref_t best = { -1, 0, 0 }, fallback = { -1, 0, 0 };
    for (int i = 0; i < n; i++) {
        uint16_t st = ctrl_bringup(i, &ctrls[i]);
        if (!st) continue;
        for (int cad = 0; cad < 15; cad++)
            if (st & (1u << cad)) scan_codec(i, cad, &best, &fallback);
    }

    // аналоговый выход настраиваем последним: setup_afg заново читает дерево виджетов
    afg_ref_t* pick = best.ctrl >= 0 ? &best : (fallback.ctrl >= 0 ? &fallback : NULL);
    if (pick) {
        ctx_select(pick->ctrl);
        if (setup_afg(pick->cad, pick->afg) == 0) s_active = pick->ctrl;
        else s_out_n = 0;
    }

    if (s_active < 0 && !hdmi_available()) {
        kputs("[HDA] выходов для звука не найдено\n", 0xFFFF55);
        return -3;
    }

    s_hda_status = HDA_STATUS_INITIALIZED;
    hda_set_output(HDA_OUT_AUTO);          // сам выберет: динамики/наушники/HDMI
    return 0;
}

void hda_play_test_tone(void) {
    if (s_hda_status != HDA_STATUS_INITIALIZED) return;
    if (s_target == TGT_NONE) return;

    // Треугольник 1 кГц (48 отсчётов на период). 100 Гц ноутбучный динамик почти не воспроизводит.
    for (int b = 0; b < NUM_BLOCKS; b++) {
        for (int i = 0; i < TONE_FRAMES_PER_BLOCK; i++) {
            int ph = i % 48;
            int16_t s = (ph < 24) ? (int16_t)(-8000 + (16000 * ph) / 24)
                                  : (int16_t)(8000 - (16000 * (ph - 24)) / 24);
            s_pcm_buf[b][i * 2 + 0] = s;
            s_pcm_buf[b][i * 2 + 1] = s;
        }
    }

    if (s_target == TGT_HDMI) {
        if (s_hd_sel < 0) s_hd_sel = hdmi_pick();
        if (hdmi_prepare(s_hd_sel) < 0) return;
        start_stream(s_hdmi[s_hd_sel].ctrl);
    } else {
        start_stream(s_active);
    }
    s_hda_status = HDA_STATUS_PLAYING;
}

void hda_stop(void) {
    if (s_hda_status == HDA_STATUS_PLAYING) {
        if (s_play_ctrl >= 0) {
            ctx_select(s_play_ctrl);
            hda_write32(s_out_stream_offset + SD_CTL, 0);
        }
        if (s_active >= 0) ctx_select(s_active);
        s_hda_status = HDA_STATUS_INITIALIZED;
    }
}

int hda_get_status(void) {
    return s_hda_status;
}