#include "ehci-msc.h"
#include "ehci.h"

extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern void sleep_ms(uint32_t ms);
extern void hex_str(uint32_t val, char* out);

extern ehci_op_regs_t* op_regs;

static ehci_msc_device_t g_msc;
static uint32_t g_cbw_tag = 0x1000;

// Аппаратные состояния тумблеров Data Toggle для Bulk IN и Bulk OUT
static uint8_t g_toggle_in  = 0;
static uint8_t g_toggle_out = 0;

// Выровненные DMA-дескрипторы и буферы
static volatile ehci_qh_t  qh_bulk __attribute__((aligned(64)));
static volatile ehci_qtd_t qtd_bulk __attribute__((aligned(32)));

static volatile usb_cbw_t  g_cbw __attribute__((aligned(64)));
static volatile usb_csw_t  g_csw __attribute__((aligned(64)));
static volatile uint8_t    msc_dma_buf[4096] __attribute__((aligned(4096)));

// Перевод виртуального адреса Higher-Half в физический для DMA
#define V2P(va) (((uintptr_t)(va) >= 0xFFFFFFFF80000000ULL) ? \
    (uint32_t)((uintptr_t)(va) - 0xFFFFFFFF80000000ULL) : \
    (uint32_t)(uintptr_t)(va))

// Настоящий сброс кэш-линий процессора в ОЗУ (чтобы железо увидело данные)
static void msc_flush(volatile void* addr, uint32_t size) {
    if (!addr || size == 0) return;
    uintptr_t p = (uintptr_t)addr & ~63;
    uintptr_t end = (uintptr_t)addr + size;
    for (; p < end; p += 64) {
        __asm__ __volatile__("clflush (%0)" :: "r"(p) : "memory");
    }
    __asm__ __volatile__("mfence" ::: "memory");
}

static int ehci_bulk_transfer(uint8_t ep, void* buffer, int len, int is_in) {
    if (op_regs->usbcmd & (1 << 5)) {
        op_regs->usbcmd &= ~(1 << 5);
        while (op_regs->usbsts & (1 << 15)) { }
    }

    uint8_t ep_num = ep & 0x0F;
    uint16_t max_pkt = is_in ? g_msc.max_packet_in : g_msc.max_packet_out;
    if (max_pkt == 0) max_pkt = 512;

    // Берём актуальный тумблер для нужного направления
    uint8_t toggle = is_in ? g_toggle_in : g_toggle_out;

    // 1. Формируем qTD
    qtd_bulk.next_qtd = 1;
    qtd_bulk.alt_next_qtd = 1;
    uint32_t pid = is_in ? EHCI_PID_IN : EHCI_PID_OUT;

    qtd_bulk.token = (1 << 7) | (pid << 8) | (3 << 10) | ((uint32_t)(toggle & 1) << 31) | (len << 16);
    
    qtd_bulk.buffer[0] = V2P(buffer);
    for (int i = 1; i < 5; i++) {
        qtd_bulk.buffer[i] = V2P(((uintptr_t)buffer + i * 4096) & ~0xFFF);
    }
    for (int i = 0; i < 5; i++) qtd_bulk.buffer_hi[i] = 0;

    // 2. Формируем QH: H=1 (Head), DTC=1 (взять тоггл из qTD), EPS=2 (High-Speed)
    qh_bulk.horizontal_link = V2P(&qh_bulk) | 0x02;
    qh_bulk.ep_caps[0] = (4 << 28) | ((uint32_t)max_pkt << 16) | (1 << 15) | (1 << 14) |
                         (2 << 12) | ((uint32_t)ep_num << 8) | (g_msc.dev_addr & 0x7F);
    qh_bulk.ep_caps[1] = (1 << 30);
    qh_bulk.current_qtd = 0;
    qh_bulk.next_qtd = V2P(&qtd_bulk);
    qh_bulk.alt_next_qtd = 1;
    qh_bulk.token = 0;
    for (int i = 0; i < 5; i++) {
        qh_bulk.buffer[i] = 0;
        qh_bulk.buffer_hi[i] = 0;
    }

    msc_flush(buffer, len);
    msc_flush((void*)&qtd_bulk, sizeof(ehci_qtd_t));
    msc_flush((void*)&qh_bulk, sizeof(ehci_qh_t));

    // 3. Запуск планировщика
    op_regs->asynclistaddr = V2P(&qh_bulk);
    op_regs->usbcmd |= (1 << 5);
    while (!(op_regs->usbsts & (1 << 15))) { }

    // 4. Ожидание завершения (увеличено для записи флеш-памяти)
    int timeout_ms = 5000;
    while (timeout_ms > 0) {
        sleep_ms(1);
        timeout_ms--;

        msc_flush((void*)&qtd_bulk, sizeof(ehci_qtd_t));
        uint32_t token = qtd_bulk.token;

        // Бит 6 = Halted, биты 5..1 = ошибки шины (Buffer Err, Babble, XactErr)
        if (token & 0x7E) {
            kdebug("  [!] Bulk Transfer Error/Halted (Device disconnected or STALL)!\n", 0xFF5555);
            // Останавливаем асинхронное расписание
            op_regs->usbcmd &= ~(1 << 5);
            while (op_regs->usbsts & (1 << 15)) { }
            return 0;
        }

        // Бит 7: Active (0 = контроллер завершил работу с дескриптором)
        if (!(token & (1 << 7))) {
            if (is_in && len > 0) {
                msc_flush(buffer, len);
            }

            // Переключаем Data Toggle в зависимости от числа переданных пакетов
            uint32_t packets = (len + max_pkt - 1) / max_pkt;
            if (packets == 0) packets = 1;
            if (packets & 1) {
                if (is_in) g_toggle_in ^= 1;
                else       g_toggle_out ^= 1;
            }

            // Останавливаем асинхронное расписание
            op_regs->usbcmd &= ~(1 << 5);
            while (op_regs->usbsts & (1 << 15)) { }
            return 1;
        }
    }

    kdebug("  [!] Bulk Transfer Timeout!\n", 0xFF5555);
    op_regs->usbcmd &= ~(1 << 5);
    while (op_regs->usbsts & (1 << 15)) { }
    return 0;
}

// BOT цикл: CBW -> Data Phase -> CSW
static int msc_exec_command(void* cdb, uint8_t cdb_len, void* data, uint32_t data_len, int is_in) {
    for (int i = 0; i < (int)sizeof(usb_cbw_t); i++) ((uint8_t*)&g_cbw)[i] = 0;

    g_cbw.signature = CBW_SIGNATURE;
    g_cbw.tag = ++g_cbw_tag;
    g_cbw.data_transfer_len = data_len;
    g_cbw.flags = is_in ? CBW_FLAGS_IN : CBW_FLAGS_OUT;
    g_cbw.lun = 0;
    g_cbw.cmd_len = cdb_len;
    for (int i = 0; i < cdb_len; i++) g_cbw.cmd[i] = ((uint8_t*)cdb)[i];

    msc_flush((void*)&g_cbw, sizeof(usb_cbw_t));
    
    // Шаг 1: CBW через Bulk OUT
    if (!ehci_bulk_transfer(g_msc.ep_out, (void*)&g_cbw, sizeof(usb_cbw_t), 0)) {
        return 0;
    }

    // Шаг 2: Данные (если есть)
    if (data_len > 0 && data) {
        if (!ehci_bulk_transfer(is_in ? g_msc.ep_in : g_msc.ep_out, data, data_len, is_in)) {
            return 0;
        }
    }

    // Шаг 3: CSW через Bulk IN
    for (int i = 0; i < (int)sizeof(usb_csw_t); i++) ((uint8_t*)&g_csw)[i] = 0;
    msc_flush((void*)&g_csw, sizeof(usb_csw_t));
    
    if (!ehci_bulk_transfer(g_msc.ep_in, (void*)&g_csw, sizeof(usb_csw_t), 1)) {
        return 0;
    }

    msc_flush((void*)&g_csw, sizeof(usb_csw_t));

    if (g_csw.signature != CSW_SIGNATURE || g_csw.tag != g_cbw.tag || g_csw.status != 0) {
        kdebug("  [!] CSW Status Failure!\n", 0xFF5555);
        return 0;
    }

    return 1;
}

static int scsi_inquiry(void) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (!msc_exec_command(cdb, 6, (void*)msc_dma_buf, 36, 1)) return 0;

    char vendor[9], product[17], rev[5];
    for (int i = 0; i < 8; i++) vendor[i] = msc_dma_buf[8 + i];
    vendor[8] = '\0';
    for (int i = 0; i < 16; i++) product[i] = msc_dma_buf[16 + i];
    product[16] = '\0';
    for (int i = 0; i < 4; i++) rev[i] = msc_dma_buf[32 + i];
    rev[4] = '\0';

    kdebug("  SCSI Vendor:  [", 0x55FF55); kdebug(vendor, 0xFFFF55); kdebug("]\n", 0x55FF55);
    kdebug("  SCSI Product: [", 0x55FF55); kdebug(product, 0xFFFF55); kdebug("]\n", 0x55FF55);
    kdebug("  SCSI Rev:     [", 0x55FF55); kdebug(rev, 0xFFFF55); kdebug("]\n", 0x55FF55);
    return 1;
}

static int scsi_read_capacity(void) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!msc_exec_command(cdb, 10, (void*)msc_dma_buf, 8, 1)) return 0;

    uint32_t last_lba = ((uint32_t)msc_dma_buf[0] << 24) | ((uint32_t)msc_dma_buf[1] << 16) | 
                        ((uint32_t)msc_dma_buf[2] << 8)  | (uint32_t)msc_dma_buf[3];
    uint32_t block_sz = ((uint32_t)msc_dma_buf[4] << 24) | ((uint32_t)msc_dma_buf[5] << 16) | 
                        ((uint32_t)msc_dma_buf[6] << 8)  | (uint32_t)msc_dma_buf[7];

    g_msc.total_blocks = last_lba + 1;
    g_msc.block_size = block_sz;

    uint32_t size_mb = (uint32_t)(((uint64_t)g_msc.total_blocks * block_sz) / (1024 * 1024));

    char buf[16];
    kdebug("  Sector Size:  ", 0x55FF55); itoa(block_sz, buf); kdebug(buf, 0xFFFF55); kdebug(" bytes\n", 0);
    kdebug("  Total Blocks: ", 0x55FF55); itoa(g_msc.total_blocks, buf); kdebug(buf, 0xFFFF55); kdebug("\n", 0);
    kdebug("  Capacity:     ~", 0x55FF55); itoa(size_mb, buf); kdebug(buf, 0xFFFF55); kdebug(" MB\n", 0);
    return 1;
}

int ehci_msc_read_sectors(uint32_t lba, uint16_t count, void* out_buffer) {
    if (count == 0 || !out_buffer) return 0;

    uint32_t sector_size = g_msc.block_size ? g_msc.block_size : 512;
    uint8_t* dst = (uint8_t*)out_buffer;

    // Читаем безопасно порциями через физически непрерывный буфер msc_dma_buf
    uint16_t sectors_per_batch = sizeof(msc_dma_buf) / sector_size;

    while (count > 0) {
        uint16_t cur = (count > sectors_per_batch) ? sectors_per_batch : count;
        uint32_t bytes = cur * sector_size;

        uint8_t cdb[10] = {0};
        cdb[0] = 0x28; // READ (10)
        cdb[2] = (lba >> 24) & 0xFF;
        cdb[3] = (lba >> 16) & 0xFF;
        cdb[4] = (lba >> 8) & 0xFF;
        cdb[5] = lba & 0xFF;
        cdb[7] = (cur >> 8) & 0xFF;
        cdb[8] = cur & 0xFF;

        if (!msc_exec_command(cdb, 10, (void*)msc_dma_buf, bytes, 1)) {
            return 0;
        }

        for (uint32_t i = 0; i < bytes; i++) {
            dst[i] = msc_dma_buf[i];
        }

        dst += bytes;
        lba += cur;
        count -= cur;
    }

    return 1;
}

// SCSI WRITE 10 (0x2A) с гарантированной изоляцией DMA через буфер
int ehci_msc_write_sectors(uint32_t lba, uint16_t count, const void* in_buffer) {
    if (count == 0 || !in_buffer) return 0;

    uint32_t sector_size = g_msc.block_size ? g_msc.block_size : 512;
    const uint8_t* src = (const uint8_t*)in_buffer;

    uint16_t sectors_per_batch = sizeof(msc_dma_buf) / sector_size;

    while (count > 0) {
        uint16_t cur = (count > sectors_per_batch) ? sectors_per_batch : count;
        uint32_t bytes = cur * sector_size;

        // Копируем данные в наш DMA буфер перед отправкой
        for (uint32_t i = 0; i < bytes; i++) {
            msc_dma_buf[i] = src[i];
        }
        msc_flush((void*)msc_dma_buf, bytes);

        uint8_t cdb[10] = {0};
        cdb[0] = 0x2A; // WRITE (10)
        cdb[2] = (lba >> 24) & 0xFF;
        cdb[3] = (lba >> 16) & 0xFF;
        cdb[4] = (lba >> 8) & 0xFF;
        cdb[5] = lba & 0xFF;
        cdb[7] = (cur >> 8) & 0xFF;
        cdb[8] = cur & 0xFF;

        // flags = 0 (Data OUT)
        if (!msc_exec_command(cdb, 10, (void*)msc_dma_buf, bytes, 0)) {
            return 0;
        }

        src += bytes;
        lba += cur;
        count -= cur;
    }

    return 1;
}

int ehci_msc_init_device(uint8_t dev_addr, const uint8_t* cfg_desc, uint16_t cfg_len) {
    g_msc.dev_addr = dev_addr;
    g_msc.ep_in = 0;
    g_msc.ep_out = 0;

    g_toggle_in = 0;
    g_toggle_out = 0;

    uint32_t off = 0;
    while (off + 2 <= cfg_len) {
        uint8_t len  = cfg_desc[off];
        uint8_t type = cfg_desc[off + 1];
        if (len == 0) break;

        if (type == 0x05 && off + 7 <= cfg_len) {
            uint8_t ep_addr = cfg_desc[off + 2];
            uint8_t ep_attr = cfg_desc[off + 3];
            uint16_t max_pkt = cfg_desc[off + 4] | (cfg_desc[off + 5] << 8);

            if ((ep_attr & 0x03) == 0x02) {
                if (ep_addr & 0x80) {
                    g_msc.ep_in = ep_addr & 0x0F;
                    g_msc.max_packet_in = max_pkt;
                } else {
                    g_msc.ep_out = ep_addr & 0x0F;
                    g_msc.max_packet_out = max_pkt;
                }
            }
        }
        off += len;
    }

    if (!g_msc.ep_in || !g_msc.ep_out) {
        kdebug("  [!] Bulk IN/OUT endpoints not found!\n", 0xFF5555);
        return 0;
    }

    kdebug("\n[MSC] Bulk Endpoints Configured:\n", 0x55FFFF);
    char buf[16];
    kdebug("  Bulk IN:  EP ", 0xAAAAAA); itoa(g_msc.ep_in, buf); kdebug(buf, 0x55FF55);
    kdebug(" (MaxPacket: ", 0xAAAAAA); itoa(g_msc.max_packet_in, buf); kdebug(buf, 0x55FF55); kdebug(")\n", 0);
    kdebug("  Bulk OUT: EP ", 0xAAAAAA); itoa(g_msc.ep_out, buf); kdebug(buf, 0x55FF55);
    kdebug(" (MaxPacket: ", 0xAAAAAA); itoa(g_msc.max_packet_out, buf); kdebug(buf, 0x55FF55); kdebug(")\n", 0);

    if (!scsi_inquiry()) {
        kdebug("  [!] SCSI INQUIRY failed!\n", 0xFF5555);
        return 0;
    }

    if (!scsi_read_capacity()) {
        kdebug("  [!] SCSI READ CAPACITY failed!\n", 0xFF5555);
        return 0;
    }

    return 1;
}

ehci_msc_device_t* ehci_msc_get_device(void) {
    return &g_msc;
}