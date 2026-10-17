// =============================================================================
// ehci.c — контроллер USB 2.0 (EHCI) и перечисление устройств на нём.
//
// Что здесь есть:
//   * передача контроллера от BIOS, сброс, запуск;
//   * управляющие передачи по нулевой точке (через асинхронное расписание);
//   * перечисление: каждый корневой порт, а за ним -- хаб. На ноутбуках Intel
//     за корневым портом стоит встроенный хаб чипсета (RMH), и все внешние
//     разъёмы на самом деле -- его порты;
//   * горячее подключение (ehci_poll): корневые порты по отметке смены
//     подключения, порты хабов -- запросом GET_STATUS.
// Накопители дальше доводит ehci-msc.c и регистрирует как диски в
// drivers/blk.c. О файловой системе драйвер не знает ничего: раньше флешку
// монтировал прямо отсюда (fs_mount), и вторую флешку было просто некуда деть.
//
// Low/Full-speed устройства (клавиатуры, мыши) за хабом требуют split-
// транзакций, которых здесь нет; на корневом порту их забирает companion-
// контроллер.
// =============================================================================

#include "ehci.h"
#include "ehci-msc.h"
#include "usb.h"

extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern void sleep_ms(uint32_t ms);

// Перевод виртуального адреса ядра (Higher-Half) в физический адрес для DMA железа
#define V2P(va) (((uintptr_t)(va) >= 0xFFFFFFFF80000000ULL) ? \
    (uint32_t)((uintptr_t)(va) - 0xFFFFFFFF80000000ULL) : \
    ((uintptr_t)(va) >= 0xFFFF800000000000ULL) ? \
    (uint32_t)((uintptr_t)(va) - 0xFFFF800000000000ULL) : \
    (uint32_t)(uintptr_t)(va))

static void print_num(int val, uint32_t color) {
    char buf[16];
    itoa(val, buf);
    kdebug(buf, color);
}

static void print_hex(uint32_t val, uint32_t color) {
    char hex_str[11] = {'0', 'x'};
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        hex_str[2 + i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    hex_str[10] = '\0';
    kdebug(hex_str, color);
}

static void usb_flush_range(volatile void* addr, uint32_t size) {
    if (!addr || size == 0) return;
    __asm__ __volatile__("mfence" ::: "memory");
}

// Экспортируем op_regs для работы ehci-msc.c
ehci_op_regs_t* op_regs = 0;
static int s_up = 0;
static int s_num_ports = 0;
static pci_device_t g_ehci_dev;
static uint8_t g_eecp = 0;

static volatile ehci_qh_t qh_control __attribute__((aligned(64)));
static volatile ehci_qtd_t qtd_setup __attribute__((aligned(32)));
static volatile ehci_qtd_t qtd_data  __attribute__((aligned(32)));
static volatile ehci_qtd_t qtd_status __attribute__((aligned(32)));
static volatile uint8_t shared_setup_pkt[8] __attribute__((aligned(64)));
static volatile uint8_t dma_buffer[4096] __attribute__((aligned(4096)));
static volatile uint32_t periodic_list[1024] __attribute__((aligned(4096)));

#define HUB_FEAT_PORT_RESET    4
#define HUB_FEAT_PORT_POWER    8
#define HUB_FEAT_C_PORT_CONN   16
#define HUB_FEAT_C_PORT_RESET  20

#define HUB_PORT_STAT_CONN     (1 << 0)
#define HUB_PORT_STAT_ENABLE   (1 << 1)
#define HUB_PORT_STAT_RESET    (1 << 4)
#define HUB_PORT_STAT_HIGH_SPD (1 << 10)
#define HUB_PORT_CHG_CONN      (1 << 0)

// ------------------------------------------------------- таблица устройств
// Каждое устройство на шине: где оно сидит (корневой порт, хаб и его порт) и
// кто оно (хаб, накопитель, что-то ещё). По этой таблице опрос понимает, что
// пропало вместе с выдернутым хабом и на какой порт пришло новое.
#define EDEV_MAX 16
typedef struct {
    int     used;
    uint8_t addr;           // адрес на шине (1..127)
    uint8_t mps0;           // размер пакета нулевой точки
    int     root_port;      // корневой порт контроллера (с нуля)
    uint8_t hub_addr;       // 0 -- сидит прямо на корневом порту
    uint8_t hub_port;       // порт хаба (с единицы)
    uint8_t is_hub;
    uint8_t hub_nports;
    int     msc;            // индекс в ehci-msc.c, -1 -- не накопитель
} ehci_dev_t;

static ehci_dev_t s_dev[EDEV_MAX];
static uint8_t    s_addr_used[128];

static int alloc_addr(void) {
    for (int a = 1; a < 128; a++) if (!s_addr_used[a]) { s_addr_used[a] = 1; return a; }
    return -1;
}

static ehci_dev_t* dev_new(void) {
    for (int i = 0; i < EDEV_MAX; i++) {
        if (s_dev[i].used) continue;
        ehci_dev_t* d = &s_dev[i];
        for (int k = 0; k < (int)sizeof(*d); k++) ((uint8_t*)d)[k] = 0;
        d->used = 1;
        d->msc = -1;
        return d;
    }
    return 0;
}

static int pci_find_ehci(pci_device_t* out_dev) {
    if (!pci_find_usb_by_progif(0x20, out_dev)) return 0;
    out_dev->bar0 = pci_config_read(out_dev->bus, out_dev->slot, out_dev->func, 0x10) & ~0xF;
    return 1;
}

static uint8_t ehci_bios_handoff(pci_device_t* dev, volatile uint8_t* cap_regs) {
    uint32_t hccparams = *(volatile uint32_t*)(cap_regs + 0x08);
    uint8_t eecp = (hccparams >> 8) & 0xFF;

    if (eecp < 0x40) {
        kdebug("  [i] No EECP (BIOS handoff not applicable on this HC).\n", 0xAAAAAA);
        return 0;
    }

    uint32_t legsup = pci_config_read(dev->bus, dev->slot, dev->func, eecp);
    kdebug("  [i] USBLEGSUP before handoff: ", 0xAAAAAA); print_hex(legsup, 0xFFFF55); kdebug("\n", 0);

    if (legsup & (1 << 16)) {
        kdebug("  [!] BIOS owns EHCI. Requesting OS ownership...\n", 0xFFFF55);
        pci_config_write(dev->bus, dev->slot, dev->func, eecp, legsup | (1 << 24));

        int timeout = 100;
        while (timeout--) {
            legsup = pci_config_read(dev->bus, dev->slot, dev->func, eecp);
            if (!(legsup & (1 << 16))) break;
            sleep_ms(2);
        }
        if (legsup & (1 << 16)) {
            pci_config_write(dev->bus, dev->slot, dev->func, eecp, (1 << 24));
        }
    }
    pci_config_write(dev->bus, dev->slot, dev->func, eecp + 4, 0);

    legsup = pci_config_read(dev->bus, dev->slot, dev->func, eecp);
    kdebug("  [i] USBLEGSUP after handoff:  ", 0xAAAAAA); print_hex(legsup, legsup & (1 << 16) ? 0xFF5555 : 0x55FF55);
    if (legsup & (1 << 16)) kdebug(" [!] BIOS STILL claims ownership!\n", 0xFF5555);
    else if (legsup & (1 << 24)) kdebug(" [+] OS-Owned confirmed.\n", 0x55FF55);
    else kdebug(" [?] Neither bit set - ambiguous.\n", 0xFFFF55);

    return eecp;
}

static int ehci_reset_port(int port_idx) {
    uint32_t val = op_regs->portsc[port_idx];

    kdebug("  [PORT ", 0xAAAAAA); print_num(port_idx, 0xAAAAAA);
    kdebug("] Initial PORTSC: ", 0xAAAAAA); print_hex(val, 0xFFFF55); kdebug("\n", 0);

    if (!(val & 1)) return 0;

    kdebug("  [+] Device present on Port ", 0x55FF55); print_num(port_idx, 0x55FF55); kdebug("\n", 0);

    if ((val & (3 << 10)) == (1 << 10)) {
        kdebug("  [-] Low-Speed device. Releasing to Companion...\n", 0xAAAAAA);
        op_regs->portsc[port_idx] = (op_regs->portsc[port_idx] & ~0x2A) | (1 << 13) | (1 << 12);
        return 0;
    }

    kdebug("  -> Asserting Port Reset...\n", 0xAAAAAA);
    op_regs->portsc[port_idx] = (op_regs->portsc[port_idx] & ~0x2A) | (1 << 8) | (1 << 12);
    sleep_ms(60);

    kdebug("  -> Deasserting Port Reset...\n", 0xAAAAAA);
    op_regs->portsc[port_idx] = (op_regs->portsc[port_idx] & ~0x2A & ~(1 << 8)) | (1 << 12);

    int timeout = 100;
    while ((op_regs->portsc[port_idx] & (1 << 8)) && timeout--) sleep_ms(2);

    sleep_ms(100);

    val = op_regs->portsc[port_idx];
    kdebug("  [PORT ", 0xAAAAAA); print_num(port_idx, 0xAAAAAA);
    kdebug("] Post-Reset PORTSC: ", 0xAAAAAA); print_hex(val, 0xFFFF55); kdebug("\n", 0);

    if (val & (1 << 2)) {
        kdebug("  [+] Port Enabled as High-Speed (USB 2.0)!\n", 0x55FF55);
        return 1;
    }
    return 0;
}

static int ehci_control_transfer(void* setup_pkt, void* buffer, int len, int is_in, uint8_t dev_addr, uint8_t max_packet0) {
    if (op_regs->usbcmd & (1 << 5)) {
        op_regs->usbcmd &= ~(1 << 5);
        while (op_regs->usbsts & (1 << 15)) { }
    }

    for (int i = 0; i < 8; i++) shared_setup_pkt[i] = ((uint8_t*)setup_pkt)[i];

    // 1. SETUP qTD
    qtd_setup.next_qtd = (len > 0) ? V2P(&qtd_data) : V2P(&qtd_status); // <-- V2P
    qtd_setup.alt_next_qtd = 1;
    qtd_setup.token = (1 << 7) | (EHCI_PID_SETUP << 8) | (3 << 10) | (8 << 16);
    qtd_setup.buffer[0] = V2P(&shared_setup_pkt); // <-- V2P
    for (int i = 1; i < 5; i++) qtd_setup.buffer[i] = 0;
    for (int i = 0; i < 5; i++) qtd_setup.buffer_hi[i] = 0;

    // 2. DATA qTD
    if (len > 0) {
        qtd_data.next_qtd = V2P(&qtd_status); // <-- V2P
        qtd_data.alt_next_qtd = 1;
        uint32_t pid = is_in ? EHCI_PID_IN : EHCI_PID_OUT;
        qtd_data.token = (1 << 7) | (pid << 8) | (3 << 10) | (1U << 31) | (len << 16);
        qtd_data.buffer[0] = V2P(buffer); // <-- V2P
        for (int i = 1; i < 5; i++) qtd_data.buffer[i] = 0;
        for (int i = 0; i < 5; i++) qtd_data.buffer_hi[i] = 0;
    }

    // 3. STATUS qTD
    qtd_status.next_qtd = 1;
    qtd_status.alt_next_qtd = 1;
    uint32_t status_pid = (len == 0 || !is_in) ? EHCI_PID_IN : EHCI_PID_OUT;
    qtd_status.token = (1 << 7) | (status_pid << 8) | (3 << 10) | (1U << 31);
    for (int i = 0; i < 5; i++) qtd_status.buffer[i] = 0;
    for (int i = 0; i < 5; i++) qtd_status.buffer_hi[i] = 0;

    // 4. QH: High-Speed (EPS=2), RL=4
    qh_control.horizontal_link = V2P(&qh_control) | 0x02; // <-- V2P
    qh_control.ep_caps[0] = (4 << 28) | ((uint32_t)max_packet0 << 16) | (1 << 15) | (1 << 14) | (2 << 12) | (dev_addr & 0x7F);
    qh_control.ep_caps[1] = (1 << 30);
    qh_control.current_qtd = 0;
    qh_control.next_qtd = V2P(&qtd_setup); // <-- V2P
    qh_control.alt_next_qtd = 1;
    qh_control.token = 0;
    for (int i = 0; i < 5; i++) {
        qh_control.buffer[i] = 0;
        qh_control.buffer_hi[i] = 0;
    }

    // ... сброс кэшей (flush_range) оставляем без изменений ...
    usb_flush_range(&shared_setup_pkt, 8);
    if (len > 0) usb_flush_range(buffer, len);
    usb_flush_range(&qtd_setup, sizeof(qtd_setup));
    if (len > 0) usb_flush_range(&qtd_data, sizeof(qtd_data));
    usb_flush_range(&qtd_status, sizeof(qtd_status));
    usb_flush_range(&qh_control, sizeof(qh_control));

    // 5. Запуск расписания (используем V2P)
    op_regs->asynclistaddr = V2P(&qh_control); // <-- V2P
    
    // ... остальной код функции ...
    op_regs->usbcmd |= (1 << 5);
    while (!(op_regs->usbsts & (1 << 15))) { }

    // 5b. Doorbell проверки доступа контроллера
    op_regs->usbsts = (1 << 5);
    op_regs->usbcmd |= (1 << 6);
    for (int i = 0; i < 20 && !(op_regs->usbsts & (1 << 5)); i++) sleep_ms(1);
    if (!(op_regs->usbsts & (1 << 5))) {
        kdebug("  [!] IAA Doorbell TIMEOUT: HC did not visit async list!\n", 0xFF5555);
    }
    op_regs->usbsts = (1 << 5);

    // 6. Ожидание завершения
    int timeout_ms = 1000;
    while (timeout_ms > 0) {
        sleep_ms(1);
        timeout_ms--;

        if (g_eecp >= 0x40 && (timeout_ms % 50) == 0) {
            uint32_t legsup = pci_config_read(g_ehci_dev.bus, g_ehci_dev.slot, g_ehci_dev.func, g_eecp);
            if (legsup & (1 << 16)) {
                kdebug("  [!] BIOS RECLAIMED EHCI ownership! Re-asserting...\n", 0xFF5555);
                pci_config_write(g_ehci_dev.bus, g_ehci_dev.slot, g_ehci_dev.func, g_eecp, (1 << 24));
                pci_config_write(g_ehci_dev.bus, g_ehci_dev.slot, g_ehci_dev.func, g_eecp + 4, 0);
            }
        }

        usb_flush_range(&qtd_status, sizeof(qtd_status));
        usb_flush_range(&qtd_setup, sizeof(qtd_setup));
        if (len > 0) usb_flush_range(buffer, len);

        if (qtd_setup.token & (1 << 6) || (len > 0 && qtd_data.token & (1 << 6)) || (qtd_status.token & (1 << 6))) {
            kdebug("  [!] Transaction HALTED! Tokens: Setup=", 0xFF5555);
            print_hex(qtd_setup.token, 0xFF5555);
            kdebug(" Status=", 0xAAAAAA); print_hex(qtd_status.token, 0xFF5555);
            kdebug("\n", 0);
            return 0;
        }

        if (!(qtd_status.token & (1 << 7))) {
            if (len > 0) usb_flush_range(buffer, len);
            return 1;
        }
    }

    usb_flush_range(&qh_control, sizeof(qh_control));
    kdebug("  [!] EHCI Control Transfer TIMEOUT!\n", 0xFF5555);
    return 0;
}


// ------------------------------------------------------- перечисление
static void dev_remove(ehci_dev_t* d);
static int  enumerate_at_addr0(int root_port, uint8_t hub_addr, uint8_t hub_port, int depth);

// Хаб: конфигурация, питание портов и все, кто уже воткнут
static void hub_attach_port(ehci_dev_t* hub, uint8_t p, int depth);

static int hub_setup(ehci_dev_t* hub, int depth) {
    uint8_t a = hub->addr, m = hub->mps0;
    kdebug("[EHCI] hub at address ", 0x55FFFF); print_num(a, 0x55FFFF); kdebug("\n", 0);

    uint8_t set_cfg[8] = { 0x00, USB_REQ_SET_CONFIG, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (!ehci_control_transfer(set_cfg, 0, 0, 0, a, m)) {
        kdebug("  [!] Failed to set Hub configuration.\n", 0xFF5555);
        return -1;
    }
    sleep_ms(20);

    uint8_t get_hub_desc[8] = { 0xA0, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DESC_HUB, 0x00, 0x00, 0x08, 0x00 };
    if (!ehci_control_transfer(get_hub_desc, (void*)dma_buffer, 8, 1, a, m)) {
        kdebug("  [!] Failed to get Hub Descriptor.\n", 0xFF5555);
        return -1;
    }
    hub->hub_nports = dma_buffer[2];
    uint32_t pwr_good_delay = (uint32_t)dma_buffer[5] * 2;
    if (pwr_good_delay < 100) pwr_good_delay = 100;

    for (uint8_t p = 1; p <= hub->hub_nports; p++) {
        uint8_t pwr_pkt[8] = { 0x23, USB_REQ_SET_FEATURE, HUB_FEAT_PORT_POWER, 0x00, p, 0x00, 0x00, 0x00 };
        ehci_control_transfer(pwr_pkt, 0, 0, 0, a, m);
    }
    sleep_ms(pwr_good_delay);

    for (uint8_t p = 1; p <= hub->hub_nports; p++) hub_attach_port(hub, p, depth);
    return 0;
}

// Порт хаба: прочитать состояние, снять отметку о смене подключения и, если
// там кто-то есть, сбросить порт и перечислить устройство. Отметку снимаем
// всегда: по ней потом опрос узнаёт о выдёргивании и новой вставке.
static void hub_attach_port(ehci_dev_t* hub, uint8_t p, int depth) {
    uint8_t a = hub->addr, m = hub->mps0;
    uint8_t get_stat[8] = { 0xA3, USB_REQ_GET_STATUS, 0x00, 0x00, p, 0x00, 0x04, 0x00 };
    if (!ehci_control_transfer(get_stat, (void*)dma_buffer, 4, 1, a, m)) return;
    uint16_t st = dma_buffer[0] | (dma_buffer[1] << 8);

    uint8_t clr_conn[8] = { 0x23, USB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_CONN, 0x00, p, 0x00, 0x00, 0x00 };
    ehci_control_transfer(clr_conn, 0, 0, 0, a, m);
    if (!(st & HUB_PORT_STAT_CONN)) return;

    kdebug("  [+] Device on hub port ", 0x55FF55); print_num(p, 0xFFFF55); kdebug("\n", 0);

    uint8_t reset_pkt[8] = { 0x23, USB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, 0x00, p, 0x00, 0x00, 0x00 };
    ehci_control_transfer(reset_pkt, 0, 0, 0, a, m);
    sleep_ms(60);
    uint8_t clr_reset[8] = { 0x23, USB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_RESET, 0x00, p, 0x00, 0x00, 0x00 };
    ehci_control_transfer(clr_reset, 0, 0, 0, a, m);
    sleep_ms(20);

    if (!ehci_control_transfer(get_stat, (void*)dma_buffer, 4, 1, a, m)) return;
    st = dma_buffer[0] | (dma_buffer[1] << 8);
    if (!(st & HUB_PORT_STAT_ENABLE)) {
        kdebug("  [-] Port not enabled after reset.\n", 0xFFAA00);
        return;
    }
    if (!(st & HUB_PORT_STAT_HIGH_SPD)) {
        kdebug("  [!] Low/Full-Speed device on hub port. Skipped (requires split transactions).\n", 0xFFAA00);
        return;
    }
    sleep_ms(100);                                   // USB recovery time
    enumerate_at_addr0(hub->root_port, a, p, depth + 1);
}

// Устройство только что сброшено и отвечает по адресу 0: узнать размер
// пакета, дать адрес, прочитать дескрипторы и решить, кто это.
static int enumerate_at_addr0(int root_port, uint8_t hub_addr, uint8_t hub_port, int depth) {
    if (depth > 3) return -1;                        // хаб за хабом за хабом -- хватит

    uint8_t mps0 = 64;
    uint8_t get8[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DESC_DEVICE, 0x00, 0x00, 0x08, 0x00 };
    if (!ehci_control_transfer(get8, (void*)dma_buffer, 8, 1, 0, 64)) {
        kdebug("  [!] No device descriptor at address 0.\n", 0xFF5555);
        return -1;
    }
    if (dma_buffer[7] == 8 || dma_buffer[7] == 16 || dma_buffer[7] == 32 || dma_buffer[7] == 64) mps0 = dma_buffer[7];

    int addr = alloc_addr();
    if (addr < 0) return -1;
    uint8_t set_addr[8] = { 0x00, USB_REQ_SET_ADDRESS, (uint8_t)addr, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (!ehci_control_transfer(set_addr, 0, 0, 0, 0, mps0)) {
        kdebug("  [!] SET_ADDRESS failed.\n", 0xFF5555);
        s_addr_used[addr] = 0;
        return -1;
    }
    sleep_ms(15);

    uint8_t get_dev[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DESC_DEVICE, 0x00, 0x00, 0x12, 0x00 };
    if (!ehci_control_transfer(get_dev, (void*)dma_buffer, 18, 1, (uint8_t)addr, mps0)) {
        s_addr_used[addr] = 0;
        return -1;
    }
    uint8_t  dev_class = dma_buffer[4];
    uint16_t vid = dma_buffer[8]  | (dma_buffer[9] << 8);
    uint16_t pid = dma_buffer[10] | (dma_buffer[11] << 8);

    uint8_t get_cfg9[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DESC_CONFIG, 0x00, 0x00, 0x09, 0x00 };
    if (!ehci_control_transfer(get_cfg9, (void*)dma_buffer, 9, 1, (uint8_t)addr, mps0)) {
        s_addr_used[addr] = 0;
        return -1;
    }
    uint16_t total = dma_buffer[2] | (dma_buffer[3] << 8);
    if (total > sizeof(dma_buffer)) total = sizeof(dma_buffer);
    uint8_t get_cfg[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DESC_CONFIG, 0x00, 0x00,
                           (uint8_t)(total & 0xFF), (uint8_t)(total >> 8) };
    if (!ehci_control_transfer(get_cfg, (void*)dma_buffer, total, 1, (uint8_t)addr, mps0)) {
        s_addr_used[addr] = 0;
        return -1;
    }

    uint8_t cfg_value = dma_buffer[5];
    uint8_t if_class = 0;
    for (uint32_t off = 0; off + 2 <= total; ) {
        uint8_t dlen = dma_buffer[off], dtype = dma_buffer[off + 1];
        if (dlen == 0) break;
        if (dtype == USB_DESC_INTERFACE && off + 9 <= total) { if_class = dma_buffer[off + 5]; break; }
        off += dlen;
    }

    ehci_dev_t* d = dev_new();
    if (!d) { s_addr_used[addr] = 0; return -1; }
    d->addr = (uint8_t)addr;
    d->mps0 = mps0;
    d->root_port = root_port;
    d->hub_addr = hub_addr;
    d->hub_port = hub_port;

    kdebug("[EHCI] device ", 0x55FFFF); print_hex((uint32_t)vid << 16 | pid, 0xFFFF55);
    kdebug(" at address ", 0xAAAAAA); print_num(addr, 0xFFFFFF);
    kdebug(", class ", 0xAAAAAA); print_num(if_class ? if_class : dev_class, 0xFFFFFF); kdebug("\n", 0);

    if (dev_class == USB_CLASS_HUB || if_class == USB_CLASS_HUB) {
        d->is_hub = 1;
        hub_setup(d, depth);
    } else if (if_class == USB_CLASS_STORAGE) {
        uint8_t set_cfg[8] = { 0x00, USB_REQ_SET_CONFIG, cfg_value ? cfg_value : 1, 0x00, 0x00, 0x00, 0x00, 0x00 };
        if (!ehci_control_transfer(set_cfg, 0, 0, 0, (uint8_t)addr, mps0)) {
            kdebug("  [!] Failed to set device configuration!\n", 0xFF5555);
            return 0;
        }
        sleep_ms(20);
        d->msc = ehci_msc_attach((uint8_t)addr, root_port, (const uint8_t*)dma_buffer, total, vid, pid);
    }
    return 0;
}

// Устройство ушло: его диск (если это накопитель) пропадает в реестре, а если
// это хаб -- вместе с ним всё, что было за ним.
static void dev_remove(ehci_dev_t* d) {
    if (!d->used) return;
    if (d->is_hub) {
        for (int i = 0; i < EDEV_MAX; i++)
            if (s_dev[i].used && s_dev[i].hub_addr == d->addr && &s_dev[i] != d) dev_remove(&s_dev[i]);
    }
    if (d->msc >= 0) ehci_msc_detach(d->msc);
    s_addr_used[d->addr] = 0;
    d->used = 0;
}

// Подключено ли что-то к корневому порту. На ноутбуках Intel за ним всегда
// встроенный хаб, и порт "подключён" всегда -- такой ответ ничего не доказывает,
// выдёргивание за хабом видно по его порту (ehci_poll) и по отказу передачи.
int ehci_port_connected(int root_port) {
    if (!op_regs || root_port < 0 || root_port >= s_num_ports) return 0;
    return (op_regs->portsc[root_port] & 1) ? 1 : 0;
}

int ehci_storage_found(void) { return ehci_msc_alive_count() > 0; }

static void root_port_attach(int i) {
    if (ehci_reset_port(i)) enumerate_at_addr0(i, 0, 0, 0);
}

int ehci_init(void) {
    kdebug("\n[EHCI] Initializing Host Controller...\n", 0x55FFFF);
    s_up = 0;
    pci_device_t ehci_dev;
    if (!pci_find_ehci(&ehci_dev)) {
        kdebug("  [!] EHCI Controller not found!\n", 0xFF5555);
        return -1;
    }
    g_ehci_dev = ehci_dev;
    s_num_ports = 0;

    uint32_t ehci_id = pci_config_read(ehci_dev.bus, ehci_dev.slot, ehci_dev.func, 0x00);
    kdebug("  [i] EHCI VID:DID = ", 0xAAAAAA);
    print_hex(ehci_id, 0x55FF55);
    kdebug(" (low16=Vendor, high16=Device)\n", 0);

    uint32_t pci_cmd = pci_config_read(ehci_dev.bus, ehci_dev.slot, ehci_dev.func, 0x04);
    pci_config_write(ehci_dev.bus, ehci_dev.slot, ehci_dev.func, 0x04, pci_cmd | 0x06);

    uint32_t pci_cmd_readback = pci_config_read(ehci_dev.bus, ehci_dev.slot, ehci_dev.func, 0x04);
    kdebug("  [i] PCI COMMAND readback: ", 0xAAAAAA);
    print_hex(pci_cmd_readback, 0x55FF55);
    if (pci_cmd_readback & (1 << 2)) {
        kdebug(" [Bus Master: ON]\n", 0x55FF55);
    } else {
        kdebug(" [!] Bus Master: OFF! Controller cannot DMA into RAM!\n", 0xFF5555);
    }

    volatile uint8_t* cap_regs = (volatile uint8_t*)(uintptr_t)ehci_dev.bar0;
    uint8_t cap_length = cap_regs[0];
    uint32_t hcsparams = *(volatile uint32_t*)(cap_regs + 4);
    uint8_t num_ports = hcsparams & 0x0F;
    s_num_ports = num_ports;

    g_eecp = ehci_bios_handoff(&ehci_dev, cap_regs);

    op_regs = (ehci_op_regs_t*)(uintptr_t)(ehci_dev.bar0 + cap_length);

    // Сброс контроллера
    op_regs->usbcmd &= ~1;
    int reset_timeout = 100;
    while ((op_regs->usbsts & (1 << 12)) == 0 && reset_timeout > 0) { 
        sleep_ms(1);
        reset_timeout--;
    }
    
    op_regs->usbcmd |= 2;
    reset_timeout = 100;
    while ((op_regs->usbcmd & 2) && reset_timeout > 0) { 
        sleep_ms(1);
        reset_timeout--;
    }
    op_regs->ctrldssegment = 0;

    for (int i = 0; i < 1024; i++) periodic_list[i] = 1;
    usb_flush_range(periodic_list, sizeof(periodic_list));
    op_regs->periodiclistbase = V2P(periodic_list);
    op_regs->usbcmd |= (1 << 4);
    int pse_timeout = 200;
    while (!(op_regs->usbsts & (1 << 14)) && pse_timeout--) sleep_ms(1);

    op_regs->ctrldssegment = 0;

    for (int i = 0; i < 1024; i++) periodic_list[i] = 1;
    usb_flush_range((void*)periodic_list, sizeof(periodic_list));
    op_regs->periodiclistbase = V2P(periodic_list);

    // Сначала запускаем контроллер (Run/Stop = 1) и включаем периодический список
    kdebug("  [+] Starting Host Controller (Run/Stop)...\n", 0xAAAAAA);
    op_regs->usbcmd |= (1 << 0) | (1 << 4);
    (void)op_regs->usbcmd;

    op_regs->configflag = 1;
    sleep_ms(10);

    // [ИСПРАВЛЕНИЕ]: Добавляем таймаут!
    int halt_timeout = 100;
    while ((op_regs->usbsts & (1 << 12)) && halt_timeout > 0) {
        sleep_ms(1);
        halt_timeout--;
    }
    
    if (op_regs->usbsts & (1 << 12)) {
        kdebug("  [!] Controller FAILED to start (HCHalted bit is stuck)!\n", 0xFF5555);
        return -1; // Аварийно выходим из драйвера, чтобы не вешать всю ОС
    }
    kdebug("  [+] Controller is RUNNING!\n", 0x55FF55);

    kdebug("  [+] Powering on root ports...\n", 0xAAAAAA);
    for (int i = 0; i < num_ports; i++) {
        op_regs->portsc[i] = (op_regs->portsc[i] & ~0x2A) | (1 << 12);
    }
    sleep_ms(100);


    // Отметки о смене подключения снимаем до перечисления: дальше по ним
    // опрос узнаёт о выдёргивании и новой вставке.
    for (int i = 0; i < num_ports; i++) op_regs->portsc[i] = (op_regs->portsc[i] & ~0x2A) | 0x02;

    kdebug("  [+] Scanning and resetting root ports...\n", 0xAAAAAA);
    for (int i = 0; i < num_ports; i++) root_port_attach(i);
    s_up = 1;
    return 0;
}

// ------------------------------------------------------- горячее подключение
// Раз в полсекунды (из usb_poll, под usb_lock). Корневой порт сообщает о смене
// подключения сам (бит CSC), хаб -- только на запрос GET_STATUS, а это
// управляющая передача на каждый порт, поэтому хабы спрашиваем через раз.
void ehci_poll(void) {
    static int tick = 0;
    if (!s_up || !op_regs) return;
    tick++;

    for (int i = 0; i < s_num_ports; i++) {
        uint32_t sc = op_regs->portsc[i];
        if (!(sc & 0x02)) continue;                          // смены не было
        op_regs->portsc[i] = (sc & ~0x2A) | 0x02;            // снять отметку
        for (int k = 0; k < EDEV_MAX; k++)
            if (s_dev[k].used && s_dev[k].root_port == i && !s_dev[k].hub_addr) dev_remove(&s_dev[k]);
        if (sc & 1) { sleep_ms(100); root_port_attach(i); }
    }

    // Накопитель замолчал (пять секунд без ответа, ошибка транзакции), а порт
    // его по-прежнему видит. На Intel за встроенным хабом корневой порт не
    // пустеет никогда, и выдёргивание тоже выглядит так. Снимаем устройство и
    // поднимаем порт заново: если флешка на месте, она вернётся новым диском,
    // и монтирование узнает в нём прежнюю.
    // Раз в две секунды, не чаще: мёртвая флешка отвечает таймаутами.
    for (int k = 0; (tick & 3) == 0 && k < EDEV_MAX; k++) {
        ehci_dev_t* d = &s_dev[k];
        if (!d->used || d->msc < 0 || !ehci_msc_is_gone(d->msc)) continue;
        int rp = d->root_port;
        uint8_t ha = d->hub_addr, hp = d->hub_port;
        dev_remove(d);
        if (!ha) { if (ehci_port_connected(rp)) root_port_attach(rp); continue; }
        for (int h = 0; h < EDEV_MAX; h++)
            if (s_dev[h].used && s_dev[h].is_hub && s_dev[h].addr == ha) { hub_attach_port(&s_dev[h], hp, 1); break; }
    }

    if (tick & 1) return;
    for (int k = 0; k < EDEV_MAX; k++) {
        ehci_dev_t* hub = &s_dev[k];
        if (!hub->used || !hub->is_hub) continue;
        for (uint8_t p = 1; p <= hub->hub_nports; p++) {
            uint8_t get_stat[8] = { 0xA3, USB_REQ_GET_STATUS, 0x00, 0x00, p, 0x00, 0x04, 0x00 };
            if (!ehci_control_transfer(get_stat, (void*)dma_buffer, 4, 1, hub->addr, hub->mps0)) continue;
            uint16_t chg = dma_buffer[2] | (dma_buffer[3] << 8);
            if (!(chg & HUB_PORT_CHG_CONN)) continue;
            for (int c = 0; c < EDEV_MAX; c++)
                if (s_dev[c].used && s_dev[c].hub_addr == hub->addr && s_dev[c].hub_port == p) dev_remove(&s_dev[c]);
            hub_attach_port(hub, p, 1);                      // снимет отметку и поднимет новое
        }
    }
}
