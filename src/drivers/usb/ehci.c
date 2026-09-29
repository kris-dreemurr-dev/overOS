#include "ehci.h"
#include "ehci-msc.h"
#include "../../fs/fs.h"

extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern void sleep_ms(uint32_t ms);
extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void hex_str(uint32_t val, char* out);

// Перевод виртуального адреса ядра (Higher-Half) в физический адрес для DMA железа
#define V2P(va) (((uintptr_t)(va) >= 0xFFFFFFFF80000000ULL) ? \
    (uint32_t)((uintptr_t)(va) - 0xFFFFFFFF80000000ULL) : \
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

static void print_hex16(uint16_t val, uint32_t color) {
    char hex_str[7] = {'0', 'x'};
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 3; i >= 0; i--) {
        hex_str[2 + i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    hex_str[6] = '\0';
    kdebug(hex_str, color);
}

static void usb_flush_range(volatile void* addr, uint32_t size) {
    if (!addr || size == 0) return;
    __asm__ __volatile__("mfence" ::: "memory");
}

// Экспортируем op_regs для работы ehci-msc.c
ehci_op_regs_t* op_regs = 0;
static int active_port = -1;
static pci_device_t g_ehci_dev;
static uint8_t g_eecp = 0;

static volatile ehci_qh_t qh_control __attribute__((aligned(64)));
static volatile ehci_qtd_t qtd_setup __attribute__((aligned(32)));
static volatile ehci_qtd_t qtd_data  __attribute__((aligned(32)));
static volatile ehci_qtd_t qtd_status __attribute__((aligned(32)));
static volatile uint8_t shared_setup_pkt[8] __attribute__((aligned(64)));
static volatile uint8_t dma_buffer[4096] __attribute__((aligned(4096)));
static volatile uint32_t periodic_list[1024] __attribute__((aligned(4096)));

// Стандартные и хабовые константы USB
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09

#define HUB_FEAT_PORT_RESET    4
#define HUB_FEAT_PORT_POWER    8
#define HUB_FEAT_C_PORT_RESET  20

#define HUB_PORT_STAT_CONN     (1 << 0)
#define HUB_PORT_STAT_ENABLE   (1 << 1)
#define HUB_PORT_STAT_RESET    (1 << 4)
#define HUB_PORT_STAT_HIGH_SPD (1 << 10)

static int pci_find_ehci(pci_device_t* out_dev) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read(bus, slot, func, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_rev = pci_config_read(bus, slot, func, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                uint8_t subclass   = (class_rev >> 16) & 0xFF;
                uint8_t prog_if    = (class_rev >> 8) & 0xFF;

                if (base_class == 0x0C && subclass == 0x03 && prog_if == 0x20) {
                    out_dev->bus = bus;
                    out_dev->slot = slot;
                    out_dev->func = func;
                    out_dev->bar0 = pci_config_read(bus, slot, func, 0x10) & ~0xF;
                    return 1;
                }
            }
        }
    }
    return 0;
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

// Запрос и парсинг UTF-16LE строкового дескриптора USB в читаемую ASCII-строку
static int ehci_get_string(uint8_t dev_addr, uint8_t str_idx, char* out_str, int max_len) {
    if (str_idx == 0 || !out_str || max_len <= 0) return 0;
    out_str[0] = '\0';

    // LangID = 0x0409 (US English)
    uint8_t setup[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, str_idx, 0x03, 0x09, 0x04, 255, 0x00 };
    if (!ehci_control_transfer(setup, (void*)dma_buffer, 255, 1, dev_addr, 64)) {
        return 0;
    }

    uint8_t len = dma_buffer[0];
    if (len < 2 || dma_buffer[1] != 0x03) return 0;

    int out_pos = 0;
    for (int i = 2; i < len && out_pos < max_len - 1; i += 2) {
        char c = (char)dma_buffer[i];
        out_str[out_pos++] = (c >= 32 && c <= 126) ? c : '?';
    }
    out_str[out_pos] = '\0';
    return 1;
}

// Вычитка дескрипторов, текстовых имен и инициализация флешки
static void ehci_dump_flash_drive(uint8_t dev_addr, uint8_t max_packet0) {
    uint8_t setup_get_dev[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
    if (!ehci_control_transfer(setup_get_dev, (void*)dma_buffer, 18, 1, dev_addr, max_packet0)) {
        kdebug("  [!] Failed to get Flash Drive Device Descriptor.\n", 0xFF5555);
        return;
    }

    uint16_t bcdUSB     = dma_buffer[2] | (dma_buffer[3] << 8);
    uint8_t  devClass   = dma_buffer[4];
    uint16_t idVendor   = dma_buffer[8]  | (dma_buffer[9] << 8);
    uint16_t idProduct  = dma_buffer[10] | (dma_buffer[11] << 8);
    uint16_t bcdDevice  = dma_buffer[12] | (dma_buffer[13] << 8);
    uint8_t  iMfg       = dma_buffer[14];
    uint8_t  iProd      = dma_buffer[15];
    uint8_t  iSerial    = dma_buffer[16];
    uint8_t  numConfigs = dma_buffer[17];

    kdebug("\n========================================\n", 0x55FFFF);
    kdebug(">>>  TARGET USB DEVICE IDENTIFIED!   <<<\n", 0x55FF55);
    kdebug("========================================\n", 0x55FFFF);
    kdebug("  Vendor ID    = ", 0xAAAAAA); print_hex16(idVendor, 0xFFFF55); kdebug("\n", 0);
    kdebug("  Product ID   = ", 0xAAAAAA); print_hex16(idProduct, 0xFFFF55); kdebug("\n", 0);

    // Вычитываем реальные текстовые строки из устройства
    char str_buf[64];
    if (ehci_get_string(dev_addr, iMfg, str_buf, sizeof(str_buf))) {
        kdebug("  Manufacturer = \"", 0x55FF55); kdebug(str_buf, 0xFFFF55); kdebug("\"\n", 0x55FF55);
    }
    if (ehci_get_string(dev_addr, iProd, str_buf, sizeof(str_buf))) {
        kdebug("  Product Name = \"", 0x55FF55); kdebug(str_buf, 0xFFFF55); kdebug("\"\n", 0x55FF55);
    }
    if (ehci_get_string(dev_addr, iSerial, str_buf, sizeof(str_buf))) {
        kdebug("  Serial Num   = \"", 0x55FF55); kdebug(str_buf, 0xFFFF55); kdebug("\"\n", 0x55FF55);
    }

    kdebug("  USB Spec     = ", 0xAAAAAA);
    print_num((bcdUSB >> 8) & 0xFF, 0xFFFF55); kdebug(".", 0xFFFFFF); print_num(bcdUSB & 0xFF, 0xFFFF55);
    if (bcdUSB >= 0x0300)      kdebug(" (USB 3.x Flash Drive via USB2 link)\n", 0x55FF55);
    else if (bcdUSB >= 0x0200) kdebug(" (USB 2.0 High-Speed Flash Drive)\n", 0x55FF55);
    else                       kdebug(" (USB 1.x Legacy)\n", 0xFFAA00);

    kdebug("  Link Speed   = High-Speed (480 Mbit/s)\n", 0x55FF55);
    kdebug("  Device Class = ", 0xAAAAAA); print_hex(devClass, 0xFFFF55); kdebug("\n", 0);
    kdebug("  bcdDevice    = ", 0xAAAAAA); print_hex16(bcdDevice, 0xFFFF55); kdebug("\n", 0);
    kdebug("  #Configs     = ", 0xAAAAAA); print_num(numConfigs, 0xFFFF55); kdebug("\n", 0);

    // Читаем Configuration Descriptor Header (9 байт)
    uint8_t setup_get_cfg9[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00 };
    if (!ehci_control_transfer(setup_get_cfg9, (void*)dma_buffer, 9, 1, dev_addr, max_packet0)) {
        kdebug("  [!] Failed to read Config Descriptor header.\n", 0xFF5555);
        kdebug("========================================\n", 0x55FFFF);
        return;
    }

    uint16_t total_len = dma_buffer[2] | (dma_buffer[3] << 8);
    if (total_len > sizeof(dma_buffer)) total_len = sizeof(dma_buffer);

    uint8_t setup_get_cfg_full[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x02, 0x00, 0x00,
                                       (uint8_t)(total_len & 0xFF), (uint8_t)(total_len >> 8) };
    if (!ehci_control_transfer(setup_get_cfg_full, (void*)dma_buffer, total_len, 1, dev_addr, max_packet0)) {
        kdebug("  [!] Failed to read full Config Descriptor.\n", 0xFF5555);
        kdebug("========================================\n", 0x55FFFF);
        return;
    }

    // Проверяем интерфейс устройства
    uint8_t is_mass_storage = 0;
    uint32_t off = 0;
    while (off + 2 <= total_len) {
        uint8_t dlen  = dma_buffer[off];
        uint8_t dtype = dma_buffer[off + 1];
        if (dlen == 0) break;
        if (dtype == 4 && off + 9 <= total_len) {
            uint8_t ifClass = dma_buffer[off + 5];
            uint8_t ifSub   = dma_buffer[off + 6];
            uint8_t ifProto = dma_buffer[off + 7];

            kdebug("  Interface    = Class ", 0xAAAAAA); print_hex(ifClass, 0xFFFF55);
            kdebug(" SubClass ", 0xAAAAAA); print_hex(ifSub, 0xFFFF55);
            kdebug(" Proto ", 0xAAAAAA); print_hex(ifProto, 0xFFFF55);
            kdebug(" (", 0xAAAAAA);
            if (ifClass == 0x08) {
                kdebug("Mass Storage - Flash Drive Verified!", 0x55FF55);
                is_mass_storage = 1;
            } else if (ifClass == 0x03) {
                kdebug("HID (Keyboard/Mouse)", 0x55FF55);
            } else if (ifClass == 0x0E) {
                kdebug("UVC Camera", 0x55FF55);
            } else {
                kdebug("Other", 0xFFAA00);
            }
            kdebug(")\n", 0xAAAAAA);
            break;
        }
        off += dlen;
    }
    kdebug("========================================\n", 0x55FFFF);

    // Если это флешка — конфигурируем и отдаем управление EHCI MSC-драйверу
    if (is_mass_storage) {
        uint8_t set_cfg[8] = { 0x00, USB_REQ_SET_CONFIG, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
        if (!ehci_control_transfer(set_cfg, 0, 0, 0, dev_addr, max_packet0)) {
            kdebug("  [!] Failed to set device configuration!\n", 0xFF5555);
            return;
        }
        sleep_ms(20);

        if (ehci_msc_init_device(dev_addr, (const uint8_t*)dma_buffer, total_len)) {
            static uint8_t sector_buf[512] __attribute__((aligned(64)));
            kdebug("\n[MSC] Reading Sector 0 (MBR/Boot Sector)...\n", 0x55FFFF);

            if (ehci_msc_read_sectors(0, 1, sector_buf)) {
                uint16_t boot_sig = sector_buf[510] | (sector_buf[511] << 8);
                kdebug("  [+] Read Sector 0 SUCCESS!\n", 0x55FF55);
                kdebug("      Boot Signature: ", 0x55FF55); print_hex16(boot_sig, 0xFFFF55);

                if (boot_sig == 0xAA55) {
                    kdebug(" (VALID 0x55AA - Ready to mount FAT!)\n", 0x55FF55);
                    if (fs_mount(0)) {
                    fs_dir();
                }
                } else {
                    kdebug(" (No 0x55AA signature)\n", 0xFFAA00);
                }
            } else {
                kdebug("  [!] Failed to read Sector 0!\n", 0xFF5555);
            }
        }
    }
}

// Опрос хаба и поиск подключенных к нему устройств
static void ehci_poll_hub(uint8_t hub_addr, uint8_t max_packet0) {
    kdebug("\n[HUB] Configuring Intel RMH (Addr ", 0x55FFFF);
    print_num(hub_addr, 0x55FFFF);
    kdebug(")...\n", 0x55FFFF);

    // 1. SET_CONFIGURATION(1)
    uint8_t set_cfg[8] = { 0x00, USB_REQ_SET_CONFIG, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (!ehci_control_transfer(set_cfg, 0, 0, 0, hub_addr, max_packet0)) {
        kdebug("  [!] Failed to set Hub configuration.\n", 0xFF5555);
        return;
    }
    sleep_ms(20);

    // 2. GET_HUB_DESCRIPTOR (Descriptor type 0x29)
    uint8_t get_hub_desc[8] = { 0xA0, USB_REQ_GET_DESCRIPTOR, 0x00, 0x29, 0x00, 0x00, 0x08, 0x00 };
    if (!ehci_control_transfer(get_hub_desc, (void*)dma_buffer, 8, 1, hub_addr, max_packet0)) {
        kdebug("  [!] Failed to get Hub Descriptor.\n", 0xFF5555);
        return;
    }

    uint8_t num_ports = dma_buffer[2];
    uint32_t pwr_good_delay = (uint32_t)dma_buffer[5] * 2;
    if (pwr_good_delay < 100) pwr_good_delay = 100;

    kdebug("  [+] Hub has ", 0x55FF55); print_num(num_ports, 0xFFFF55);
    kdebug(" downstream ports. Power-good delay: ", 0xAAAAAA); print_num(pwr_good_delay, 0xFFFF55);
    kdebug(" ms\n", 0);

    // 3. Подаем питание на все порты
    kdebug("  [+] Powering on Hub downstream ports...\n", 0xAAAAAA);
    for (uint8_t p = 1; p <= num_ports; p++) {
        uint8_t pwr_pkt[8] = { 0x23, USB_REQ_SET_FEATURE, HUB_FEAT_PORT_POWER, 0x00, p, 0x00, 0x00, 0x00 };
        ehci_control_transfer(pwr_pkt, 0, 0, 0, hub_addr, max_packet0);
    }
    sleep_ms(pwr_good_delay);

    // 4. Сканируем downstream-порты хаба
    int found_flash = 0;
    for (uint8_t p = 1; p <= num_ports; p++) {
        uint8_t get_stat[8] = { 0xA3, USB_REQ_GET_STATUS, 0x00, 0x00, p, 0x00, 0x04, 0x00 };
        if (!ehci_control_transfer(get_stat, (void*)dma_buffer, 4, 1, hub_addr, max_packet0)) {
            continue;
        }

        uint16_t port_status = dma_buffer[0] | (dma_buffer[1] << 8);

        if (port_status & HUB_PORT_STAT_CONN) {
            kdebug("  [+] Device detected on Hub Port ", 0x55FF55);
            print_num(p, 0xFFFF55);
            kdebug("! Resetting port...\n", 0);

            // Сброс порта (PORT_RESET)
            uint8_t reset_pkt[8] = { 0x23, USB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, 0x00, p, 0x00, 0x00, 0x00 };
            ehci_control_transfer(reset_pkt, 0, 0, 0, hub_addr, max_packet0);
            sleep_ms(60);

            // Очистка флага C_PORT_RESET
            uint8_t clr_reset[8] = { 0x23, USB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_RESET, 0x00, p, 0x00, 0x00, 0x00 };
            ehci_control_transfer(clr_reset, 0, 0, 0, hub_addr, max_packet0);
            sleep_ms(20);

            // Читаем статус после сброса
            ehci_control_transfer(get_stat, (void*)dma_buffer, 4, 1, hub_addr, max_packet0);
            port_status = dma_buffer[0] | (dma_buffer[1] << 8);

            if (!(port_status & HUB_PORT_STAT_ENABLE)) {
                kdebug("  [-] Port not enabled after reset.\n", 0xFFAA00);
                continue;
            }

            if (!(port_status & HUB_PORT_STAT_HIGH_SPD)) {
                kdebug("  [!] Low/Full-Speed device on port. Skipped (requires EHCI Split-Transactions).\n", 0xFFAA00);
                continue;
            }

            // USB recovery time
            sleep_ms(100);

            // 5. Устройство проснулось на адресе 0. Читаем первые 8 байт для max_packet0
            uint8_t dev_max_pkt0 = 64;
            uint8_t setup_get_desc8[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00 };
            if (ehci_control_transfer(setup_get_desc8, (void*)dma_buffer, 8, 1, 0, 64)) {
                if (dma_buffer[7] == 8 || dma_buffer[7] == 16 || dma_buffer[7] == 32 || dma_buffer[7] == 64) {
                    dev_max_pkt0 = dma_buffer[7];
                }
            }

            // 6. Назначаем устройству постоянный адрес 2
            uint8_t setup_set_addr2[8] = { 0x00, USB_REQ_SET_ADDRESS, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (!ehci_control_transfer(setup_set_addr2, 0, 0, 0, 0, dev_max_pkt0)) {
                kdebug("  [!] Failed to set Address 2 on downstream device.\n", 0xFF5555);
                continue;
            }
            sleep_ms(15);

            // 7. Считываем полную информацию о флешке
            ehci_dump_flash_drive(2, dev_max_pkt0);
            found_flash = 1;
            break;
        }
    }

    if (!found_flash) {
        kdebug("  [-] No High-Speed flash drive found on any downstream port of this hub.\n", 0xFFFF55);
    }
}

// Энумерация корневого устройства (на порту EHCI сидит Hub)
static void ehci_enumerate_device(uint8_t max_packet0) {
    uint8_t setup_set_addr[8] = { 0x00, USB_REQ_SET_ADDRESS, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (!ehci_control_transfer(setup_set_addr, 0, 0, 0, 0, max_packet0)) {
        kdebug("  [!] SET_ADDRESS failed.\n", 0xFF5555);
        return;
    }
    sleep_ms(10);

    uint8_t setup_get_dev[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
    if (!ehci_control_transfer(setup_get_dev, (void*)dma_buffer, 18, 1, 1, max_packet0)) {
        kdebug("  [!] Failed to get full Device Descriptor.\n", 0xFF5555);
        return;
    }

    uint16_t bcdUSB     = dma_buffer[2] | (dma_buffer[3] << 8);
    uint8_t  devClass   = dma_buffer[4];
    uint16_t idVendor   = dma_buffer[8]  | (dma_buffer[9] << 8);
    uint16_t idProduct  = dma_buffer[10] | (dma_buffer[11] << 8);
    uint16_t bcdDevice  = dma_buffer[12] | (dma_buffer[13] << 8);
    uint8_t  numConfigs = dma_buffer[17];

    kdebug("\n=== Root USB Device Info ===\n", 0x55FFFF);
    kdebug("  Vendor ID    = ", 0xAAAAAA); print_hex16(idVendor, 0xFFFF55); kdebug("\n", 0);
    kdebug("  Product ID   = ", 0xAAAAAA); print_hex16(idProduct, 0xFFFF55); kdebug("\n", 0);
    kdebug("  USB Spec     = ", 0xAAAAAA);
    print_num((bcdUSB >> 8) & 0xFF, 0xFFFF55); kdebug(".", 0xFFFFFF); print_num(bcdUSB & 0xFF, 0xFFFF55);
    kdebug(" (USB 2.0)\n", 0x55FF55);
    kdebug("  Link Speed   = High-Speed (480 Mbit/s)\n", 0x55FF55);
    kdebug("  Device Class = ", 0xAAAAAA); print_hex(devClass, 0xFFFF55); kdebug("\n", 0);
    kdebug("  bcdDevice    = ", 0xAAAAAA); print_hex16(bcdDevice, 0xFFFF55); kdebug("\n", 0);
    kdebug("  #Configs     = ", 0xAAAAAA); print_num(numConfigs, 0xFFFF55); kdebug("\n", 0);

    uint8_t setup_get_cfg9[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00 };
    if (!ehci_control_transfer(setup_get_cfg9, (void*)dma_buffer, 9, 1, 1, max_packet0)) {
        kdebug("  [!] Failed to get Configuration Descriptor header.\n", 0xFF5555);
        return;
    }
    uint16_t total_len = dma_buffer[2] | (dma_buffer[3] << 8);
    if (total_len > sizeof(dma_buffer)) total_len = sizeof(dma_buffer);

    uint8_t setup_get_cfg_full[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x02, 0x00, 0x00,
                                       (uint8_t)(total_len & 0xFF), (uint8_t)(total_len >> 8) };
    if (!ehci_control_transfer(setup_get_cfg_full, (void*)dma_buffer, total_len, 1, 1, max_packet0)) {
        kdebug("  [!] Failed to get full Configuration Descriptor.\n", 0xFF5555);
        return;
    }

    uint8_t ifClass = 0;
    uint32_t off = 0;
    while (off + 2 <= total_len) {
        uint8_t dlen  = dma_buffer[off];
        uint8_t dtype = dma_buffer[off + 1];
        if (dlen == 0) break;
        if (dtype == 4 && off + 9 <= total_len) {
            ifClass = dma_buffer[off + 5];
            uint8_t ifSub   = dma_buffer[off + 6];
            uint8_t ifProto = dma_buffer[off + 7];
            kdebug("  Interface    = Class ", 0xAAAAAA); print_hex(ifClass, 0xFFFF55);
            kdebug(" SubClass ", 0xAAAAAA); print_hex(ifSub, 0xFFFF55);
            kdebug(" Proto ", 0xAAAAAA); print_hex(ifProto, 0xFFFF55);
            kdebug(" (", 0xAAAAAA);
            if (ifClass == 0x09) kdebug("Hub", 0x55FF55);
            else if (ifClass == 0x08) kdebug("Mass Storage", 0x55FF55);
            else kdebug("Other", 0xFFAA00);
            kdebug(")\n", 0xAAAAAA);
            break;
        }
        off += dlen;
    }
    kdebug("============================\n", 0x55FFFF);

    if (devClass == 0x09 || ifClass == 0x09) {
        ehci_poll_hub(1, max_packet0);
    } else if (ifClass == 0x08) {
        // Прямое подключение флешки к корневому порту (как в QEMU)
        ehci_dump_flash_drive(1, max_packet0);
    }
}

void ehci_init(void) {
    kdebug("\n[EHCI] Initializing Host Controller...\n", 0x55FFFF);

    pci_device_t ehci_dev;
    if (!pci_find_ehci(&ehci_dev)) {
        kdebug("  [!] EHCI Controller not found!\n", 0xFF5555);
        return;
    }
    g_ehci_dev = ehci_dev;

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

    volatile uint8_t* cap_regs = (volatile uint8_t*)ehci_dev.bar0;
    uint8_t cap_length = cap_regs[0];
    uint32_t hcsparams = *(volatile uint32_t*)(cap_regs + 4);
    uint8_t num_ports = hcsparams & 0x0F;

    g_eecp = ehci_bios_handoff(&ehci_dev, cap_regs);

    op_regs = (ehci_op_regs_t*)(ehci_dev.bar0 + cap_length);

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
        return; // Аварийно выходим из драйвера, чтобы не вешать всю ОС
    }
    kdebug("  [+] Controller is RUNNING!\n", 0x55FF55);

    kdebug("  [+] Powering on root ports...\n", 0xAAAAAA);
    for (int i = 0; i < num_ports; i++) {
        op_regs->portsc[i] = (op_regs->portsc[i] & ~0x2A) | (1 << 12);
    }
    sleep_ms(100);

    kdebug("  [+] Scanning and resetting root ports...\n", 0xAAAAAA);
    for (int i = 0; i < num_ports; i++) {
        if (ehci_reset_port(i)) {
            active_port = i;
            break;
        }
    }

    if (active_port == -1) {
        kdebug("  [-] No High-Speed USB device detected on root ports.\n", 0xFFFF55);
        return;
    }

    kdebug("  -> Requesting Root Device Descriptor (8 bytes)...\n", 0xAAAAAA);
    uint8_t setup_get_desc[8] = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00 };

    if (ehci_control_transfer(setup_get_desc, (void*)dma_buffer, 8, 1, 0, 64)) {
        kdebug("  [+] SUCCESS! Descriptor Received via EHCI!\n", 0x55FF55);
        uint8_t max_packet0 = dma_buffer[7];
        if (max_packet0 != 8 && max_packet0 != 16 && max_packet0 != 32 && max_packet0 != 64) {
            max_packet0 = 64;
        }
        ehci_enumerate_device(max_packet0);
    } else {
        kdebug("  [!] Failed to get root descriptor via EHCI.\n", 0xFF5555);
    }
}

static void ehci_print_hex32(uint32_t val, uint32_t color) {
    char buf[16];
    hex_str(val, buf);
    kputs(buf, color);
}

static void ehci_print_num(int val, uint32_t color) {
    char buf[16];
    itoa(val, buf);
    kputs(buf, color);
}

static void ehci_flush_range(volatile void* addr, uint32_t size) {
    if (!addr || size == 0) return;
    uintptr_t p = (uintptr_t)addr & ~63;
    uintptr_t end = (uintptr_t)addr + size;
    for (; p < end; p += 64) __asm__ __volatile__("clflush (%0)" :: "r"(p) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
}

void cmd_ehci_log(void) {
    kputs("\n================ [ DEEP EHCI DIAGNOSTICS ] ================\n", 0x00FFFF);

    pci_device_t dev;
    if (!pci_find_usb_controller(&dev) || dev.prog_if != 0x20) {
        kputs("[-] EHCI Controller NOT FOUND on PCI bus!\n", 0xFF5555);
        return;
    }

    uint32_t mmio_base = dev.bar0 & ~0x0FUL;
    if (!mmio_base) {
        kputs("[-] Invalid MMIO Base (BAR0 is NULL)!\n", 0xFF5555);
        return;
    }

    uint32_t pci_cmd_log = pci_config_read(dev.bus, dev.slot, dev.func, 0x04);
    kputs("[0] PCI COMMAND: ", 0xFFFF55); ehci_print_hex32(pci_cmd_log, 0x55FF55);
    if (pci_cmd_log & (1 << 1)) kputs(" MemSpace=ON", 0x55FF55); else kputs(" MemSpace=OFF!", 0xFF5555);
    if (pci_cmd_log & (1 << 2)) kputs(" BusMaster=ON\n", 0x55FF55); else kputs(" BusMaster=OFF!\n", 0xFF5555);

    uint8_t caplength = *(volatile uint8_t*)mmio_base;
    volatile uint32_t* cap_regs = (volatile uint32_t*)mmio_base;
    volatile uint32_t* op_reg_log  = (volatile uint32_t*)(mmio_base + caplength);

    uint32_t hccparams = cap_regs[2];
    kputs("[1] Chip Capabilities (HCCPARAMS):\n", 0xFFFF55);
    kputs("    Raw: ", 0xAAAAAA); ehci_print_hex32(hccparams, 0x55FF55);
    if (hccparams & 1) {
        kputs(" -> 64-bit Addressing Capable (Uses CTRDSSEGMENT)\n", 0x55FF55);
        uint32_t ctrlds = op_reg_log[4];
        kputs("    CTRLDSSEGMENT: ", 0xAAAAAA); ehci_print_hex32(ctrlds, ctrlds == 0 ? 0x55FF55 : 0xFF5555);
        if (ctrlds != 0) kputs(" [!] Non-zero 4GB Segment Base!\n", 0xFF5555);
        else kputs(" (OK, lower 4GB selected)\n", 0x55FF55);
    } else {
        kputs(" -> 32-bit Only (CTRLDSSEGMENT is ignored by HW)\n", 0xAAAAAA);
    }
    uint32_t eecp = (hccparams >> 8) & 0xFF;
    kputs("    EECP (BIOS Extended Cap): ", 0xAAAAAA); ehci_print_hex32(eecp, 0xAAAAAA); kputc('\n', 0xFFFFFF);

    uint32_t usbcmd = op_reg_log[0];
    uint32_t usbsts = op_reg_log[1];
    uint32_t frindex_start = op_reg_log[3] & 0x3FFF;
    sleep_ms(2);
    uint32_t frindex_end = op_reg_log[3] & 0x3FFF;

    kputs("\n[2] Engine & Microframe Clock:\n", 0xFFFF55);
    kputs("    USBCMD: ", 0xAAAAAA); ehci_print_hex32(usbcmd, 0x55FF55);
    kputs(" (RS=", 0xAAAAAA); ehci_print_num(usbcmd & 1, 0xFFFF55);
    kputs(" ASE=", 0xAAAAAA); ehci_print_num((usbcmd >> 5) & 1, 0xFFFF55);
    kputs(")\n", 0xAAAAAA);

    kputs("    USBSTS: ", 0xAAAAAA); ehci_print_hex32(usbsts, 0x55FF55);
    if (usbsts & (1 << 4))  kputs(" [!] HSE: PCIe Host System Bus Error!\n", 0xFF5555);
    if (usbsts & (1 << 12)) kputs(" [!] HCHalted: Controller core is STOPPED!\n", 0xFF5555);
    if (usbsts & (1 << 15)) kputs(" [+] ASS: Async Schedule ACTIVE\n", 0x55FF55);
    else                    kputs(" [-] ASS: Async Schedule INACTIVE\n", 0xFF5555);

    kputs("    FRINDEX Tick: ", 0xAAAAAA);
    if (frindex_start != frindex_end) {
        kputs("RUNNING (", 0x55FF55); ehci_print_num(frindex_start, 0x55FF55);
        kputs(" -> ", 0xAAAAAA); ehci_print_num(frindex_end, 0x55FF55); kputs(")\n", 0x55FF55);
    } else {
        kputs("FROZEN! Microframe counter is dead!\n", 0xFF5555);
    }

    uint32_t asynclist = op_reg_log[6];
    kputs("\n[3] Active Async Queue Head (QH):\n", 0xFFFF55);
    kputs("    ASYNCLISTADDR: ", 0xAAAAAA); ehci_print_hex32(asynclist, 0x55FF55);
    kputc('\n', 0xFFFFFF);

    if (asynclist == 0 || (asynclist & 0x1F) != 0) {
        kputs("    [!] ASYNCLISTADDR is NULL or misaligned!\n", 0xFF5555);
        return;
    }

    volatile uint32_t* qh = (volatile uint32_t*)(asynclist & ~0x1FUL);
    ehci_flush_range((void*)qh, 64);

    uint32_t hlink   = qh[0];
    uint32_t ep_char = qh[1];
    uint32_t cur_qtd = qh[3];
    uint32_t nxt_qtd = qh[4];
    uint32_t token   = qh[6];

    uint32_t eps = (ep_char >> 12) & 3;
    uint32_t max_packet = (ep_char >> 16) & 0x7FF;

    kputs("    QH HorizLink:   ", 0xAAAAAA); ehci_print_hex32(hlink, 0x55FF55);
    if (hlink & 1) kputs(" [!] Terminate=1! List stops here!\n", 0xFF5555);
    else kputs(" (OK)\n", 0x55FF55);

    kputs("    QH EpSpeed:     ", 0xAAAAAA); ehci_print_num(eps, 0xFFFF55);
    if (eps == 2)      kputs(" (High-Speed - OK)\n", 0x55FF55);
    else if (eps == 0) kputs(" (Full-Speed - WRONG for root HS port!)\n", 0xFF5555);
    else               kputs(" (Low-Speed)\n", 0xFF5555);

    kputs("    QH MaxPktLen:   ", 0xAAAAAA); ehci_print_num(max_packet, 0xFFFF55); kputc('\n', 0xFFFFFF);
    kputs("    QH CurQTD:      ", 0xAAAAAA); ehci_print_hex32(cur_qtd, 0x55FF55); kputc('\n', 0xFFFFFF);
    kputs("    QH NextQTD:     ", 0xAAAAAA); ehci_print_hex32(nxt_qtd, 0x55FF55);
    if (nxt_qtd & 1) kputs(" (Terminate=1 - No qTDs linked!)\n", 0xFF5555);
    else kputs(" (Linked)\n", 0x55FF55);

    kputs("    QH OverlayToken:", 0xAAAAAA); ehci_print_hex32(token, 0x55FF55);
    kputs(" [Active=", 0xAAAAAA); ehci_print_num((token >> 7) & 1, 0xFFFF55);
    kputs(" Halted=", 0xAAAAAA); ehci_print_num((token >> 6) & 1, ((token >> 6) & 1) ? 0xFF5555 : 0x55FF55);
    kputs(" CERR=", 0xAAAAAA);   ehci_print_num((token >> 10) & 3, 0xFFFF55);
    kputs("]\n", 0xAAAAAA);

    kputs("\n================ [ DIAGNOSTIC VERDICT ] ================\n", 0x00FFFF);
    if (!(pci_cmd_log & (1 << 2))) {
        kputs("[-] ROOT CAUSE: PCI Bus Master is OFF! HC can't DMA into RAM.\n", 0xFF5555);
    } else if ((hccparams & 1) && op_reg_log[4] != 0) {
        kputs("[-] ROOT CAUSE: Chip is 64-bit capable, but CTRLDSSEGMENT is non-zero!\n", 0xFF5555);
    } else if (frindex_start == frindex_end) {
        kputs("[-] ROOT CAUSE: Controller execution engine is FROZEN.\n", 0xFF5555);
    } else if (nxt_qtd & 1) {
        kputs("[-] ROOT CAUSE: QH NextQTD has Terminate=1.\n", 0xFF5555);
    } else if (eps != 2) {
        kputs("[-] ROOT CAUSE: QH EPS != High-Speed (2).\n", 0xFF5555);
    } else {
        kputs("[+] EHCI Controller is fully operational.\n", 0x55FF55);
    }
    kputs("========================================================\n", 0x00FFFF);
}
