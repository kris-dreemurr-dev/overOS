// =============================================================================
// xhci-msc.c — накопители (USB Mass Storage, Bulk-Only Transport) на xHCI.
//
// Контроллер, кольца и перечисление устройств -- в xhci.c. Сюда устройство
// попадает уже адресованным, с прочитанным конфигурационным дескриптором, и
// если у него есть интерфейс-накопитель (класс 8), xhci_msc_attach доводит его
// до дела: объявляет контроллеру две bulk-точки, говорит устройству
// SET_CONFIGURATION, спрашивает размер по SCSI и регистрирует диск в
// drivers/blk.c. Дальше система читает его через blk_read, не зная, что под
// ним xHCI.
//
// BOT здесь тот же, что в ehci-msc.c: CBW -> данные -> CSW. Разный только
// транспорт: у EHCI это очередь qTD, у xHCI -- кольцо TRB на каждую точку.
// =============================================================================

#include "xhci-priv.h"
#include "usb.h"
#include "../blk.h"

extern void sleep_ms(uint32_t ms);

// Общий буфер обмена: CBW/CSW в начале, данные с 64-го байта. Один на все
// накопители -- обмен всё равно идёт по одному за раз (usb_lock).
static uint8_t* s_io_buf = 0;
static uint64_t s_io_phys = 0;
#define IO_PAGES 2

// ------------------------------------------------ bulk-точки и SCSI (BOT)
// Нулевая точка умеет только управляющие запросы. Чтобы читать сектора, надо
// сказать контроллеру про две bulk-точки интерфейса (Configure Endpoint) и
// дать каждой своё кольцо.
static uint16_t bulk_mps(int psiv) {
    switch (psiv) {
        case 4: case 5: return 1024;
        case 3:         return 512;
        default:        return 64;
    }
}

static int xhci_config_endpoints(xhci_dev_t* d) {
    if (!d->ep_in || !d->ep_out) return -1;

    uint64_t ph_in = 0, ph_out = 0;
    d->in_ring  = (xhci_trb_t*)xhci_dma_alloc(1, &ph_in);
    d->out_ring = (xhci_trb_t*)xhci_dma_alloc(1, &ph_out);
    if (!d->in_ring || !d->out_ring) return -1;
    d->in_ring_phys = ph_in;  d->in_idx = 0;  d->in_cycle = 1;
    d->out_ring_phys = ph_out; d->out_idx = 0; d->out_cycle = 1;

    for (int r = 0; r < 2; r++) {
        xhci_trb_t* ring = r ? d->out_ring : d->in_ring;
        uint64_t    phys = r ? ph_out : ph_in;
        for (int i = 0; i < TRB_RING_SIZE; i++) { ring[i].param = 0; ring[i].status = 0; ring[i].control = 0; }
        ring[TRB_RING_SIZE - 1].param   = phys;
        ring[TRB_RING_SIZE - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;
    }

    // Индекс контекста точки (DCI) = номер * 2 + направление
    int dci_in  = d->ep_in  * 2 + 1;
    int dci_out = d->ep_out * 2;
    int dci_max = dci_in > dci_out ? dci_in : dci_out;

    uint8_t* in_ctx = d->in_ctx;
    for (int i = 0; i < 4096; i++) in_ctx[i] = 0;

    uint32_t* ic = xhci_ctx_at(in_ctx, 0);
    ic[1] = 1u | (1u << dci_in) | (1u << dci_out);     // слот + обе точки

    // Контекст слота копируем из выходного (контроллер его уже заполнил при
    // адресации) и правим только число точек.
    uint32_t* src = xhci_ctx_at(d->dev_ctx, 0);
    uint32_t* sc  = xhci_ctx_at(in_ctx, 1);
    for (int i = 0; i < 8; i++) sc[i] = src[i];
    sc[0] = (sc[0] & 0x07FFFFFFu) | ((uint32_t)dci_max << 27);

    uint16_t mps = bulk_mps(d->speed);
    uint32_t* epi = xhci_ctx_at(in_ctx, 1 + dci_in);
    epi[1] = (3u << 1) | (6u << 3) | ((uint32_t)mps << 16);    // CErr=3, Bulk IN
    epi[2] = (uint32_t)(ph_in | 1);
    epi[3] = (uint32_t)(ph_in >> 32);
    epi[4] = mps;

    uint32_t* epo = xhci_ctx_at(in_ctx, 1 + dci_out);
    epo[1] = (3u << 1) | (2u << 3) | ((uint32_t)mps << 16);    // CErr=3, Bulk OUT
    epo[2] = (uint32_t)(ph_out | 1);
    epo[3] = (uint32_t)(ph_out >> 32);
    epo[4] = mps;

    __asm__ volatile("sfence" ::: "memory");
    xhci_cmd_submit(d->in_ctx_phys, 0, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24));
    xhci_trb_t ev;
    if (!xhci_wait_event(TRB_EV_CMD_DONE, &ev, 500)) { xhci_err("[xHCI] Configure Endpoint: no answer\n"); return -1; }
    if ((int)(ev.status >> 24) != 1) {
        xhci_log_num("[xHCI] Configure Endpoint refused, code ", (int)(ev.status >> 24), "\n");
        return -1;
    }
    return 0;
}

// Снять с bulk-точки состояние Halted. После STALL контроллер точку
// останавливает, и без этого все следующие передачи на неё молча висят до
// таймаута -- отсюда были долгие секунды на загрузке. Порядок как в спецификации:
// Reset Endpoint (контроллеру) -> Set TR Dequeue Pointer (продолжить с текущего
// места кольца) -> CLEAR_FEATURE(ENDPOINT_HALT) (самому устройству).
static void xhci_clear_halt(xhci_dev_t* d, int in) {
    int dci = in ? (d->ep_in * 2 + 1) : (d->ep_out * 2);
    xhci_trb_t ev;

    xhci_cmd_submit(0, 0, (TRB_RESET_EP << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24));
    xhci_wait_event(TRB_EV_CMD_DONE, &ev, 200);

    uint64_t ring = in ? d->in_ring_phys : d->out_ring_phys;
    uint32_t idx  = in ? d->in_idx : d->out_idx;
    uint32_t cyc  = in ? d->in_cycle : d->out_cycle;
    uint64_t deq  = ring + (uint64_t)idx * sizeof(xhci_trb_t);
    xhci_cmd_submit(deq | (cyc ? 1 : 0), 0,
                    (TRB_SET_TR_DEQ << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24));
    xhci_wait_event(TRB_EV_CMD_DONE, &ev, 200);

    uint8_t ep_addr = in ? (uint8_t)(d->ep_in | 0x80) : d->ep_out;
    xhci_control_nodata(d, 0x02, 1, 0, ep_addr);   // CLEAR_FEATURE(ENDPOINT_HALT)
}

// Одна bulk-передача. Возвращает сколько байт реально прошло, <0 — ошибка.
static int xhci_bulk(xhci_dev_t* d, int in, uint64_t buf_phys, uint32_t len) {
    xhci_trb_t* ring = in ? d->in_ring : d->out_ring;
    uint64_t    phys = in ? d->in_ring_phys : d->out_ring_phys;
    uint32_t*   idx  = in ? &d->in_idx : &d->out_idx;
    uint32_t*   cyc  = in ? &d->in_cycle : &d->out_cycle;
    int dci = in ? (d->ep_in * 2 + 1) : (d->ep_out * 2);

    uint64_t trb_phys = phys + (uint64_t)(*idx) * sizeof(xhci_trb_t);
    // ISP + IOC: о коротком пакете хотим знать сразу
    xhci_ring_push(ring, phys, idx, cyc, buf_phys, len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    __asm__ volatile("sfence" ::: "memory");
    xhci_doorbell(d->slot, (uint32_t)dci);

    xhci_trb_t ev;
    if (!xhci_wait_transfer(d->slot, dci, trb_phys, &ev, 1000)) { xhci_last_cc = -1; return -1; }
    int code = (int)(ev.status >> 24);
    xhci_last_cc = code;
    if (code == 6) {                               // STALL: точку надо оживить
        xhci_clear_halt(d, in);
        return -6;
    }
    if (code != 1 && code != 13) return -2;
    return (int)len - (int)(ev.status & 0xFFFFFF);
}

// Bulk-Only Transport: CBW -> данные -> CSW. Ровно то же, что делает ehci-msc,
// только транспорт другой.
static int xhci_scsi(xhci_dev_t* d, const uint8_t* cdb, int cdb_len,
                     uint64_t data_phys, uint32_t data_len, int data_in,
                     uint8_t* cbw_virt, uint64_t cbw_phys) {
    uint8_t* w = cbw_virt;
    for (int i = 0; i < 31; i++) w[i] = 0;
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    d->tag++;
    w[4] = (uint8_t)d->tag; w[5] = (uint8_t)(d->tag >> 8);
    w[6] = (uint8_t)(d->tag >> 16); w[7] = (uint8_t)(d->tag >> 24);
    w[8]  = (uint8_t)data_len;        w[9]  = (uint8_t)(data_len >> 8);
    w[10] = (uint8_t)(data_len >> 16); w[11] = (uint8_t)(data_len >> 24);
    w[12] = data_in ? 0x80 : 0x00;
    w[13] = 0;                                   // LUN 0
    w[14] = (uint8_t)cdb_len;
    for (int i = 0; i < cdb_len && i < 16; i++) w[15 + i] = cdb[i];

    if (xhci_bulk(d, 0, cbw_phys, 31) != 31) return -1;

    // Длину проверяем строго: короткая передача -- это НЕ успех. Сектор, который
    // приехал наполовину, выглядит как обычные данные, и дальше ломается уже
    // что-то совсем другое (у нас так молча умирал рабочий стол).
    int data_ok = 1;
    if (data_len) {
        int got = xhci_bulk(d, data_in, data_phys, data_len);
        if (got == -1) return -2;                  // точка молчит -- дальше бессмысленно
        if (got != (int)data_len) data_ok = 0;     // STALL/короткий: CSW всё равно читаем (так велит BOT)
    }

    // CSW кладём туда же, где лежал CBW: он уже не нужен
    int csw = xhci_bulk(d, 1, cbw_phys, 13);
    if (csw == -6) csw = xhci_bulk(d, 1, cbw_phys, 13);   // после STALL -- вторая попытка
    if (csw != 13) return -3;
    if (!(w[0] == 'U' && w[1] == 'S' && w[2] == 'B' && w[3] == 'S')) return -4;
    if (w[12] != 0) return -5;                   // поле состояния CSW: команда не выполнена
    return data_ok ? 0 : -2;
}

// Разговор с накопителем: дождаться готовности и спросить размер
static int xhci_msc_probe(xhci_dev_t* d, uint8_t* scratch, uint64_t scratch_phys) {
    uint8_t  cdb[16];
    uint8_t* data = scratch + 64;                // CBW/CSW в начале страницы
    uint64_t data_phys = scratch_phys + 64;

    // INQUIRY: заодно будит устройство. Если на него нет даже CSW -- транспорт
    // мёртв, и десять повторов TEST UNIT READY по секунде таймаута каждый
    // только растянут загрузку (так и было: казалось, что система зависла).
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x12; cdb[4] = 36;
    int rc = xhci_scsi(d, cdb, 6, data_phys, 36, 1, scratch, scratch_phys);
    if (rc == -1 || rc == -3 || rc == -4) return rc;
    if (rc == 0) usb_inquiry_strings(data, d->vendor, d->product);   // для уведомления "подключено ..."

    // TEST UNIT READY: флешка после включения отвечает "не готова" раз-другой
    for (int t = 0; t < 10; t++) {
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x00;
        rc = xhci_scsi(d, cdb, 6, 0, 0, 0, scratch, scratch_phys);
        if (rc == 0) break;
        if (rc != -5) return rc;                   // не "не готова", а сломанный транспорт
        // REQUEST SENSE снимает состояние ошибки, иначе следующая команда тоже упадёт
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x03; cdb[4] = 18;
        xhci_scsi(d, cdb, 6, data_phys, 18, 1, scratch, scratch_phys);
        sleep_ms(50);
    }

    // READ CAPACITY(10): последний LBA и размер сектора, оба big-endian
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x25;
    rc = xhci_scsi(d, cdb, 10, data_phys, 8, 1, scratch, scratch_phys);
    if (rc != 0) return rc;

    uint32_t last = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                    ((uint32_t)data[2] << 8)  | data[3];
    uint32_t bs   = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                    ((uint32_t)data[6] << 8)  | data[7];
    if (!bs) bs = 512;
    d->blocks = last + 1;
    d->block_size = bs;
    return 0;
}


// ------------------------------------------------------ чтение и запись
static xhci_dev_t* msc_dev(int h) {
    if (h < 0 || h >= XDEV_MAX) return 0;
    xhci_dev_t* d = &xhci_devs[h];
    if (!d->used || d->cls != 8 || !d->blocks) return 0;
    return d;
}

static int io_buf(void) {
    // Буфер под обмен берём один раз на всю жизнь системы: файловая система
    // читает секторами постоянно, и выпрашивать у PMM по две страницы на каждое
    // чтение -- и дорого, и лишний повод для отказа в неудачный момент.
    if (!s_io_buf) s_io_buf = (uint8_t*)xhci_dma_alloc(IO_PAGES, &s_io_phys);
    return s_io_buf ? 0 : -1;
}

// Первые несколько отказов печатаем всегда: без них на чужом железе видно
// только "программа не запустилась".
static void complain(const char* what, uint32_t lba, uint32_t n, int rc) {
    static int complained = 0;
    if (complained >= 3) return;
    complained++;
    xhci_err_hex(what, lba, "");
    xhci_err_hex(", sectors ", n, "");
    xhci_err_hex(", code ", (uint64_t)(uint32_t)(-rc), "\n");
}

static void rw_cdb(uint8_t* cdb, uint8_t op, uint32_t lba, uint32_t n) {
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = op;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(n >> 8);    cdb[8] = (uint8_t)n;
}

static int msc_read(int h, uint32_t lba, uint32_t count, void* out) {
    xhci_dev_t* d = msc_dev(h);
    if (!d) return -1;
    usb_lock();
    if (!xhci_dev_present(d)) { xhci_dev_gone(d); usb_unlock(); return -3; }
    if (io_buf() != 0) { usb_unlock(); return -1; }

    uint8_t  cdb[16];
    uint8_t* dst = (uint8_t*)out;
    uint32_t per = (IO_PAGES * 4096 - 64) / d->block_size;     // сколько секторов за раз
    if (!per) per = 1;
    int ret = 0;
    while (count) {
        uint32_t n = count > per ? per : count;
        rw_cdb(cdb, SCSI_READ10, lba, n);
        int rc = xhci_scsi(d, cdb, 10, s_io_phys + 64, n * d->block_size, 1, s_io_buf, s_io_phys);
        if (rc != 0 && !xhci_dev_present(d)) { xhci_dev_gone(d); ret = -3; break; }   // выдернули на ходу
        if (rc != 0) { complain("[xHCI] read failed at LBA ", lba, n, rc); ret = -2; break; }
        for (uint32_t i = 0; i < n * d->block_size; i++) dst[i] = s_io_buf[64 + i];
        dst += n * d->block_size;
        lba += n;
        count -= n;
    }
    usb_unlock();
    return ret;
}

// Запись: то же, что чтение, только WRITE(10) и данные в сторону устройства
static int msc_write(int h, uint32_t lba, uint32_t count, const void* in) {
    xhci_dev_t* d = msc_dev(h);
    if (!d) return -1;
    usb_lock();
    if (!xhci_dev_present(d)) { xhci_dev_gone(d); usb_unlock(); return -3; }
    if (io_buf() != 0) { usb_unlock(); return -1; }

    uint8_t  cdb[16];
    const uint8_t* src = (const uint8_t*)in;
    uint32_t per = (IO_PAGES * 4096 - 64) / d->block_size;
    if (!per) per = 1;
    int ret = 0;
    while (count) {
        uint32_t n = count > per ? per : count;
        uint32_t bytes = n * d->block_size;
        for (uint32_t i = 0; i < bytes; i++) s_io_buf[64 + i] = src[i];
        rw_cdb(cdb, SCSI_WRITE10, lba, n);
        int rc = xhci_scsi(d, cdb, 10, s_io_phys + 64, bytes, 0, s_io_buf, s_io_phys);
        if (rc != 0 && !xhci_dev_present(d)) { xhci_dev_gone(d); ret = -3; break; }
        if (rc != 0) { complain("[xHCI] write failed at LBA ", lba, n, rc); ret = -2; break; }
        src += bytes;
        lba += n;
        count -= n;
    }
    usb_unlock();
    return ret;
}

// На месте ли: одно чтение регистра порта, шину не трогает
static int msc_present(int h) {
    xhci_dev_t* d = msc_dev(h);
    return d && xhci_dev_present(d);
}

static const blk_ops_t s_ops = { msc_read, msc_write, msc_present };

// ------------------------------------------------------ подключение
int xhci_msc_attach(xhci_dev_t* d, uint8_t* buf, uint64_t buf_phys) {
    if (!d->ep_in || !d->ep_out) { xhci_fail("storage without bulk endpoints", 0); return -1; }
    if (xhci_config_endpoints(d) != 0) { xhci_fail("Configure Endpoint failed", 0); return -1; }
    // Устройству тоже надо сказать "работай в конфигурации N". Контроллеру
    // мы точки объявили, а флешка без SET_CONFIGURATION остаётся в состоянии
    // Addressed, и её bulk-точек просто не существует. QEMU это прощал,
    // настоящая флешка -- нет: "storage did not answer SCSI".
    if (xhci_control_nodata(d, 0x00, USB_REQ_SET_CONFIG, d->cfg_value ? d->cfg_value : 1, 0) != 0) {
        xhci_fail("SET_CONFIGURATION failed, cc", xhci_last_cc);
        return -1;
    }
    int prc = xhci_msc_probe(d, buf, buf_phys);
    if (prc != 0) {
        // код вида rc*100 + cc: и шаг BOT, и что ответил контроллер
        xhci_fail("storage did not answer SCSI, rc/cc", (-prc) * 100 + (xhci_last_cc < 0 ? 99 : xhci_last_cc));
        return -1;
    }
    int h = (int)(d - xhci_devs);
    d->disk = blk_add(&s_ops, h, BLK_BUS_XHCI, d->vid, d->pid, d->blocks, d->block_size, d->vendor, d->product);
    uint64_t mb = ((uint64_t)d->blocks * d->block_size) / (1024 * 1024);
    xhci_log_num("[xHCI] storage ready, size ", (int)mb, " MiB\n");
    return 0;
}
