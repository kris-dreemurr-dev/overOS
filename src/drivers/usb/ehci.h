#ifndef EHCI_H
#define EHCI_H

#include <stdint.h>
#include "../pci.h"

typedef struct {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t usbintr;
    volatile uint32_t frindex;
    volatile uint32_t ctrldssegment;
    volatile uint32_t periodiclistbase;
    volatile uint32_t asynclistaddr;
    volatile uint32_t reserved[9];
    volatile uint32_t configflag;
    volatile uint32_t portsc[1];
} __attribute__((packed)) ehci_op_regs_t;

// Queue Element Transfer Descriptor (64 байта с учетом выравнивания)
typedef struct ehci_qtd {
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t buffer_hi[5]; // Обязательно для 64-битных EHCI
} __attribute__((aligned(32), packed)) ehci_qtd_t;

// Queue Head (128 байт с учетом выравнивания)
typedef struct ehci_qh {
    volatile uint32_t horizontal_link;
    volatile uint32_t ep_caps[2];
    volatile uint32_t current_qtd;
    // Overlay
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t buffer_hi[5]; // Обязательно для 64-битных EHCI
} __attribute__((aligned(64), packed)) ehci_qh_t;

#define EHCI_PID_OUT    0
#define EHCI_PID_IN     1
#define EHCI_PID_SETUP  2

void ehci_init(void);

#endif