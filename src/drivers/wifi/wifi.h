#ifndef WIFI_H
#define WIFI_H
// wifi.h — общий слой Wi-Fi: какая карта стоит, журнал шагов, список сетей.
//
// Сейчас умеем одно: слушать эфир (пассивный скан) и собирать биконы точек
// доступа. Передатчик не включаем вовсе, подключения, WPA2 и сетевого стека
// здесь нет. Железо — драйвер ath9k.c (Atheros AR9565); слой выше знает о нём
// только через ath9k.h.

#include <stdint.h>

#define WIFI_MAX_BSS 40

typedef struct {
    char     ssid[33];      // UTF-8, пустая — скрытая сеть
    uint8_t  bssid[6];
    int      channel;       // из DS Parameter Set, иначе канал, где услышали
    int      rssi_dbm;      // лучший уровень за скан
    int      beacons;       // сколько кадров от неё пришло
    int      secure;        // 1 — есть защита (бит Privacy)
    int      rsn;           // 1 — WPA2 (есть RSN IE)
} wifi_bss_t;

// Состояние адаптера
enum { WIFI_NONE = 0, WIFI_UNSUPPORTED, WIFI_FAILED, WIFI_READY };

// Найти карту и поднять её. Вызывается загрузчиком. Возвращает WIFI_*.
int  wifi_init(void);
int  wifi_state(void);
const char* wifi_chip_name(void);      // "Atheros AR9565" и т. п.
const uint8_t* wifi_mac(void);         // MAC из калибровки карты (или 0)

// Пассивный скан каналов 1..13: число найденных сетей, <0 — ошибка
int  wifi_scan(void);
int  wifi_results(wifi_bss_t* out, int max);

// Журнал: всё, что делает драйвер, с регистрами. Пишется в память и на
// флешку в /wifi.log (после загрузки и после каждого скана), чтобы его можно
// было прочитать на другом компьютере.
void wlog(const char* fmt, ...);
void wifi_log_save(void);

// Команда оболочки: wifi [scan|log|save|regs]
void cmd_wifi(const char* args);

// Копирование и обнуление без libc. Через rep movsb/stosb, а не циклом:
// GCC на -O2 сам превращает такой цикл в вызов memcpy, которого в ядре нет.
static inline void wbcopy(void* d, const void* s, uint64_t n) {
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline void wbzero(void* d, uint64_t n) {
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(0) : "memory");
}

#endif
