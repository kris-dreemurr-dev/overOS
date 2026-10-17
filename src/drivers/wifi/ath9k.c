// =============================================================================
// ath9k.c — драйвер Atheros AR9565 (QCA9565), только приём.
//
// Перенос пути инициализации драйвера ath9k из Linux (ISC, Atheros
// Communications / Qualcomm Atheros), сокращённый до одного чипа и одного
// режима: 2,4 ГГц, 20 МГц, приём. Порядок шагов и значения взяты оттуда
// один в один; имена регистров тоже, чтобы любой шаг можно было сверить с
// исходником: hw.c (сброс, питание, PLL), ar9003_phy.c (таблицы, канал),
// ar9003_eeprom.c (калибровка из OTP), ar9003_calib.c / calib.c (AGC, шум),
// ar9003_mac.c / recv.c (кольца приёма EDMA).
//
// Чего здесь нет и почему. Передатчика: для пассивного скана он не нужен,
// поэтому не трогаем ни очереди TX, ни мощности, ни калибровку PAPRD.
// Прерываний: кольцо приёма опрашиваем, как события xHCI. ANI (адаптивная
// помехоустойчивость): её уровни по умолчанию совпадают со значениями из
// таблиц. Совместной работы с Bluetooth (MCI): в Linux она по умолчанию тоже
// выключена (btcoex_enable=0).
//
// Прошивки у PCIe-карт ath9k нет: всё — регистры. Калибровка платы лежит в
// OTP чипа, сжатая дельтами к эталонам (ar9300_eeprom.h).
// =============================================================================

#include "ath9k.h"
#include "wifi.h"
#include "ath9k_regs.h"
#include "ar9565_initvals.h"
#include "ar9300_eeprom.h"
#include "../pci.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t g_tsc_per_ms;

#define ATH_VENDOR      0x168C
#define ATH_DEV_AR9565  0x0036

// ------------------------------------------------------------------ состояние
static pci_device_t s_pci;
static volatile uint8_t* s_mmio = 0;
static uint32_t s_mac_version = 0, s_mac_rev = 0;
static uint32_t s_wa = 0;                 // AR_WA, без чтения-изменения (как WARegVal)
static int s_reset_power_on = 0;
static int s_awake = 0;
static int s_attached = 0;
static uint8_t s_mac[6];

static union { struct ar9300_eeprom e; uint8_t b[sizeof(struct ar9300_eeprom)]; } s_eep;
#define EEP (s_eep.e)

static uint8_t s_rxmask = 1, s_txmask = 1;
static int s_tx_gain_idx = 0, s_rx_gain_idx = 0;
static int s_cal_tx_iq = 0, s_cal_tx_cl = 0;     // enabled_cals: TX_IQ_CAL, TX_CL_CAL
static int s_ant_div_comb = 0;
static int s_cur_freq = 0;

// ------------------------------------------------------------------ регистры
static inline uint32_t RD(uint32_t r) { return *(volatile uint32_t*)(s_mmio + r); }
static inline void WR(uint32_t r, uint32_t v) { *(volatile uint32_t*)(s_mmio + r) = v; }
// REG_RMW из ath9k: сначала снять clr, потом поставить set
static inline void RMW(uint32_t r, uint32_t set, uint32_t clr) { WR(r, (RD(r) & ~clr) | set); }
#define SETB(r, f)          RMW((r), (f), 0)
#define CLRB(r, f)          RMW((r), 0, (f))
#define SM(v, f)            ((((uint32_t)(v)) << f##_S) & (f))
#define MS(v, f)            ((((uint32_t)(v)) & (f)) >> f##_S)
#define RMW_FIELD(r, f, v)  RMW((r), SM((v), f), (f))
#define RD_FIELD(r, f)      MS(RD(r), f)

// ------------------------------------------------------------------ время
static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Микросекунды по TSC: последовательности PHY требуют настоящих микросекунд,
// а таймер ядра тикает раз в миллисекунду
static void udelay(uint32_t us) {
    if (!g_tsc_per_ms) {
        for (volatile uint32_t i = 0; i < us * 400u; i++) { }
        return;
    }
    uint64_t end = tsc_now() + (uint64_t)us * g_tsc_per_ms / 1000;
    while (tsc_now() < end) __asm__ volatile ("pause");
}

#define AH_WAIT_TIMEOUT   100000      // мкс
#define AH_TIME_QUANTUM   10
#define POWER_UP_TIME     10000
#define BASE_ACTIVATE_DELAY 100
#define RTC_PLL_SETTLE_DELAY 100
#define INIT_RSSI_THR     0x00000700
#define COEF_SCALE_S      24

// ath9k_hw_wait
static int hw_wait(uint32_t reg, uint32_t mask, uint32_t val, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout / AH_TIME_QUANTUM; i++) {
        if ((RD(reg) & mask) == val) return 1;
        udelay(AH_TIME_QUANTUM);
    }
    wlog("  ! таймаут %u мкс: рег 0x%05x = 0x%08x, маска 0x%08x, ждали 0x%08x\n",
         timeout, reg, RD(reg), mask, val);
    return 0;
}

// ------------------------------------------------------------------ проба PCI
int ath9k_probe(const char** name) {
    static char nm[48];
    if (!pci_find_by_class(0x02, 0x80, &s_pci)) return 0;
    const char* hx = "0123456789abcdef";
    if (s_pci.vendor_id != ATH_VENDOR) {
        // Сетевая карта класса 02:80, но не Atheros: назовём хотя бы цифрами
        const char* p = "Wi-Fi ";
        int n = 0;
        while (*p) nm[n++] = *p++;
        uint16_t v[2] = { s_pci.vendor_id, s_pci.device_id };
        for (int k = 0; k < 2; k++) {
            for (int s = 12; s >= 0; s -= 4) nm[n++] = hx[(v[k] >> s) & 15];
            if (!k) nm[n++] = ':';
        }
        nm[n] = '\0';
        *name = nm;
        wlog("PCI %02x:%02x.%u: карта %04x:%04x (не Atheros)\n",
             s_pci.bus, s_pci.slot, s_pci.func, s_pci.vendor_id, s_pci.device_id);
        return 2;
    }
    if (s_pci.device_id != ATH_DEV_AR9565) {
        const char* p = "Atheros 168c:";
        int n = 0;
        while (*p) nm[n++] = *p++;
        for (int s = 12; s >= 0; s -= 4) nm[n++] = hx[(s_pci.device_id >> s) & 15];
        nm[n] = '\0';
        *name = nm;
        wlog("PCI %02x:%02x.%u: Atheros %04x, а таблицы у нас только для AR9565 (0036)\n",
             s_pci.bus, s_pci.slot, s_pci.func, s_pci.device_id);
        return 2;
    }
    *name = "Atheros AR9565";
    uint32_t sub = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0x2C);
    wlog("PCI %02x:%02x.%u: Atheros AR9565 168c:0036, подсистема %04x:%04x\n",
         s_pci.bus, s_pci.slot, s_pci.func, sub & 0xFFFF, sub >> 16);
    return 1;
}

// Настройка PCI как в ath_pci_probe
static int pci_setup(void) {
    uint8_t b = s_pci.bus, sl = s_pci.slot, fn = s_pci.func;

    uint32_t r0c = pci_config_read(b, sl, fn, 0x0C);
    uint32_t csz = r0c & 0xFF;
    if (csz == 0) csz = 64 / 4;                       // размер строки кэша в словах
    r0c = (r0c & ~0xFFFFu) | csz | (0xA8u << 8);      // и latency timer 0xa8
    pci_config_write(b, sl, fn, 0x0C, r0c);

    // Память, ведущий режим шины; INTx запрещаем: обработчика нет, опрашиваем
    uint32_t cmd = pci_config_read(b, sl, fn, 0x04);
    pci_config_write(b, sl, fn, 0x04, (cmd | 0x06) | (1u << 10));

    // RETRY_TIMEOUT (0x41) выключить, как делает Linux
    uint32_t v40 = pci_config_read(b, sl, fn, 0x40);
    if (v40 & 0x0000FF00) pci_config_write(b, sl, fn, 0x40, v40 & 0xFFFF00FF);

    uint32_t bar0 = pci_config_read(b, sl, fn, 0x10);
    uint64_t base = bar0 & ~0xFULL;
    if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)pci_config_read(b, sl, fn, 0x14) << 32;
    wlog("BAR0 = 0x%08x%08x, команда PCI 0x%04x -> 0x%04x, cache line %u\n",
         (uint32_t)(base >> 32), (uint32_t)base, cmd & 0xFFFF,
         pci_config_read(b, sl, fn, 0x04) & 0xFFFF, csz);
    if (!base) { wlog("  ! BAR0 пуст: прошивка не выдала карте адреса\n"); return -1; }
    if (base >= 0x100000000ULL) { wlog("  ! BAR0 выше 4 ГБ, пока не поддерживаем\n"); return -1; }
    s_mmio = (volatile uint8_t*)(uintptr_t)base;      // низ 4 ГБ отображён 1:1
    return 0;
}

// ------------------------------------------------------------------ сброс
enum { RESET_POWER_ON, RESET_WARM, RESET_COLD };

// ath9k_hw_set_reset
static int set_reset(int type) {
    WR(AR_WA, s_wa);
    udelay(10);
    WR(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);

    uint32_t tmp = RD(AR_INTR_SYNC_CAUSE) & (AR_INTR_SYNC_LOCAL_TIMEOUT | AR_INTR_SYNC_RADM_CPL_TIMEOUT);
    if (tmp) {
        wlog("  перед сбросом висел таймаут шины (SYNC_CAUSE 0x%08x)\n", tmp);
        WR(AR_INTR_SYNC_ENABLE, 0);
        WR(AR_RC, AR_RC_HOSTIF);
    }
    uint32_t rst = AR_RTC_RC_MAC_WARM;
    if (type == RESET_COLD) rst |= AR_RTC_RC_MAC_COLD;

    WR(AR_RTC_RC, rst);
    udelay(50);
    WR(AR_RTC_RC, 0);
    if (!hw_wait(AR_RTC_RC, AR_RTC_RC_M, 0, AH_WAIT_TIMEOUT)) {
        wlog("  ! RTC застрял в сбросе MAC\n");
        return 0;
    }
    WR(AR_RC, 0);
    return 1;
}

// ath9k_hw_set_reset_power_on
static int set_reset_power_on(void) {
    WR(AR_WA, s_wa);
    udelay(10);
    WR(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    WR(AR_RTC_RESET, 0);
    udelay(2);
    WR(AR_RTC_RESET, 1);
    if (!hw_wait(AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON, AH_WAIT_TIMEOUT)) {
        wlog("  ! RTC не просыпается\n");
        return 0;
    }
    return set_reset(RESET_WARM);
}

// ath9k_hw_set_reset_reg
static int set_reset_reg(int type) {
    WR(AR_WA, s_wa);
    udelay(10);
    WR(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    if (!s_reset_power_on) type = RESET_POWER_ON;
    int ok;
    if (type == RESET_POWER_ON) {
        ok = set_reset_power_on();
        if (ok) s_reset_power_on = 1;
    } else {
        ok = set_reset(type);
    }
    return ok;
}

// ath9k_hw_set_power_awake (+ проверка режима, как в ath9k_hw_setpower)
static int set_power_awake(void) {
    if (s_awake) return 1;
    WR(AR_WA, s_wa);
    udelay(10);
    if ((RD(AR_RTC_STATUS) & AR_RTC_STATUS_M) == AR_RTC_STATUS_SHUTDOWN) {
        if (!set_reset_reg(RESET_POWER_ON)) return 0;
    }
    SETB(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    udelay(50);
    int i;
    for (i = POWER_UP_TIME / 50; i > 0; i--) {
        if ((RD(AR_RTC_STATUS) & AR_RTC_STATUS_M) == AR_RTC_STATUS_ON) break;
        udelay(50);
        SETB(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    }
    if (i == 0) {
        wlog("  ! чип не проснулся за %u мкс (RTC_STATUS 0x%08x)\n", POWER_UP_TIME, RD(AR_RTC_STATUS));
        return 0;
    }
    CLRB(AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    s_awake = 1;
    return 1;
}

// ath9k_hw_init_pll для AR9565 (полная скорость, без half/quarter)
static void init_pll(void) {
    uint32_t pll = SM(0x5, AR_RTC_9300_PLL_REFDIV) | SM(0x2c, AR_RTC_9300_PLL_DIV);

    // PLL основной полосы: ki=0x4, kd=0x40
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_PLL_PWD, 0x1);
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_DPLL2_KD, 0x40);
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_DPLL2_KI, 0x4);
    RMW_FIELD(AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_REFDIV, 0x5);
    RMW_FIELD(AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_NINI, 0x58);
    RMW_FIELD(AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_NFRAC, 0x0);
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_OUTDIV, 0x1);
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_LOCAL_PLL, 0x1);
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_EN_NEGTRIG, 0x1);
    RMW_FIELD(AR_CH0_BB_DPLL3, AR_CH0_BB_DPLL3_PHASE_SHIFT, 0x6);
    RMW_FIELD(AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_PLL_PWD, 0x0);
    udelay(1000);

    pll |= 0x40000;                       // AR9565
    WR(AR_RTC_PLL_CONTROL, pll);
    udelay(RTC_PLL_SETTLE_DELAY);
    WR(AR_RTC_SLEEP_CLK, AR_RTC_FORCE_DERIVED_CLK);
}

// ath9k_hw_chip_test: регистр отвечает тем, что в него записали
static int chip_test(void) {
    static const uint32_t pat[4] = { 0x55555555, 0xaaaaaaaa, 0x66666666, 0x99999999 };
    uint32_t addr = AR_STA_ID0, hold = RD(addr);
    for (uint32_t j = 0; j < 0x100; j++) {
        uint32_t w = (j << 16) | j;
        WR(addr, w);
        uint32_t r = RD(addr);
        if (r != w) { wlog("  ! проверка регистра 0x%x: записали 0x%08x, прочли 0x%08x\n", addr, w, r); return 0; }
    }
    for (int j = 0; j < 4; j++) {
        WR(addr, pat[j]);
        uint32_t r = RD(addr);
        if (r != pat[j]) { wlog("  ! проверка регистра 0x%x: записали 0x%08x, прочли 0x%08x\n", addr, pat[j], r); return 0; }
    }
    WR(addr, hold);
    udelay(100);
    return 1;
}

// ------------------------------------------------------------------ EEPROM / OTP
#define COMP_HDR_LEN   4
#define COMP_CKSUM_LEN 2

// ath_pci_eeprom_read: одно 16-битное слово внешнего EEPROM
static int nvram_read(uint32_t off, uint16_t* data) {
    (void)RD(AR5416_EEPROM_OFFSET + (off << AR5416_EEPROM_S));
    if (!hw_wait(AR_EEPROM_STATUS_DATA, AR_EEPROM_STATUS_DATA_BUSY | AR_EEPROM_STATUS_DATA_PROT_ACCESS,
                 0, AH_WAIT_TIMEOUT)) return 0;
    *data = (uint16_t)MS(RD(AR_EEPROM_STATUS_DATA), AR_EEPROM_STATUS_DATA_VAL);
    return 1;
}

static int eeprom_read_byte(int address, uint8_t* buf) {
    uint16_t v;
    if (!nvram_read((uint32_t)address / 2, &v)) return 0;
    *buf = (uint8_t)((v >> (8 * (address % 2))) & 0xff);
    return 1;
}

static int eeprom_read_word(int address, uint8_t* buf) {
    uint16_t v;
    if (!nvram_read((uint32_t)address / 2, &v)) return 0;
    buf[0] = (uint8_t)(v >> 8);
    buf[1] = (uint8_t)(v & 0xff);
    return 1;
}

// ar9300_read_eeprom: байты читаются в обратном порядке, от address вниз
static int read_eeprom(int address, uint8_t* buf, int count) {
    if (address < 0 || (address + count) / 2 > AR9300_EEPROM_SIZE - 1) return 0;
    if (address % 2 == 0) {
        if (!eeprom_read_byte(address--, buf++)) return 0;
        count--;
    }
    for (int i = 0; i < count / 2; i++) {
        if (!eeprom_read_word(address, buf)) return 0;
        address -= 2;
        buf += 2;
    }
    if (count % 2)
        if (!eeprom_read_byte(address, buf)) return 0;
    return 1;
}

static int otp_read_word(int addr, uint32_t* data) {
    (void)RD(AR9300_OTP_BASE + 4u * (uint32_t)addr);
    if (!hw_wait(AR9300_OTP_STATUS, AR9300_OTP_STATUS_TYPE, AR9300_OTP_STATUS_VALID, 1000)) return 0;
    *data = RD(AR9300_OTP_READ_DATA);
    return 1;
}

static int read_otp(int address, uint8_t* buf, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t data;
        int offset = 8 * ((address - i) % 4);
        if (!otp_read_word((address - i) / 4, &data)) return 0;
        buf[i] = (uint8_t)((data >> offset) & 0xff);
    }
    return 1;
}

typedef int (*nv_read_t)(int address, uint8_t* buf, int count);

static int check_header(const uint8_t* w) {
    uint32_t v = (uint32_t)w[0] | ((uint32_t)w[1] << 8) | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);
    return !(v == 0 || v == 0xFFFFFFFFu);
}

static int check_nv_header(nv_read_t rd, int base) {
    uint8_t h[4];
    if (!rd(base, h, 4)) return 0;
    return check_header(h);
}

static const uint8_t* template_by_id(int id) {
    for (int i = 0; i < AR9300_TEMPLATES; i++)
        if (ar9300_templates[i][offsetof(struct ar9300_eeprom, templateVersion)] == id) return ar9300_templates[i];
    return 0;
}

// ar9300_uncompress_block: пары (смещение, длина) с данными поверх эталона
static int uncompress_block(uint8_t* mptr, int msize, const uint8_t* block, int size) {
    int spot = 0, length = 0;
    for (int it = 0; it < size; it += length + 2) {
        int offset = block[it] & 0xff;
        spot += offset;
        length = block[it + 1] & 0xff;
        if (length > 0 && spot >= 0 && spot + length <= msize) {
            wbcopy(&mptr[spot], &block[it + 2], (uint64_t)length);
            spot += length;
        } else if (length > 0) {
            wlog("  ! плохой кусок блока: место %d, дл