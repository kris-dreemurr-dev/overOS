#ifndef USB_HID_H
#define USB_HID_H
// hid.h — USB-клавиатуры и мыши (класс HID, «загрузочный» протокол).
//
// Контроллер (xhci-hid.c) приносит готовые отчёты сюда из прерывания таймера;
// здесь они превращаются в то, что система и так понимает: клавиатура — в
// скан-коды набора 1 (как от PS/2), мышь — в движения и кнопки. Всё это
// копится в кольце и разбирается в ps2_hw_service (usb_hid_drain) — там же,
// где байты PS/2, так что для TTY, композитора и игр USB-клавиатура ничем не
// отличается от встроенной. Повтор зажатой клавиши (у USB его нет) — тоже здесь.

#include <stdint.h>

#define HID_KBD_MAX 4

int  hid_kbd_alloc(void);                                  // номер клавиатуры или -1
void hid_kbd_free(int kb);                                 // отключили: отпустить её клавиши
void hid_kbd_report(int kb, const uint8_t* r, int len);   // 8 байт загрузочного отчёта
void hid_mouse_report(const uint8_t* r, int len);          // кнопки, dx, dy, [колесо]
void hid_mouse_count(int delta);

// Из прерывания таймера (1 мс): опрос контроллеров и повтор клавиш
void usb_hid_tick(void);
// Из ps2_hw_service: отдать накопленное клавиатуре и мыши ядра
void usb_hid_drain(void);
// Для lsusb
void usb_hid_counts(int* kbds, int* mice);

#endif
