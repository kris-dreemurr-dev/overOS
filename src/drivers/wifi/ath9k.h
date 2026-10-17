#ifndef ATH9K_H
#define ATH9K_H
// ath9k.h — драйвер Atheros AR9565 (семейство AR9300, в Linux — ath9k).
// Только приём в 2,4 ГГц, 20 МГц: этого хватает, чтобы слушать биконы.

#include <stdint.h>

// Есть ли на шине карта Atheros. name — что это за чип (для журнала и
// загрузочной строки). 1 — нашлась AR9565, которую мы умеем; 2 — Atheros, но
// другая; 0 — нет.
int  ath9k_probe(const char** name);

// Поднять чип: сброс, пробуждение, калибровка из OTP, MAC. 0 — успех.
int  ath9k_attach(void);
const uint8_t* ath9k_mac(void);

// Полный сброс на частоту freq (МГц, 2412..2472) и запуск приёма. 0 — успех.
int  ath9k_set_channel(int freq);
void ath9k_rx_stop(void);

// Принятый кадр. data указывает в буфер DMA и годится до следующего вызова.
typedef struct {
    const uint8_t* data;
    int  len;            // без FCS
    int  rssi;           // дБ над шумом, -128 — неизвестно
    int  ok;             // 1 — кадр цел
    int  crc_err, phy_err, phy_code;
    int  rate;
} ath9k_frame_t;

// 1 — взят кадр, 0 — очередь пуста
int  ath9k_rx_poll(ath9k_frame_t* f);

// Ключевые регистры в журнал (для разбора, если эфир молчит)
void ath9k_dump_regs(void);

#endif
