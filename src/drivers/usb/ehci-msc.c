// =============================================================================
// ehci-msc.c — накопители (USB Mass Storage, Bulk-Only Transport) на EHCI.
//
// Раньше накопитель здесь был один, глобальный (g_msc), с одним признаком
// "пропал" на всех. Теперь это таблица: у каждой флешки свой адрес на шине,
// свои bulk-точки и свои переключатели DATA0/DATA1. Найденный накопитель
// регистрируется в реестре дисков (drivers/blk.c) -- дальше система читает
// его через blk_read и не знает, что под ним EHCI.
//
// BOT: CBW -> данные -> CSW, тот же, что в xhci-msc.c; транспорт -- очередь
// qTD в асинхронном расписании.
// =============================================================================

#include "ehci-msc.h"
#include "ehci.h"
#include "usb.h"
#include "../blk.h"

extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern void sleep_ms(uint32_t ms);

extern ehci_op_regs_t* op_regs;

typedef struct {
    int      used;
    int      gone;              // выдернули: на шину больше не ходим
    uint8_t  addr;
    int      root_port;
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;
    uint8_t  tog_in, tog_out;   // Data Toggle для Bulk IN и Bulk OUT
    uint32_t blocks, block_size;
    uint32_t tag;
    int      disk;              // номер в реестре дисков
    char     vendor[9], product[17];
} msc_dev_t;

static msc_dev_t s_msc[EHCI_MSC_MAX];

// Выровненные DMA-дескрипторы и буферы: общие, обмен идёт по одному за раз
static volatile ehci_qh_t  qh_bulk __attribute__((aligned(64)));
static volatile ehci_qtd_t qtd_bulk __attribute__((aligned(32)));

static volatile usb_cbw_t  g_cbw __attribute__((aligned(64)));
static volatile usb_csw_t  g_csw __attribute__((aligned(64)));
static volatile uint8_t    msc_dma_buf[4096] __attribute__((aligned(4096)));

// Перевод виртуального адреса Higher-Half в физический для DMA
#define V2P(va) (((uintptr_t)(va) >= 0xFFFFFFFF80000000ULL) ? \
    (uint32_t)((uintptr_t)(va) - 0xFFFFFFFF80000000ULL) : \
    ((uintptr_t)(va) >= 0xFFFF800000000000ULL) ? \
    (uint32_t)((uintptr_t)(va) - 0xFFFF800000000000ULL) : \
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

// Флешку выдернули: на шину больше не ходим, все операции отказывают сразу.
// Без этого каждая команда SCSI -- три передачи по пять секунд ожидания, и
// система, читающая десятки секторов подряд, выглядела намертво зависшей.
static int ehci_bulk_transfer(msc_dev_t* m, uint8_t ep, void* buffer, int len, int is_in) {
    if (m->gone) return 0;
    // Без поднятого контроллера сюда заходить нельзя. На ноутбуке, где BIOS
    // переключён в режим "только xHCI", EHCI на шине нет вовсе: op_regs остаётся
    // нулевым, а ожидание ниже читало единицы и висело вечно -- загрузка
    // замирала на строке "Mounting Root File System..." без единого слова.
    if (!op_regs || !m->addr) return 0;

    if (op_regs->usbcmd & (1 << 5)) {
        op_regs->usbcmd &= ~(1 << 5);
        for (int w = 0; w < 100000; w++) {          // не вечно: железо может молчать
            if (!(op_regs->usbsts & (1 << 15))) break;
        }
    }
    // Корневой порт флешки пуст -- её выдернули (вместе с хабом, если он был)
    if (!ehci_port_connected(m->root_port)) { m->gone = 1; return 0; }

    uint8_t ep_num = ep & 0x0F;
    uint16_t max_pkt = is_in ? m->mps_in : m->mps_out;
    if (max_pkt == 0) max_pkt = 512;

    // Берём актуальный тумблер для нужного направления
    uint8_t toggle = is_in ? m->tog_in : m->tog_out;

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
                         (2 << 12) | ((uint32_t)ep_num << 8) | (m->addr & 0x7F);
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
    for (int w = 0; w < 100000 && !(op_regs->usbsts & (1 << 15)); w++) { }

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
            // Чистый STALL (только Halted) -- это ответ живого устройства. А вот
            // ошибка транзакции, babble или ошибка буфера после всех повторов --
            // признак того, что на том конце никого нет.
            if (token & 0x38) m->gone = 1;
            // Останавливаем асинхронное расписание
            op_regs->usbcmd &= ~(1 << 5);
            for (int w = 0; w < 100000 && (op_regs->usbsts & (1 << 15)); w++) { }
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
                if (is_in) m->tog_in ^= 1;
                else       m->tog_out ^= 1;
            }

            // Останавливаем асинхронное расписание
            op_regs->usbcmd &= ~(1 << 5);
            for (int w = 0; w < 100000 && (op_regs->usbsts & (1 << 15)); w++) { }
            return 1;
        }
    }

    kdebug("  [!] Bulk Transfer Timeout!\n", 0xFF5555);
    // Пять секунд тишины -- устройство не отвечает. Дальше не ждём на каждой
    // команде, а считаем его пропавшим; сторож проверит, вернулось ли оно.
    m->gone = 1;
    op_regs->usbcmd &= ~(1 << 5);
    for (int w = 0; w < 100000 && (op_regs->usbsts & (1 << 15)); w++) { }
    return 0;
}


// BOT цикл: CBW -> Data Phase -> CSW
static int msc_exec_command(msc_dev_t* m, void* cdb, uint8_t cdb_len, void* data, uint32_t data_len, int is_in) {
    for (int i = 0; i < (int)sizeof(usb_cbw_t); i++) ((uint8_t*)&g_cbw)[i] = 0;

    g_cbw.signature = CBW_SIGNATURE;
    g_cbw.tag = ++m->tag;
    g_cbw.data_transfer_len = data_len;
    g_cbw.flags = is_in ? CBW_FLAGS_IN : CBW_FLAGS_OUT;
    g_cbw.lun = 0;
    g_cbw.cmd_len = cdb_len;
    for (int i = 0; i < cdb_len; i++) g_cbw.cmd[i] = ((uint8_t*)cdb)[i];

    msc_flush((void*)&g_cbw, sizeof(usb_cbw_t));

    // Шаг 1: CBW через Bulk OUT
    if (!ehci_bulk_transfer(m, m->ep_out, (void*)&g_cbw, sizeof(usb_cbw_t), 0)) return 0;

    // Шаг 2: Данные (если есть)
    if (data_len > 0 && data) {
        if (!ehci_bulk_transfer(m, is_in ? m->ep_in : m->ep_out, data, data_len, is_in)) return 0;
    }

    // Шаг 3: CSW через Bulk IN
    for (int i = 0; i < (int)sizeof(usb_csw_t); i++) ((uint8_t*)&g_csw)[i] = 0;
    msc_flush((void*)&g_csw, sizeof(usb_csw_t));
    if (!ehci_bulk_transfer(m, m->ep_in, (void*)&g_csw, sizeof(usb_csw_t), 1)) return 0;
    msc_flush((void*)&g_csw, sizeof(usb_csw_t));

    if (g_csw.signature != CSW_SIGNATURE || g_csw.tag != g_cbw.tag || g_csw.status != 0) {
        kdebug("  [!] CSW Status Failure!\n", 0xFF5555);
        return 0;
    }
    return 1;
}

static int scsi_inquiry(msc_dev_t* m) {
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    if (!msc_exec_command(m, cdb, 6, (void*)msc_dma_buf, 36, 1)) return 0;
    usb_inquiry_strings((const uint8_t*)msc_dma_buf, m->vendor, m->product);
    kdebug("  SCSI: [", 0x55FF55); kdebug(m->vendor, 0xFFFF55); kdebug("] [", 0x55FF55);
    kdebug(m->product, 0xFFFF55); kdebug("]\n", 0x55FF55);
    return 1;
}

// TEST UNIT READY: флешка после включения отвечает "не готова" раз-другой.
// REQUEST SENSE снимает это состояние, иначе следующая команда тоже упадёт.
static void scsi_wait_ready(msc_dev_t* m) {
    for (int t = 0; t < 5; t++) {
        uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
        if (msc_exec_command(m, tur, 6, 0, 0, 0)) return;
        if (m->gone) return;
        uint8_t rs[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
        msc_exec_command(m, rs, 6, (void*)msc_dma_buf, 18, 1);
        sleep_ms(50);
    }
}

static int scsi_read_capacity(msc_dev_t* m) {
    uint8_t cdb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!msc_exec_command(m, cdb, 10, (void*)msc_dma_buf, 8, 1)) return 0;

    uint32_t last_lba = ((uint32_t)msc_dma_buf[0] << 24) | ((uint32_t)msc_dma_buf[1] << 16) |
                        ((uint32_t)msc_dma_buf[2] << 8)  | (uint32_t)msc_dma_buf[3];
    uint32_t block_sz = ((uint32_t)msc_dma_buf[4] << 24) | ((uint32_t)msc_dma_buf[5] << 16) |
                        ((uint32_t)msc_dma_buf[6] << 8)  | (uint32_t)msc_dma_buf[7];
    m->blocks = last_lba + 1;
    m->block_size = block_sz ? block_sz : 512;

    char buf[16];
    uint32_t size_mb = (uint32_t)(((uint64_t)m->blocks * m->block_size) / (1024 * 1024));
    kdebug("  Capacity: ~", 0x55FF55); itoa(size_mb, buf); kdebug(buf, 0xFFFF55); kdebug(" MB\n", 0);
    return 1;
}

static void rw_cdb(uint8_t* cdb, uint8_t op, uint32_t lba, uint16_t cur) {
    for (int i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = op;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[7] = (cur >> 8) & 0xFF;
    cdb[8] = cur & 0xFF;
}

static msc_dev_t* msc_get(int h) {
    if (h < 0 || h >= EHCI_MSC_MAX || !s_msc[h].used || !s_msc[h].blocks) return 0;
    return &s_msc[h];
}

// Чтение порциями через физически непрерывный буфер msc_dma_buf
static int msc_read(int h, uint32_t lba, uint32_t count, void* out_buffer) {
    msc_dev_t* m = msc_get(h);
    if (!m || m->gone) return -1;
    usb_lock();
    uint8_t* dst = (uint8_t*)out_buffer;
    uint32_t per = sizeof(msc_dma_buf) / m->block_size;
    int ret = 0;
    while (count > 0) {
        uint32_t cur = (count > per) ? per : count;
        uint32_t bytes = cur * m->block_size;
        uint8_t cdb[10];
        rw_cdb(cdb, SCSI_READ10, lba, (uint16_t)cur);
        if (!msc_exec_command(m, cdb, 10, (void*)msc_dma_buf, bytes, 1)) { ret = -2; break; }
        for (uint32_t i = 0; i < bytes; i++) dst[i] = msc_dma_buf[i];
        dst += bytes;
        lba += cur;
        count -= cur;
    }
    usb_unlock();
    return ret;
}

// SCSI WRITE 10 с изоляцией DMA через тот же буфер
static int msc_write(int h, uint32_t lba, uint32_t count, const void* in_buffer) {
    msc_dev_t* m = msc_get(h);
    if (!m || m->gone) return -1;
    usb_lock();
    const uint8_t* src = (const uint8_t*)in_buffer;
    uint32_t per = sizeof(msc_dma_buf) / m->block_size;
    int ret = 0;
    while (count > 0) {
        uint32_t cur = (count > per) ? per : count;
        uint32_t bytes = cur * m->block_size;
        for (uint32_t i = 0; i < bytes; i++) msc_dma_buf[i] = src[i];
        msc_flush((void*)msc_dma_buf, bytes);
        uint8_t cdb[10];
        rw_cdb(cdb, SCSI_WRITE10, lba, (uint16_t)cur);
        if (!msc_exec_command(m, cdb, 10, (void*)msc_dma_buf, bytes, 0)) { ret = -2; break; }
        src += bytes;
        lba += cur;
        count -= cur;
    }
    usb_unlock();
    return ret;
}

static int msc_present(int h) {
    msc_dev_t* m = msc_get(h);
    return m && !m->gone && ehci_port_connected(m->root_port);
}

static const blk_ops_t s_ops = { msc_read, msc_write, msc_present };

int ehci_msc_attach(uint8_t dev_addr, int root_port, const uint8_t* cfg_desc, uint16_t cfg_len,
                    uint16_t vid, uint16_t pid) {
    int idx = -1;
    for (int i = 0; i < EHCI_MSC_MAX; i++) if (!s_msc[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    msc_dev_t* m = &s_msc[idx];
    for (int i = 0; i < (int)sizeof(*m); i++) ((uint8_t*)m)[i] = 0;
    m->addr = dev_addr;
    m->root_port = root_port;
    m->disk = -1;
    m->tag = 0x1000;

    for (uint32_t off = 0; off + 2 <= cfg_len; ) {
        uint8_t len = cfg_desc[off], type = cfg_desc[off + 1];
        if (len == 0) break;
        if (type == USB_DESC_ENDPOINT && off + 7 <= cfg_len) {
            uint8_t ep_addr = cfg_desc[off + 2];
            uint8_t ep_attr = cfg_desc[off + 3];
            uint16_t max_pkt = cfg_desc[off + 4] | (cfg_desc[off + 5] << 8);
            if ((ep_attr & 0x03) == 0x02) {
                if (ep_addr & 0x80) { m->ep_in = ep_addr & 0x0F;  m->mps_in = max_pkt; }
                else                { m->ep_out = ep_addr & 0x0F; m->mps_out = max_pkt; }
            }
        }
        off += len;
    }
    if (!m->ep_in || !m->ep_out) {
        kdebug("  [!] Bulk IN/OUT endpoints not found!\n", 0xFF5555);
        return -1;
    }

    m->used = 1;
    if (!scsi_inquiry(m)) { kdebug("  [!] SCSI INQUIRY failed!\n", 0xFF5555); m->used = 0; return -1; }
    scsi_wait_ready(m);
    if (!scsi_read_capacity(m)) { kdebug("  [!] SCSI READ CAPACITY failed!\n", 0xFF5555); m->used = 0; return -1; }

    m->disk = blk_add(&s_ops, idx, BLK_BUS_EHCI, vid, pid, m->blocks, m->block_size, m->vendor, m->product);
    return idx;
}

void ehci_msc_detach(int idx) {
    if (idx < 0 || idx >= EHCI_MSC_MAX || !s_msc[idx].used) return;
    msc_dev_t* m = &s_msc[idx];
    m->gone = 1;
    if (m->disk >= 0) blk_gone(m->disk);
    m->used = 0;
}

int ehci_msc_is_gone(int idx) {
    return idx >= 0 && idx < EHCI_MSC_MAX && s_msc[idx].used && s_msc[idx].gone;
}

int ehci_msc_alive_count(void) {
    int n = 0;
    for (int i = 0; i < EHCI_MSC_MAX; i++) if (s_msc[i].used && !s_msc[i].gone && s_msc[i].blocks) n++;
    return n;
}
