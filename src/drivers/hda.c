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
#include "../kernel/sched.h"   // sched_pid_alive: освобождение голоса упавшей программы
#include <stddef.h>
#include <stdint.h>

extern void kputs(const char* str, uint32_t color);
extern void kdebug(const char* str, uint32_t color);

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

static int      s_verbose = 0;
static uint16_t s_fmt = 0x0011;            // формат ЦАП/стрима: 0x0011 = 48 кГц/16 бит/стерео, 0x4011 = 44.1 кГц
static uint8_t  s_dac_list[8];             // ЦАП аналогового тракта (чтобы менять им формат)
static int      s_dac_n = 0;
static int      s_an_cad = 0;              // адрес кодека аналогового выхода

static int      s_volume = 100;            // 0..100 (программная громкость)
static int      s_muted = 0;
static uint32_t s_gain_q15 = 32768;

#define MODE_NONE 0
#define MODE_TONE 1
#define MODE_PCM  2
static int s_mode = MODE_NONE;

// Потоковый PCM с микшированием. Раньше поток был один на всю систему: второй
// плеер вызывал hda_pcm_open, тот останавливал чужой поток, и оба замолкали.
// Теперь у каждой программы (по pid) свой голос со своим кольцом в формате
// выхода, а в аппаратное кольцо идёт их сумма (mix_pump).
#define PCM_VOICES    4
#define VRING_FRAMES  8192        // ~170 мс при 48 кГц на голос

typedef struct {
    int      used;
    uint64_t owner;               // pid программы, открывшей голос
    uint32_t in_rate;
    int      ch, bits;
    int16_t  buf[VRING_FRAMES * 2];   // стерео 16 бит в формате выхода
    uint64_t wr, rd;                  // кадров записано программой / забрано микшером
    uint64_t base_frames;             // кадры, проигранные до перезапуска стрима
    uint64_t last_wr_pos;             // позиция в аппаратном кольце после последнего его кадра
    uint32_t underruns;
    // линейный ресемплер (вход -> частота выхода), 16.16
    int      rs_active, rs_primed;
    uint32_t rs_step, rs_pos, rs_maxout;
    int32_t  rs_pl, rs_pr;
} pcm_voice_t;

static pcm_voice_t s_voices[PCM_VOICES];

static int      s_pcm_open = 0;            // аппаратный поток запущен
static uint32_t s_pcm_out_rate = 48000;
static uint64_t s_wr_total = 0;            // сколько байт микшер положил в кольцо
static uint64_t s_pl_total = 0;            // сколько байт DMA уже прочитал
static uint32_t s_last_lpib = 0;

static volatile int s_mix_busy = 0;   // микшер занят (защита от таймера, см. hda_tick)
static void stream_halt(void);
static void pcm_restart(void);

#define TONE_FRAMES_PER_BLOCK 480   // 480 кадров = ровно 10 периодов 1 кГц при 48 кГц
#define NUM_BLOCKS 2

// Кольцевой буфер для потокового PCM: всегда 16 бит, стерео
#define RING_ENTRIES      16
#define RING_ENTRY_BYTES  4096
#define RING_BYTES        (RING_ENTRIES * RING_ENTRY_BYTES)   // 64 КБ (~340 мс при 48 кГц)
// Запас очереди задаёт ring_target(): ~170 мс с одним голосом, меньше -- когда
// играют несколько (чтобы пауза одного не тянула хвост общего микса)
#define RING_GAP          2048       // запас перед позицией DMA при старте и после underrun
static uint8_t s_ring[RING_BYTES] __attribute__((aligned(128)));
static hda_bdl_entry_t s_bdl[RING_ENTRIES] __attribute__((aligned(128)));
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
    if (a >= 0xFFFF800000000000ULL) return a - 0xFFFF800000000000ULL;   // прямое окно физпамяти
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

// Логи выключены по умолчанию (hda_set_verbose(1) перед hda_init() покажет всё, что нашёл драйвер)
// s_user_out = 1 только внутри hda_print_outputs()/hda_dump_state(): это ответ на явную команду пользователя,
// он идёт в консоль (kputs). Остальные диагностические логи драйвера идут в kdebug.
static int s_user_out = 0;
static void vputs(const char* s, uint32_t color) {
    if (!s_verbose) return;
    if (s_user_out) kputs(s, color);
    else            kdebug(s, color);
}

static void put_hex(uint32_t v, uint32_t color) {
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF];
    b[10] = 0;
    vputs(b, color);
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

    hda_write8(R_RIRBCTL, 0x03);   // RIRB DMA enable + флаг ответа (иначе QEMU встанет после RINTCNT ответов)
    hda_write8(R_CORBCTL, 0x02);   // CORB DMA enable
    return (hda_read8(R_CORBCTL) & 2) ? 0 : -1;
}

static int hda_xfer(uint32_t verb, uint32_t* resp) {
    if (s_use_imm) return imm_xfer(verb, resp);

    uint16_t wp = (uint16_t)((hda_read16(R_CORBWP) + 1) % s_corb_n);
    s_corb[wp] = verb;
    __asm__ volatile("mfence" ::: "memory");
    hda_write16(R_CORBWP, wp);

    // Кодек отвечает за единицы микросекунд; 50 мс ожидания на запрос, который
    // не придёт, превращались в заметный рывок на весь рабочий стол -- опрос
    // разъёмов идёт из цикла композитора. Не ответил за 2 мс -- уходим на
    // Immediate Command и дальше работаем через него.
    for (uint32_t t = 0; t < 2000; t++) {
        uint16_t rwp = hda_read16(R_RIRBWP) & 0xFF;
        while (s_rirb_rp != rwp) {
            s_rirb_rp = (uint16_t)((s_rirb_rp + 1) % s_rirb_n);
            uint64_t e = s_rirb[s_rirb_rp];
            hda_write8(R_RIRBSTS, 0x05);         // подтвердить ответ (RINTFL/RIRBOIS); без этого QEMU
                                                 // перестаёт обрабатывать CORB после RINTCNT ответов
            if ((e >> 32) & 0x10) continue;      // unsolicited - пропускаем
            *resp = (uint32_t)e;
            return 0;
        }
        udelay(1);
    }
    // CORB/RIRB завис: пробуем Immediate Command и остаёмся на нём
    if (imm_xfer(verb, resp) == 0) { s_use_imm = 1; return 0; }
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
            hda_set4(s_cad, nid, 0x2, s_fmt);                         // формат потока
            int known = 0;
            for (int k = 0; k < s_dac_n; k++) if (s_dac_list[k] == nid) known = 1;
            if (!known && s_dac_n < 8) s_dac_list[s_dac_n++] = (uint8_t)nid;
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
    s_dac_n = 0;
    s_an_cad = cad;

    if (HDA_GPIO_MASK) {
        hda_set12(cad, afg, 0x716, HDA_GPIO_MASK);   // enable
        hda_set12(cad, afg, 0x717, HDA_GPIO_MASK);   // direction = out
        hda_set12(cad, afg, 0x715, HDA_GPIO_MASK);   // data = 1
    }

    if (!((s_afg_pcm >> 6) & 1) || !((s_afg_pcm >> 17) & 1))
        vputs("[HDA] Внимание: кодек не заявляет 48 кГц/16 бит\n", 0xFFFF55);

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
            vputs("[HDA] выход: пин ", 0xAAAAAA); put_hex(nid, 0xFFFFFF);
            vputs(" cfg ", 0xAAAAAA);             put_hex(w->pincfg, 0xFFFFFF);
            vputs(dev == 1 ? " (динамик)" : (dev == 2 ? " (наушники)" : " (line-out)"), 0x55FF55);
            vputs(" -> ЦАП ", 0xAAAAAA);          put_hex(path[len - 1], 0xFFFFFF);
            vputs("\n", 0);
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
    vputs("[HDA] кодек ", 0xAAAAAA); put_hex(cad, 0xFFFFFF);
    vputs(" vendor/device ", 0xAAAAAA); put_hex(vid, 0x55FFFF); vputs("\n", 0);

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

    vputs("[HDA] контроллер ", 0xAAAAAA);
    put_hex(c->pci.vendor_id, 0xFFFFFF); vputs(":", 0xAAAAAA);
    put_hex(c->pci.device_id, 0xFFFFFF); vputs(" bus/slot ", 0xAAAAAA);
    put_hex(c->pci.bus, 0xFFFFFF); vputs("/", 0xAAAAAA);
    put_hex(c->pci.slot, 0xFFFFFF); vputs(" BAR ", 0xAAAAAA);
    put_hex((uint32_t)c->base, 0xFFFFFF); vputs("\n", 0);

    pci_enable_bus_master(&c->pci);
    intel_quirks(&c->pci);

    s_hda_mmio = (uintptr_t)c->base;
    s_use_imm = 0;

    uint16_t gcap = hda_read16(R_GCAP);
    if (gcap == 0 || gcap == 0xFFFF) {
        vputs("[HDA]   MMIO не отвечает (проверь маппинг BAR как uncacheable)\n", 0xFF5555);
        return 0;
    }

    hda_write32(R_INTCTL, 0);
    hda_write32(R_GCTL, hda_read32(R_GCTL) & ~1u);
    wait_for(R_GCTL, 4, 1, 0, 100000);
    udelay(200);
    hda_write32(R_GCTL, hda_read32(R_GCTL) | 1u);
    if (!wait_for(R_GCTL, 4, 1, 1, 100000)) {
        vputs("[HDA]   контроллер не вышел из reset\n", 0xFF5555);
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
        vputs("[HDA]   кодеки не обнаружены\n", 0xFF5555);
        return 0;
    }

    uint8_t num_iss = (gcap >> 8) & 0x0F;
    uint8_t num_oss = (gcap >> 12) & 0x0F;
    if (!num_oss) {
        vputs("[HDA]   нет выходных стримов\n", 0xFF5555);
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
    vputs("[HDA] HDMI: пин ", 0xAAAAAA);        put_hex(h->nid, 0xFFFFFF);
    vputs(" -> конвертер ", 0xAAAAAA);          put_hex(h->conv, 0xFFFFFF);
    vputs((sense >> 31) & 1 ? ", дисплей: да" : ", дисплей: НЕТ", (sense >> 31) & 1 ? 0x55FF55 : 0xFF5555);
    vputs((sense >> 30) & 1 ? ", ELD: да\n" : ", ELD: нет\n", 0xAAAAAA);

    hda_set12(cad, h->afg, 0x705, 0);                // D0 для AFG, конвертера и пина
    hda_set12(cad, h->conv, 0x705, 0);
    hda_set12(cad, h->nid, 0x705, 0);
    hda_set12(cad, h->nid, 0x701, h->conv_idx);      // пин берёт звук с этого конвертера
    hdmi_unmute(cad, h->afg, h->conv, -1);
    hdmi_unmute(cad, h->afg, h->nid, h->conv_idx);

    hda_set12(cad, h->conv, 0x70D, 0x01);            // Digital Enable (обычный PCM, не "non-audio")
    hda_set12(cad, h->conv, 0x72D, 0x01);            // число каналов - 1
    hda_set12(cad, h->conv, 0x706, (HDA_STREAM_TAG << 4) | 0);
    hda_set4(cad, h->conv, 0x2, s_fmt);              // формат потока
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

    // Выбранного класса пинов может не быть: у ПК и у QEMU только line-out, динамика нет.
    // Тогда режим "Speakers" не должен глушить единственный выход.
    //
    // Считаем только пины, которые BIOS объявил ПОДКЛЮЧЁННЫМИ. На ноутбуках
    // встречается пин "Speaker", за которым физически ничего нет, а настоящие
    // динамики висят на line-out: по старой проверке режим "Встроенные динамики"
    // включал мёртвый пин, глушил живой -- и кнопка выглядела нерабочей.
    int any = 0;
    for (int i = 0; i < s_out_n; i++) {
        int nid = s_out_pins[i];
        if (((s_w[nid].pincfg >> 30) & 3) == 1) continue;        // "не подключён"
        if (pin_dev(nid) == 1 ? spk_on : jack_on) any = 1;
    }
    if (!any) { spk_on = 1; jack_on = 1; }

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
    int mode = s_mode;

    if (changed && was_playing) stream_halt();
    s_target = t;
    if (t == TGT_ANALOG) apply_routing();
    else if (t == TGT_HDMI) s_hd_sel = hdmi_pick();
    if (changed && was_playing) {
        if (mode == MODE_TONE) {
            s_hda_status = HDA_STATUS_INITIALIZED;
            s_mode = MODE_NONE;
            hda_play_test_tone();
        } else if (mode == MODE_PCM) {
            pcm_restart();                // очередь теряется (~170 мс), позиция сохраняется
        }
    }
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
        vputs(hp ? "[HDA] наушники подключены\n" : "[HDA] наушники отключены\n", 0xFFFF55);
        changed = 1;
    }
    if (hd != s_hd_connected) {
        s_hd_connected = hd;
        vputs(hd ? "[HDA] HDMI: дисплей подключён\n" : "[HDA] HDMI: дисплей отключён\n", 0xFFFF55);
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
// Куда сейчас идёт звук: HDA_TARGET_NONE / HDA_TARGET_ANALOG / HDA_TARGET_HDMI
int hda_get_target(void) { return s_target; }
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

static const char* yesno(int v) { return v ? "да" : "нет"; }

void hda_print_outputs(void) {
    hda_outputs_t o;
    int sv = s_verbose;
    s_verbose = 1; s_user_out = 1;           // явный запрос пользователя: печатаем всегда
    if (check_audio_outputs(&o) < 0) {
        vputs("[HDA] инвентарь недоступен: сначала hda_init()\n", 0xFF5555);
        s_verbose = sv; s_user_out = 0;
        return;
    }
    vputs("Аудио-выходы:\n", 0x55FFFF);
    vputs("  встроенные динамики: ", 0xAAAAAA); vputs(yesno(o.has_speakers), 0xFFFFFF); vputs("\n", 0);
    vputs("  разъём наушников:    ", 0xAAAAAA); vputs(yesno(o.has_headphones), 0xFFFFFF);
    if (o.has_headphones) { vputs(o.headphones_plugged ? " (воткнуты)" : " (пусто)", 0xFFFF55); }
    vputs("\n", 0);
    vputs("  линейный выход:      ", 0xAAAAAA); vputs(yesno(o.has_lineout), 0xFFFFFF);
    if (o.has_lineout) { vputs(o.lineout_plugged ? " (воткнут)" : " (пусто)", 0xFFFF55); }
    vputs("\n", 0);
    vputs("  HDMI/DisplayPort:    ", 0xAAAAAA); vputs(yesno(o.has_hdmi > 0), 0xFFFFFF);
    if (o.has_hdmi) {
        vputs(" (портов: ", 0xFFFF55); put_hex(o.has_hdmi, 0xFFFF55);
        vputs(", с дисплеем: ", 0xFFFF55); put_hex(o.hdmi_connected, 0xFFFF55); vputs(")", 0xFFFF55);
    }
    vputs("\n", 0);
    vputs("  SPDIF:               ", 0xAAAAAA); vputs(yesno(o.has_spdif), 0xFFFFFF); vputs("\n", 0);
    vputs("  сейчас звук идёт на: ", 0xAAAAAA);
    vputs(s_target == TGT_HDMI ? "HDMI" : (s_target == TGT_ANALOG ? "аналоговый выход" : "никуда"), 0x55FF55);
    vputs("\n", 0);
    s_verbose = sv; s_user_out = 0;
}

// ---------------------------------------------------------------- громкость
// Программная громкость (работает одинаково на динамиках, наушниках и HDMI).
// Кривая квадратичная: 50% = -12 дБ, ближе к восприятию, чем линейная.
void hda_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    s_gain_q15 = ((uint32_t)percent * (uint32_t)percent * 32768u) / 10000u;
}
int  hda_get_volume(void) { return s_volume; }
void hda_set_mute(int mute) { s_muted = mute ? 1 : 0; }
int  hda_get_mute(void) { return s_muted; }

static inline int16_t apply_gain(int32_t s) {
    if (s_muted) return 0;
    int32_t v = (int32_t)(((int64_t)s * (int64_t)s_gain_q15) >> 15);
    if (v > 32767) v = 32767;            // сумма голосов может выйти за int16:
    if (v < -32768) v = -32768;          // ограничиваем, иначе было бы переполнение и треск
    return (int16_t)v;
}

void hda_set_verbose(int on) { s_verbose = on ? 1 : 0; }

// ---------------------------------------------------------------- диагностика
static uint32_t hda_get4(int cad, int nid, uint32_t verb, uint32_t p) {
    uint32_t r = 0;
    if (hda_xfer(cmd4(cad, nid, verb, p), &r) < 0) return 0xDEAD0000;   // кодек не ответил
    return r;
}

static void dump_kv(const char* name, uint32_t v) {
    vputs("  ", 0xAAAAAA); vputs(name, 0xAAAAAA); vputs(" = ", 0xAAAAAA);
    put_hex(v, 0xFFFFFF); vputs("\n", 0);
}

// Печатает всё, что нужно, чтобы понять, почему нет звука: состояние драйвера,
// регистры стрима (двигается ли LPIB) и то, что реально записано в кодеке (ЦАП, пин).
void hda_dump_state(void) {
    int sv = s_verbose;
    s_verbose = 1; s_user_out = 1;                     // явный запрос: печатаем всегда

    vputs("[HDA] === состояние драйвера ===\n", 0x55FFFF);
    dump_kv("status (0 нет, 1 готов, 2 играет)", (uint32_t)s_hda_status);
    dump_kv("mode (0 нет, 1 тон, 2 PCM)", (uint32_t)s_mode);
    dump_kv("target (0 нет, 1 аналог, 2 HDMI)", (uint32_t)s_target);
    dump_kv("режим выхода (0 auto,1 spk,2 hp,3 hdmi)", (uint32_t)s_out_mode);
    dump_kv("активный контроллер", (uint32_t)s_active);
    dump_kv("аналоговых пинов", (uint32_t)s_out_n);
    dump_kv("ЦАП в тракте", (uint32_t)s_dac_n);
    dump_kv("Immediate Command вместо CORB", (uint32_t)s_use_imm);
    dump_kv("громкость", (uint32_t)s_volume);
    dump_kv("mute", (uint32_t)s_muted);
    dump_kv("формат потока", (uint32_t)s_fmt);
    dump_kv("PCM открыт", (uint32_t)s_pcm_open);

    int c = (s_play_ctrl >= 0) ? s_play_ctrl : s_active;
    if (c >= 0) {
        ctx_select(c);
        uint32_t sreg = s_out_stream_offset;
        vputs("[HDA] --- стрим ---\n", 0x55FFFF);
        dump_kv("SD_CTL (бит1 RUN, биты 20-23 тег)", hda_read32(sreg + SD_CTL));
        dump_kv("SD_STS", hda_read8(sreg + SD_STS));
        uint32_t p1 = hda_read32(sreg + SD_LPIB);
        udelay(20000);
        uint32_t p2 = hda_read32(sreg + SD_LPIB);
        dump_kv("LPIB (сейчас)", p1);
        dump_kv("LPIB (через 20 мс)", p2);
        vputs(p1 != p2 ? "  DMA идёт\n" : "  DMA СТОИТ (или поток не запущен)\n", p1 != p2 ? 0x55FF55 : 0xFF5555);
        dump_kv("SD_CBL", hda_read32(sreg + SD_CBL));
        dump_kv("SD_LVI", hda_read16(sreg + SD_LVI));
        dump_kv("SD_FMT", hda_read16(sreg + SD_FMT));
        dump_kv("SD_BDLPL", hda_read32(sreg + SD_BDLPL));
    }

    if (s_active >= 0) {
        ctx_select(s_active);
        vputs("[HDA] --- кодек: что реально записано ---\n", 0x55FFFF);
        for (int k = 0; k < s_dac_n; k++) {
            int nid = s_dac_list[k];
            vputs("ЦАП ", 0xFFFF55); put_hex(nid, 0xFFFF55); vputs("\n", 0);
            dump_kv("питание (0 = D0)", hda_get(s_an_cad, nid, 0xF05, 0));
            dump_kv("стрим/канал (тег в битах 4-7)", hda_get(s_an_cad, nid, 0xF06, 0));
            dump_kv("формат", hda_get4(s_an_cad, nid, 0xA, 0));
            dump_kv("усилитель L (бит7 = mute)", hda_get4(s_an_cad, nid, 0xB, 0xA000));
            dump_kv("усилитель R (бит7 = mute)", hda_get4(s_an_cad, nid, 0xB, 0x8000));
        }
        for (int k = 0; k < s_out_n; k++) {
            int nid = s_out_pins[k];
            vputs("пин ", 0xFFFF55); put_hex(nid, 0xFFFF55);
            vputs(pin_dev(nid) == 1 ? " (динамик)\n" : (pin_dev(nid) == 2 ? " (наушники)\n" : " (line-out)\n"), 0xFFFF55);
            dump_kv("питание (0 = D0)", hda_get(s_an_cad, nid, 0xF05, 0));
            dump_kv("управление пином (бит6 OUT_EN)", hda_get(s_an_cad, nid, 0xF07, 0));
            dump_kv("EAPD (бит1)", hda_get(s_an_cad, nid, 0xF0C, 0));
            dump_kv("выбранный вход", hda_get(s_an_cad, nid, 0xF01, 0));
            dump_kv("усилитель L (бит7 = mute)", hda_get4(s_an_cad, nid, 0xB, 0xA000));
        }
    }
    vputs("[HDA] === конец ===\n", 0x55FFFF);

    if (s_active >= 0) ctx_select(s_active);
    s_verbose = sv; s_user_out = 0;
}

// ---------------------------------------------------------------- стрим
// Настраивает кодек под выбранную цель и формат s_fmt. Возвращает контроллер, на котором играть, или -1.
static int prepare_output(void) {
    if (s_target == TGT_HDMI) {
        if (s_hd_sel < 0) s_hd_sel = hdmi_pick();
        if (hdmi_prepare(s_hd_sel) < 0) return -1;
        return s_hdmi[s_hd_sel].ctrl;
    }
    if (s_target == TGT_ANALOG && s_active >= 0) {
        ctx_select(s_active);
        for (int k = 0; k < s_dac_n; k++) {
            hda_set12(s_an_cad, s_dac_list[k], 0x706, (HDA_STREAM_TAG << 4) | 0);   // заново привязать стрим к ЦАП
            hda_set4(s_an_cad, s_dac_list[k], 0x2, s_fmt);
        }
        return s_active;
    }
    return -1;
}

static void start_stream(int ctrl, void* buf, uint32_t entry_bytes, int entries) {
    ctx_select(ctrl);

    for (int b = 0; b < entries; b++) {
        uintptr_t p = hda_virt_to_phys((uint8_t*)buf + (uint32_t)b * entry_bytes);
        s_bdl[b].addr_low  = (uint32_t)(p & 0xFFFFFFFF);
        s_bdl[b].addr_high = (uint32_t)((uint64_t)p >> 32);
        s_bdl[b].length    = entry_bytes;
        s_bdl[b].flags     = 0;
    }

    uint32_t sreg = s_out_stream_offset;

    hda_write32(sreg + SD_CTL, (uint32_t)HDA_STREAM_TAG << 20);   // RUN=0, tag остаётся
    wait_for(sreg + SD_CTL, 4, 2, 0, 100000);

    int rst_ok = 0;
    for (int attempt = 0; attempt < 3 && !rst_ok; attempt++) {
        hda_write32(sreg + SD_CTL, ((uint32_t)HDA_STREAM_TAG << 20) | 1u);   // SRST=1
        int a = wait_for(sreg + SD_CTL, 4, 1, 1, 100000);  // контроллер вошёл в сброс
        udelay(100);
        hda_write32(sreg + SD_CTL, (uint32_t)HDA_STREAM_TAG << 20);          // SRST=0
        int b = wait_for(sreg + SD_CTL, 4, 1, 0, 100000);  // вышел из сброса
        rst_ok = a && b;
        if (!rst_ok) vputs("[HDA] SRST не завершился, повтор\n", 0xFF5555);
    }
    hda_write8(sreg + SD_STS, 0x1C);

    uintptr_t bp = hda_virt_to_phys(s_bdl);
    hda_write32(sreg + SD_BDLPL, (uint32_t)(bp & 0xFFFFFFFF));
    hda_write32(sreg + SD_BDLPU, (uint32_t)((uint64_t)bp >> 32));
    hda_write32(sreg + SD_CBL, entry_bytes * (uint32_t)entries);
    hda_write16(sreg + SD_LVI, (uint16_t)(entries - 1));
    hda_write16(sreg + SD_FMT, s_fmt);

    hda_write32(sreg + SD_CTL, ((uint32_t)HDA_STREAM_TAG << 20) | (1u << 1));   // RUN
    s_play_ctrl = ctrl;

    if (s_verbose) {
        vputs("[HDA] SD_CTL после RUN: ", 0xAAAAAA);
        put_hex(hda_read32(sreg + SD_CTL), 0xFFFFFF);
        vputs("\n", 0);
    }
}

static void stream_halt(void) {
    if (s_play_ctrl >= 0) {
        ctx_select(s_play_ctrl);
        uint32_t sreg = s_out_stream_offset;
        // Сбрасываем ТОЛЬКО бит RUN, номер стрима (tag, биты 23:20) оставляем: так делает и Linux.
        // Если записать 0 целиком, контроллер сообщает кодеку об остановке стрима №0, а не №1,
        // и кодек (в QEMU точно) считает, что стрим всё ещё играет.
        hda_write32(sreg + SD_CTL, (uint32_t)HDA_STREAM_TAG << 20);
        wait_for(sreg + SD_CTL, 4, 2, 0, 100000);   // дождаться RUN=0
        hda_write8(sreg + SD_STS, 0x1C);
    }
    if (s_active >= 0) ctx_select(s_active);
}

// ---------------------------------------------------------------- потоковый PCM
// Данные пишутся в кольцевой буфер, DMA крутит его по кругу. Позиция DMA берётся из LPIB.
// Всё, что DMA уже прочитал, обнуляется: при нехватке данных играет тишина, а не повтор старого звука.

// Размер кадра на входе голоса (каналы * байт на сэмпл)
static uint32_t voice_frame_bytes(const pcm_voice_t* v) {
    return (uint32_t)v->ch * (uint32_t)(v->bits / 8);
}

static pcm_voice_t* voice_find(uint64_t owner) {
    for (int i = 0; i < PCM_VOICES; i++)
        if (s_voices[i].used && s_voices[i].owner == owner) return &s_voices[i];
    return 0;
}

static int voices_active(void) {
    int n = 0;
    for (int i = 0; i < PCM_VOICES; i++) if (s_voices[i].used) n++;
    return n;
}

// Сколько держим в аппаратной очереди. Один голос -- как раньше (~170 мс, меньше
// риск прерывания); несколько -- короче, чтобы пауза и перемотка одного из них
// не тянули за собой хвост уже смикшированного звука.
static uint32_t ring_target(void) {
    return (voices_active() > 1) ? 12288u : 32768u;
}

// Обновить, сколько DMA прочитал
static void pcm_update(void) {
    if (!s_pcm_open || s_play_ctrl < 0) return;
    ctx_select(s_play_ctrl);
    uint32_t lp = hda_read32(s_out_stream_offset + SD_LPIB);
    if (lp >= RING_BYTES) lp = 0;
    uint32_t d = (lp + RING_BYTES - s_last_lpib) % RING_BYTES;
    if (d) {
        uint32_t idx = (uint32_t)(s_pl_total % RING_BYTES);
        uint32_t n1 = RING_BYTES - idx;
        if (n1 > d) n1 = d;
        zero(s_ring + idx, n1);
        if (d > n1) zero(s_ring, d - n1);
        s_pl_total += d;
        s_last_lpib = lp;
    }
    if (s_wr_total < s_pl_total) {                       // микшер не успел: пишем дальше от DMA
        s_wr_total = s_pl_total + RING_GAP;
    }
    if (s_active >= 0) ctx_select(s_active);
}

// Сколько кадров этого голоса ещё стоит в аппаратной очереди. Кадр, смикшированный
// в позицию P, звучит, когда DMA дошёл до P, поэтому хватает одной отметки на голос.
static uint64_t voice_in_flight(const pcm_voice_t* v) {
    if (s_pl_total >= v->last_wr_pos) return 0;
    uint64_t q = (v->last_wr_pos - s_pl_total) / 4;
    return (q > v->rd) ? v->rd : q;
}

// Кадры голоса, которые уже прозвучали
static uint64_t voice_played_frames(const pcm_voice_t* v) {
    return v->base_frames + (v->rd - voice_in_flight(v));
}

// Сложить голоса в аппаратное кольцо до нужного запаса очереди.
// Голос без данных просто молчит (его rd не двигается) -- остальные играют дальше.
// Когда данных нет ни у кого, микшер останавливается: дальше кольцо не заполняется,
// иначе очередь вечно стояла бы полной тишины и позиция/drained врали бы.
static void mix_pump(void) {
    if (!s_pcm_open) return;
    uint32_t target = ring_target();
    while ((s_wr_total - s_pl_total) + 4 <= target) {
        int32_t l = 0, r = 0, any = 0;
        for (int i = 0; i < PCM_VOICES; i++) {
            pcm_voice_t* v = &s_voices[i];
            if (!v->used || v->rd >= v->wr) continue;
            uint32_t idx = (uint32_t)(v->rd % VRING_FRAMES) * 2;
            l += v->buf[idx];
            r += v->buf[idx + 1];
            v->rd++;
            v->last_wr_pos = s_wr_total + 4;
            any = 1;
        }
        if (!any) break;
        uint32_t widx = (uint32_t)(s_wr_total % RING_BYTES);
        int16_t* q = (int16_t*)(s_ring + widx);
        q[0] = apply_gain(l);
        q[1] = apply_gain(r);
        s_wr_total += 4;
    }
}

static void pcm_reset_counters(void) {
    zero(s_ring, sizeof(s_ring));
    s_pl_total = 0;
    s_wr_total = RING_GAP;
    s_last_lpib = 0;
}

static void pcm_restart(void) {
    int c = prepare_output();
    if (c < 0) {                                         // новый выход не готов: поток закрывается
        s_pcm_open = 0;
        s_mode = MODE_NONE;
        s_hda_status = HDA_STATUS_INITIALIZED;
        return;
    }
    for (int i = 0; i < PCM_VOICES; i++) {
        pcm_voice_t* v = &s_voices[i];
        if (!v->used) continue;
        v->base_frames = voice_played_frames(v);          // позиция переживает смену выхода
        v->rd = v->wr = 0;
        v->last_wr_pos = 0;
        v->rs_primed = 0;
        v->rs_pos = 0;
    }
    pcm_reset_counters();
    start_stream(c, s_ring, RING_ENTRY_BYTES, RING_ENTRIES);
}

static int codec_has_44k(void) {
    if (s_target == TGT_HDMI) return 1;                  // HDMI-приёмники обязаны понимать 44.1 кГц
    return (s_afg_pcm >> 5) & 1;                         // бит 5: 44.1 кГц
}

// Открыть поток. Принимает 8/16/24/32 бит, 1 или более каналов (берутся первые два), частоту 8000..192000.
// 44.1 и 48 кГц играются как есть, остальное ресемплируется в 48 кГц.
// Возврат: 0 ок; -1 драйвер не готов; -2 нет выхода; -3 неверный формат; -5 не удалось настроить выход.
// Открыть голос программы. Принимает 8/16/24/32 бит, 1 или более каналов
// (берутся первые два), частоту 8000..192000. Первый голос задаёт формат
// аппаратного потока (44.1 кГц, если кодек умеет, иначе 48); остальные
// пересчитываются в него. Повторный вызов тем же владельцем перезапускает
// его голос, чужие при этом продолжают играть.
// Возврат: 0 ок; -1 драйвер не готов; -2 нет выхода; -3 неверный формат;
// -4 нет свободного голоса; -5 не удалось настроить выход.
int hda_pcm_open(uint32_t rate, int channels, int bits, uint64_t owner) {
    if (s_hda_status == HDA_STATUS_NOT_FOUND) return -1;
    if (s_target == TGT_NONE) return -2;
    if (channels < 1 || channels > 8) return -3;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return -3;
    if (rate < 8000 || rate > 192000) return -3;

    // Голоса мёртвых процессов освобождаем: иначе упавший плеер держал бы слот
    for (int i = 0; i < PCM_VOICES; i++)
        if (s_voices[i].used && s_voices[i].owner && !sched_pid_alive(s_voices[i].owner))
            s_voices[i].used = 0;

    if (s_pcm_open && s_mode != MODE_PCM) hda_stop();      // играл тестовый тон
    if (!s_pcm_open) {
        for (int i = 0; i < PCM_VOICES; i++) s_voices[i].used = 0;
        if (s_hda_status == HDA_STATUS_PLAYING) hda_stop();
        uint32_t out = 48000;
        uint16_t fmt = 0x0011;
        if (rate == 44100 && codec_has_44k()) { out = 44100; fmt = 0x4011; }
        s_pcm_out_rate = out;
        s_fmt = fmt;

        int c = prepare_output();
        if (c < 0) return -5;
        pcm_reset_counters();
        s_pcm_open = 1;
        s_mode = MODE_PCM;
        start_stream(c, s_ring, RING_ENTRY_BYTES, RING_ENTRIES);
        s_hda_status = HDA_STATUS_PLAYING;
    }

    pcm_voice_t* v = voice_find(owner);
    if (!v) for (int i = 0; i < PCM_VOICES; i++) if (!s_voices[i].used) { v = &s_voices[i]; break; }
    if (!v) return -4;

    v->used = 1;
    v->owner = owner;
    v->in_rate = rate;
    v->ch = channels;
    v->bits = bits;
    v->wr = v->rd = 0;
    v->last_wr_pos = 0;
    v->base_frames = 0;
    v->underruns = 0;
    v->rs_active = (rate != s_pcm_out_rate);
    v->rs_step = (uint32_t)(((uint64_t)rate << 16) / s_pcm_out_rate);
    v->rs_maxout = (v->rs_step < 65536) ? (65536u / v->rs_step + 2u) : 2u;
    v->rs_primed = 0;
    v->rs_pos = 0;
    v->rs_pl = v->rs_pr = 0;
    return 0;
}

static inline int32_t rd_sample(const uint8_t* p, int bits) {
    switch (bits) {
    case 8:  return ((int32_t)p[0] - 128) << 8;
    case 16: return (int16_t)(p[0] | (p[1] << 8));
    case 24: return (int16_t)(p[1] | (p[2] << 8));      // старшие 16 бит
    default: return (int16_t)(p[2] | (p[3] << 8));
    }
}

// Кадр в кольцо голоса (громкость применяется позже, при микшировании)
static inline void voice_put(pcm_voice_t* v, int32_t l, int32_t r) {
    uint32_t idx = (uint32_t)(v->wr % VRING_FRAMES) * 2;
    v->buf[idx]     = (int16_t)(l >  32767 ? 32767 : (l < -32768 ? -32768 : l));
    v->buf[idx + 1] = (int16_t)(r >  32767 ? 32767 : (r < -32768 ? -32768 : r));
    v->wr++;
}

static inline int voice_full(const pcm_voice_t* v) {
    return (v->wr - v->rd) + v->rs_maxout >= VRING_FRAMES;
}

// Дописать данные (целые кадры формата, заданного в hda_pcm_open). Не блокируется.
// Возвращает, сколько байт принято; 0 = кольцо голоса заполнено, повтори позже.
int hda_pcm_write(const void* data, uint32_t bytes, uint64_t owner) {
    if (!s_pcm_open || !data) return 0;
    pcm_voice_t* v = voice_find(owner);
    if (!v) return 0;
    s_mix_busy = 1;
    pcm_update();

    const uint8_t* p = (const uint8_t*)data;
    uint32_t fb = voice_frame_bytes(v);
    uint32_t bps = (uint32_t)(v->bits / 8);
    uint32_t used = 0;

    while (used + fb <= bytes) {
        if (voice_full(v)) break;

        int32_t l = rd_sample(p + used, v->bits);
        int32_t r = (v->ch >= 2) ? rd_sample(p + used + bps, v->bits) : l;

        if (!v->rs_active) {
            voice_put(v, l, r);
        } else if (!v->rs_primed) {
            v->rs_pl = l; v->rs_pr = r; v->rs_primed = 1; v->rs_pos = 0;
        } else {
            while (v->rs_pos < 65536) {
                int32_t ol = v->rs_pl + (int32_t)(((int64_t)(l - v->rs_pl) * (int64_t)v->rs_pos) >> 16);
                int32_t or_ = v->rs_pr + (int32_t)(((int64_t)(r - v->rs_pr) * (int64_t)v->rs_pos) >> 16);
                voice_put(v, ol, or_);
                v->rs_pos += v->rs_step;
            }
            v->rs_pos -= 65536;
            v->rs_pl = l; v->rs_pr = r;
        }
        used += fb;
    }
    mix_pump();
    s_mix_busy = 0;
    return (int)used;
}

// Размер одного кадра входного формата в байтах; 0, если голос не открыт
uint32_t hda_pcm_frame_bytes(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    return v ? voice_frame_bytes(v) : 0;
}

// Сколько миллисекунд аудио этой программы уже проиграно (с момента open/flush)
uint32_t hda_pcm_position_ms(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    if (!s_pcm_open || !v) return 0;
    pcm_update();
    mix_pump();
    return (uint32_t)((voice_played_frames(v) * 1000ull) / s_pcm_out_rate);
}

// Сколько миллисекунд её звука ещё не прозвучало
uint32_t hda_pcm_queued_ms(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    if (!s_pcm_open || !v) return 0;
    pcm_update();
    uint64_t q = (v->wr - v->rd) + voice_in_flight(v);
    return (uint32_t)((q * 1000ull) / s_pcm_out_rate);
}

// 1, если всё записанное этой программой уже проиграно
int hda_pcm_drained(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    if (!s_pcm_open || !v) return 1;
    pcm_update();
    mix_pump();
    return v->rd >= v->wr && s_pl_total >= v->last_wr_pos;
}

// Сколько раз этому голосу не хватило данных (слышно как пропуск звука)
uint32_t hda_pcm_underruns(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    return v ? v->underruns : 0;
}

// Периодическая прокачка из таймера ядра. Раньше кольцо пополнялось и
// освобождалось ТОЛЬКО внутри сисколлов самой программы. Стоило плееру встать
// на паузу (или просто не успеть -- например, пока пользователь ушёл в TTY),
// как pcm_update() переставал затирать уже проигранное, DMA доходил до конца
// кольца и начинал играть его заново: тот самый повтор "го-го-го" на паузе.
// Теперь очередь обслуживает ядро, независимо от того, зовёт ли кто сисколлы.
static void tone_feed(void);        // определён ниже, вместе с тестовым тоном

void hda_tick(void) {
    if (!s_pcm_open || s_mix_busy) return;
    tone_feed();                      // сам берёт s_mix_busy через hda_pcm_write
    if (s_mix_busy) return;
    s_mix_busy = 1;
    pcm_update();
    mix_pump();
    s_mix_busy = 0;
}

// Сбросить очередь и позицию голоса (перемотка, пауза). Формат и выход прежние.
// Один голос -- чистим и аппаратное кольцо, как раньше. Если играет кто-то ещё,
// трогать общее кольцо нельзя: в нём чужой звук, поэтому остаётся короткий хвост
// уже смикшированного (для этого ring_target() при нескольких голосах меньше).
void hda_pcm_flush(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    if (!s_pcm_open || !v) return;
    v->wr = v->rd = 0;
    v->last_wr_pos = 0;
    v->base_frames = 0;
    v->rs_primed = 0;
    v->rs_pos = 0;
    if (voices_active() <= 1) {
        stream_halt();
        pcm_reset_counters();
        start_stream(s_play_ctrl, s_ring, RING_ENTRY_BYTES, RING_ENTRIES);
    }
}

// Закрыть голос программы. Аппаратный поток останавливается, когда ушёл последний.
void hda_pcm_close(uint64_t owner) {
    pcm_voice_t* v = voice_find(owner);
    if (!v) return;
    v->used = 0;
    if (!voices_active() && s_pcm_open) hda_stop();
}

// ---------------------------------------------------------------- API
int hda_init(void) {
    hda_ctrl_t ctrls[MAX_CTRL];

    if (s_hda_status != HDA_STATUS_NOT_FOUND) return 0;      // уже работает

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
        vputs("[HDA] выходов для звука не найдено\n", 0xFFFF55);
        return -3;
    }

    s_hda_status = HDA_STATUS_INITIALIZED;
    hda_set_output(HDA_OUT_AUTO);          // сам выберет: динамики/наушники/HDMI
    return 0;
}

// Безопасная ленивая инициализация: можно звать откуда угодно (сисколл 70, шелл, ядро).
// Реально инициализирует один раз; результат (0 / -1 / -3) запоминается, шину повторно не опрашивает.
int hda_ensure_init(void) {
    static int tried = 0, res = 0;
    if (s_hda_status != HDA_STATUS_NOT_FOUND) return 0;
    if (!tried) { tried = 1; res = hda_init(); }
    return res;
}

// Тестовый тон при выборе устройства вывода. Раньше он забирал себе весь
// аппаратный поток, поэтому при открытом видео не звучал вовсе (драйвер был
// занят), а его остановка глушила заодно и плеер. Теперь это обычный голос
// микшера, как у любой программы: он подмешивается к тому, что уже играет.
#define TEST_TONE_OWNER 0xFFFF0001ULL
#define TONE_FRAMES     48000u            // секунда при 48 кГц

static uint32_t s_tone_left = 0;          // сколько кадров тона ещё выдать
static uint32_t s_tone_ph   = 0;          // фаза в периоде (48 отсчётов = 1 кГц)

// Подливает тон в свой голос. Зовётся и из hda_play_test_tone, и из таймера:
// раньше тон целиком выкладывали одним куском (~300 мс), и если в этот момент
// поток перезапускался (смена выхода, опрос разъёмов), от писка не оставалось
// ничего -- отсюда "в первый раз писка нет".
static void tone_feed(void) {
    if (!s_tone_left) return;
    pcm_voice_t* v = voice_find(TEST_TONE_OWNER);
    if (!v) { s_tone_left = 0; return; }

    static int16_t blk[480 * 2];                       // 10 мс
    while (s_tone_left) {
        if (v->wr - v->rd > VRING_FRAMES / 2) break;   // запаса в голосе хватает
        uint32_t n = s_tone_left < 480 ? s_tone_left : 480;
        for (uint32_t i = 0; i < n; i++) {
            int ph = (int)((s_tone_ph + i) % 48);
            int32_t x = (ph < 24) ? (-8000 + (16000 * ph) / 24)
                                  : (8000 - (16000 * (ph - 24)) / 24);
            blk[i * 2 + 0] = (int16_t)x;
            blk[i * 2 + 1] = (int16_t)x;
        }
        int wrote = hda_pcm_write(blk, n * 4, TEST_TONE_OWNER);
        if (wrote <= 0) break;
        uint32_t fr = (uint32_t)wrote / 4;
        s_tone_ph = (s_tone_ph + fr) % 48;
        s_tone_left -= fr;
    }
}

void hda_play_test_tone(void) {
    if (s_hda_status == HDA_STATUS_NOT_FOUND) return;
    if (hda_pcm_open(48000, 2, 16, TEST_TONE_OWNER) != 0) return;
    hda_pcm_flush(TEST_TONE_OWNER);
    // 1 кГц: 100 Гц ноутбучный динамик почти не воспроизводит
    s_tone_ph   = 0;
    s_tone_left = TONE_FRAMES;
    tone_feed();
}

// Погасить только тестовый тон: чужой звук при этом продолжает играть
void hda_stop_test_tone(void) {
    s_tone_left = 0;
    hda_pcm_close(TEST_TONE_OWNER);
}

void hda_stop(void) {
    if (s_hda_status != HDA_STATUS_PLAYING) return;
    stream_halt();
    for (int i = 0; i < PCM_VOICES; i++) s_voices[i].used = 0;   // поток ушёл -- голоса тоже
    s_pcm_open = 0;
    s_mode = MODE_NONE;
    s_hda_status = HDA_STATUS_INITIALIZED;
}

int hda_get_status(void) {
    return s_hda_status;
}