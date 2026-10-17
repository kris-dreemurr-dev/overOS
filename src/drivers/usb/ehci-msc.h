#ifndef EHCI_MSC_H
#define EHCI_MSC_H
// ehci-msc.h — накопители (Bulk-Only Transport) поверх EHCI. См. ehci-msc.c.

#include <stdint.h>

#define EHCI_MSC_MAX 4

// Устройство уже адресовано и сконфигурировано (SET_CONFIGURATION сделал
// ehci.c): найти bulk-точки, спросить размер и зарегистрировать как диск.
// Возвращает индекс накопителя или -1.
int  ehci_msc_attach(uint8_t dev_addr, int root_port, const uint8_t* cfg_desc, uint16_t cfg_len,
                     uint16_t vid, uint16_t pid);
// Устройство выдернули: диск пропадает в реестре, на шину к нему не ходим
void ehci_msc_detach(int idx);
int  ehci_msc_alive_count(void);
// Накопитель перестал отвечать (таймаут, ошибка транзакции), хотя порт его
// ещё видит: ehci.c переподнимет этот порт
int  ehci_msc_is_gone(int idx);

#endif
