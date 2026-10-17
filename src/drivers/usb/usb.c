// =============================================================================
// usb.c — общий слой USB: какие контроллеры поднимать и как следить за портами.
//
// Раньше выбор драйвера жил в xhci.c, и xHCI звал ehci_init() сам -- один
// драйвер контроллера командовал другим. Теперь оба равноправны и ничего друг о
// друге не знают, а решает этот файл:
//   * xHCI поднимаем первым, если он есть: на новых чипсетах EHCI нет вовсе;
//   * EHCI поднимаем всегда, когда он есть на шине: в режиме Smart Auto BIOS
//     отдаёт ему порты, и старые машины от появления xHCI не страдают;
//   * найденные накопители оба регистрируют в drivers/blk.c, и для системы
//     дальше нет разницы, через какой контроллер воткнута флешка.
// =============================================================================

#include "usb.h"
#include "xhci.h"
#include "ehci.h"

extern void sched_yield(void);

static int s_driver = USB_DRV_NONE;
static int s_xhci_up = 0, s_ehci_up = 0;
static int s_verbose = 0;

void usb_set_verbose(int v) { s_verbose = v; xhci_set_verbose(v); }
int  usb_verbose(void) { return s_verbose; }

// ------------------------------------------------------------------ замок
static volatile int s_lock = 0;

void usb_lock(void) {
    while (__sync_lock_test_and_set(&s_lock, 1)) sched_yield();
}

void usb_unlock(void) {
    __sync_lock_release(&s_lock);
}

// ------------------------------------------------------------------ строки
void usb_inquiry_strings(const uint8_t* inq, char* vendor, char* product) {
    for (int i = 0; i < 8; i++)  vendor[i]  = (inq[8 + i]  >= 32 && inq[8 + i]  < 127) ? (char)inq[8 + i]  : ' ';
    for (int i = 0; i < 16; i++) product[i] = (inq[16 + i] >= 32 && inq[16 + i] < 127) ? (char)inq[16 + i] : ' ';
    vendor[8] = '\0';
    product[16] = '\0';
}

// ------------------------------------------------------------------ старт
int usb_bootstrap(void) {
    usb_lock();
    if (xhci_present() && xhci_init() == 0) {
        s_xhci_up = 1;
        s_driver = USB_DRV_XHCI;
    }
    // EHCI -- всегда, когда он есть: в Smart Auto флешка может оказаться на
    // нём, даже если xHCI поднялся.
    if (ehci_init() == 0) {
        s_ehci_up = 1;
        // Шину "ведёт" тот, у кого накопитель. Если xHCI пуст, а EHCI нашёл
        // флешку -- в строке загрузки честно напишем EHCI.
        if (s_driver == USB_DRV_NONE || (!xhci_storage_found() && ehci_storage_found()))
            s_driver = USB_DRV_EHCI;
    }
    usb_unlock();
    return s_driver;
}

int usb_active_driver(void) { return s_driver; }

// ------------------------------------------------------------------ опрос
void usb_poll(void) {
    usb_lock();
    if (s_xhci_up) xhci_poll();
    if (s_ehci_up) ehci_poll();
    usb_unlock();
}
