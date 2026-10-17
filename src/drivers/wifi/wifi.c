// =============================================================================
// wifi.c — общий слой Wi-Fi: журнал, пассивный скан, разбор биконов.
//
// Скан пассивный: на каждом канале 1..13 чип сбрасывается на нужную частоту,
// и мы просто слушаем. Точка доступа сама шлёт бикон примерно десять раз в
// секунду, поэтому 200 мс на канал хватает, а передатчик не нужен вовсе.
//
// Отладка вслепую была бы безнадёжной, поэтому каждый шаг драйвера пишется в
// журнал: в память и файлом /wifi.log на флешку. Его можно прочитать на
// другом компьютере целиком, а не по фотографиям экрана.
// =============================================================================

#include "wifi.h"
#include "ath9k.h"
#include "../../fs/fs.h"
#include <stdint.h>

extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern uint64_t g_tsc_per_ms;

static int s_state = WIFI_NONE;
static const char* s_chip = "";

// ------------------------------------------------------------------ журнал
#define WLOG_MAX (64 * 1024)
static char s_log[WLOG_MAX];
static int  s_log_len = 0;
static int  s_log_full = 0;

static void log_putc(char c) {
    if (s_log_len < WLOG_MAX - 64) s_log[s_log_len++] = c;
    else s_log_full = 1;
}

static void log_puts(const char* s) { while (s && *s) log_putc(*s++); }

static void log_uint(uint64_t v, int base, int width, char pad) {
    char b[24];
    int n = 0;
    const char* dig = "0123456789abcdef";
    do { b[n++] = dig[v % (uint64_t)base]; v /= (uint64_t)base; } while (v && n < 22);
    while (n < width && n < 22) b[n++] = pad;
    while (n) log_putc(b[--n]);
}

// Свой маленький printf: %s %c %d %u %x, ширина и '0' (%08x, %02x)
void wlog(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { log_putc(*p); continue; }
        p++;
        char pad = ' ';
        int width = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        switch (*p) {
            case 's': log_puts(__builtin_va_arg(ap, const char*)); break;
            case 'c': log_putc((char)__builtin_va_arg(ap, int)); break;
            case 'd': {
                int v = __builtin_va_arg(ap, int);
                if (v < 0) { log_putc('-'); v = -v; }
                log_uint((uint64_t)v, 10, width, pad);
                break;
            }
            case 'u': log_uint(__builtin_va_arg(ap, unsigned), 10, width, pad); break;
            case 'x': log_uint(__builtin_va_arg(ap, unsigned), 16, width, pad); break;
            case '%': log_putc('%'); break;
            default:  log_putc('?'); break;
        }
        if (!*p) break;
    }
    __builtin_va_end(ap);
}

void wifi_log_save(void) {
    if (s_log_full) log_puts("\n[журнал обрезан: кончилось место]\n");
    int r = fs_write_file("/wifi.log", s_log, (uint32_t)s_log_len);
    if (r < 0) kputs("Wi-Fi: не удалось записать /wifi.log\n", 0xFF5555);
}

// ------------------------------------------------------------------ время
static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t ms_now(void) { return g_tsc_per_ms ? tsc_now() / g_tsc_per_ms : 0; }

// ------------------------------------------------------------------ состояние
int wifi_state(void) { return s_state; }
const char* wifi_chip_name(void) { return s_chip; }
const uint8_t* wifi_mac(void) { return s_state == WIFI_READY ? ath9k_mac() : 0; }

int wifi_init(void) {
    s_log_len = 0;
    s_log_full = 0;
    wlog("overOS Wi-Fi: журнал драйвера\n");
    int r = ath9k_probe(&s_chip);
    if (r == 0) {
        // Карты нет — это не ошибка (настольный ПК), журнал не пишем
        s_state = WIFI_NONE;
        return s_state;
    }
    if (r == 2) {
        wlog("Карта %s: этот чип пока не поддерживается\n", s_chip);
        s_state = WIFI_UNSUPPORTED;
        wifi_log_save();
        return s_state;
    }
    s_state = (ath9k_attach() == 0) ? WIFI_READY : WIFI_FAILED;
    wlog(s_state == WIFI_READY ? "\nИтог: чип поднят\n" : "\nИтог: чип НЕ поднят\n");
    wifi_log_save();
    return s_state;
}

// ------------------------------------------------------------------ сети
static wifi_bss_t s_bss[WIFI_MAX_BSS];
static int s_nbss = 0;

static int mac_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// Счётчики одного канала: что вообще слышно, даже если это не биконы
typedef struct { int frames, ok, crc, phy, beacons, other; int best_rssi; } chan_stat_t;

// Бикон или ответ на probe: заголовок 24 байта, затем метка времени (8),
// интервал (2), возможности (2) и элементы (id, длина, данные)
static void take_frame(const ath9k_frame_t* f, int chan, chan_stat_t* st) {
    st->frames++;
    if (f->crc_err) st->crc++;
    if (f->phy_err) st->phy++;
    if (!f->ok) return;
    st->ok++;
    if (f->rssi > st->best_rssi) st->best_rssi = f->rssi;

    const uint8_t* d = f->data;
    if (f->len < 36) { st->other++; return; }
    uint16_t fc = (uint16_t)(d[0] | (d[1] << 8));
    int type = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
    if (type != 0 || (sub != 8 && sub != 5)) { st->other++; return; }
    st->beacons++;

    const uint8_t* bssid = d + 16;
    uint16_t cap = (uint16_t)(d[34] | (d[35] << 8));
    char ssid[33];
    for (int i = 0; i < 33; i++) ssid[i] = '\0';
    int ds_chan = chan, rsn = 0;
    for (int p = 36; p + 2 <= f->len; ) {
        int id = d[p], l = d[p + 1];
        if (p + 2 + l > f->len) break;
        if (id == 0 && l <= 32) {
            for (int i = 0; i < l; i++) ssid[i] = (char)d[p + 2 + i];
            ssid[l] = '\0';
        } else if (id == 3 && l == 1) {
            ds_chan = d[p + 2];
        } else if (id == 48) {
            rsn = 1;
        }
        p += 2 + l;
    }
    // Бикон соседнего канала, просочившийся через фильтр, нам тоже годится:
    // канал берём из самого бикона

    int dbm = (f->rssi == -128) ? -100 : f->rssi - 95;
    wifi_bss_t* b = 0;
    for (int i = 0; i < s_nbss; i++) if (mac_eq(s_bss[i].bssid, bssid)) { b = &s_bss[i]; break; }
    if (!b) {
        if (s_nbss >= WIFI_MAX_BSS) return;
        b = &s_bss[s_nbss++];
        for (int i = 0; i < 6; i++) b->bssid[i] = bssid[i];
        for (int i = 0; i < 33; i++) b->ssid[i] = ssid[i];
        b->channel = ds_chan;
        b->rssi_dbm = dbm;
        b->beacons = 0;
        wlog("  + %02x:%02x:%02x:%02x:%02x:%02x \"%s\" канал %d, %d dBm\n",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ssid, ds_chan, dbm);
    }
    b->beacons++;
    if (dbm > b->rssi_dbm) b->rssi_dbm = dbm;
    b->secure = (cap & 0x10) ? 1 : 0;
    b->rsn = rsn;
    if (!b->ssid[0] && ssid[0]) for (int i = 0; i < 33; i++) b->ssid[i] = ssid[i];
}

int wifi_scan(void) {
    if (s_state != WIFI_READY) return -1;
    s_nbss = 0;
    wlog("\n=== Скан: каналы 1..13, по 200 мс ===\n");
    int total_frames = 0;
    for (int ch = 1; ch <= 13; ch++) {
        int freq = 2407 + 5 * ch;
        chan_stat_t st;
        st.frames = st.ok = st.crc = st.phy = st.beacons = st.other = 0;
        st.best_rssi = -128;
        wlog("канал %d (%d МГц)\n", ch, freq);
        if (ath9k_set_channel(freq) != 0) {
            wlog("  сброс на канал не удался\n");
            continue;
        }
        uint64_t t0 = ms_now();
        ath9k_frame_t f;
        while (ms_now() - t0 < 200) {
            while (ath9k_rx_poll(&f)) take_frame(&f, ch, &st);
            __asm__ volatile ("pause");
        }
        ath9k_rx_stop();
        total_frames += st.frames;
        wlog("  кадров %d: целых %d, CRC %d, PHY %d; биконов %d, прочих %d; лучший RSSI %d\n",
             st.frames, st.ok, st.crc, st.phy, st.beacons, st.other, st.best_rssi);
    }
    if (total_frames == 0) {
        wlog("\nЗа весь скан не пришло ни одного кадра. Регистры:\n");
        ath9k_dump_regs();
    }
    // Сильные сверху
    for (int i = 0; i < s_nbss; i++)
        for (int j = i + 1; j < s_nbss; j++)
            if (s_bss[j].rssi_dbm > s_bss[i].rssi_dbm) {
                wifi_bss_t t;
                wbcopy(&t, &s_bss[i], sizeof(t));
                wbcopy(&s_bss[i], &s_bss[j], sizeof(t));
                wbcopy(&s_bss[j], &t, sizeof(t));
            }
    wlog("Итог скана: сетей %d, кадров %d\n", s_nbss, total_frames);
    wifi_log_save();
    return s_nbss;
}

int wifi_results(wifi_bss_t* out, int max) {
    int n = s_nbss < max ? s_nbss : max;
    wbcopy(out, s_bss, n * (int)sizeof(wifi_bss_t));
    return n;
}

// ------------------------------------------------------------------ команда
static void put_num(int v, uint32_t col) {
    char b[16];
    int n = 0, neg = v < 0;
    if (neg) v = -v;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 14);
    if (neg) b[n++] = '-';
    char o[16];
    int k = 0;
    while (n) o[k++] = b[--n];
    o[k] = '\0';
    kputs(o, col);
}

static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void print_status(void) {
    switch (s_state) {
        case WIFI_NONE:        kputs("Wi-Fi: карты нет\n", 0xFFFF55); return;
        case WIFI_UNSUPPORTED: kputs("Wi-Fi: ", 0xAAAAAA); kputs(s_chip, 0xFFFFFF);
                               kputs(" — чип не поддерживается\n", 0xFFFF55); return;
        case WIFI_FAILED:      kputs("Wi-Fi: ", 0xAAAAAA); kputs(s_chip, 0xFFFFFF);
                               kputs(" — не поднялся, подробности в /wifi.log\n", 0xFF5555); return;
        default: break;
    }
    const uint8_t* m = ath9k_mac();
    const char* hx = "0123456789ABCDEF";
    char mac[18];
    for (int i = 0; i < 6; i++) {
        mac[i * 3] = hx[m[i] >> 4];
        mac[i * 3 + 1] = hx[m[i] & 15];
        mac[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
    kputs("Wi-Fi: ", 0xAAAAAA); kputs(s_chip, 0xFFFFFF);
    kputs(", MAC ", 0xAAAAAA); kputs(mac, 0xFFFFFF);
    kputs(", готов\n", 0x55FF55);
}

void cmd_wifi(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args || str_eq(args, "status")) {
        print_status();
        kputs("  wifi scan — список сетей, wifi log — журнал, wifi save — записать /wifi.log\n", 0xAAAAAA);
        return;
    }
    if (str_eq(args, "log")) {
        s_log[s_log_len] = '\0';
        kputs(s_log, 0xFFFFFF);
        kputs("\n", 0);
        return;
    }
    if (str_eq(args, "save")) {
        wifi_log_save();
        kputs("Журнал записан в /wifi.log\n", 0x55FF55);
        return;
    }
    if (str_eq(args, "regs")) {
        if (s_state == WIFI_READY || s_state == WIFI_FAILED) ath9k_dump_regs();
        wifi_log_save();
        kputs("Регистры дописаны в журнал (/wifi.log)\n", 0x55FF55);
        return;
    }
    if (str_eq(args, "scan")) {
        if (s_state != WIFI_READY) { print_status(); return; }
        kputs("Слушаю эфир, каналы 1..13 (около трёх секунд)...\n", 0xAAAAAA);
        flush_buffer();
        int n = wifi_scan();
        if (n <= 0) {
            kputs("Сетей не найдено. Журнал: /wifi.log\n", 0xFFFF55);
            return;
        }
        for (int i = 0; i < s_nbss; i++) {
            const wifi_bss_t* b = &s_bss[i];
            put_num(b->rssi_dbm, b->rssi_dbm > -60 ? 0x55FF55 : b->rssi_dbm > -75 ? 0xFFFF55 : 0xFF8855);
            kputs(" dBm  к.", 0xAAAAAA);
            put_num(b->channel, 0xFFFFFF);
            kputs(b->rsn ? "  WPA2  " : b->secure ? "  WEP   " : "  open  ", 0xAAAAAA);
            if (b->ssid[0]) {
                // Непечатные байты не пускаем в консоль
                char s[33];
                for (int k = 0; k < 33; k++) {
                    unsigned char c = (unsigned char)b->ssid[k];
                    s[k] = (c && c < 0x20) ? '?' : (char)c;
                    if (!c) break;
                }
                kputs(s, 0xFFFFFF);
            } else {
                kputs("<скрытая сеть>", 0xAAAAAA);
            }
            kputs("\n", 0);
        }
        kputs("Журнал: /wifi.log\n", 0xAAAAAA);
        return;
    }
    kputs("wifi: неизвестная подкоманда\n", 0xFF5555);
}
