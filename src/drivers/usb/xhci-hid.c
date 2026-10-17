// =============================================================================
// xhci-hid.c — USB-клавиатуры и мыши на xHCI.
//
// Устройство приходит сюда адресованным, с разобранными загрузочными
// HID-интерфейсами (xhci.c, parse_config). Здесь: Configure Endpoint для их
// точек interrupt IN, SET_CONFIGURATION, SET_PROTOCOL(boot), SET_IDLE и
// очередь из XHID_QUEUED передач на каждую точку. Передачи помечены
// прерывателем 1: их события падают во второе кольцо, которое раз в
// миллисекунду разбирает таймер (xhci_hid_service), — отчёт уходит в hid.c,
// а на его место ставится новая передача. Кольцо событий 0 и ожидания
// накопителей об этом ничего не знают.
// =============================================================================

#include "xhci-priv.h"
#include "usb.h"
#include "hid.h"

// Все точки HID всех устройств — для разбора событий по (слот, точка)
#define XHID_ALL (XDEV_MAX * XHID_MAX)
static xhci_hid_t* s_eps[XHID_ALL];
static int s_slot_of[XHID_ALL];

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) { __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc"); }

// Интервал опроса в единицах контекста точки (2^n по 125 мкс). У Low/Full-speed
// bInterval — миллисекунды, у High-speed и SuperSpeed — уже показатель степени
static uint32_t ep_interval(int speed, uint8_t b) {
    if (speed == 1 || speed == 2) {
        uint32_t frames = b ? b : 1, n = 3;          // 1 мс = 8 микрокадров = 2^3
        while (n < 10 && (1u << (n + 1)) <= frames * 8) n++;
        return n;
    }
    uint32_t n = b ? (uint32_t)b - 1 : 0;
    return n > 15 ? 15 : n;
}

// Поставить в очередь одну передачу отчёта (буфер — по номеру TRB)
static void queue_one(xhci_hid_t* h) {
    uint32_t idx = h->idx;
    uint64_t buf = h->buf_phys + (uint64_t)(idx % 64) * 64;
    uint32_t len = h->mps > 64 ? 64 : h->mps;
    // Normal TRB: IOC (бит 5), ISP (бит 2) — о коротком отчёте сказать сразу;
    // цель прерывания 1 (биты 31:22 слова статуса)
    xhci_ring_push(h->ring, h->ring_phys, &h->idx, &h->cycle, buf, len | (1u << 22),
                   (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
}

int xhci_hid_attach(xhci_dev_t* d) {
    if (!xhci_evt2_ready()) { xhci_err("[xHCI] no second interrupter: USB keyboard/mouse unsupported\n"); return -1; }
    int dci_max = 1, n = 0;
    uint32_t add = 1;                              // A0: контекст слота
    for (int i = 0; i < d->hid_n; i++) {
        xhci_hid_t* h = &d->hid[i];
        if (!h->ep || !h->mps) continue;
        h->ring = (xhci_trb_t*)xhci_dma_alloc(1, &h->ring_phys);
        h->buf = (uint8_t*)xhci_dma_alloc(1, &h->buf_phys);
        if (!h->ring || !h->buf) return -1;
        for (int k = 0; k < TRB_RING_SIZE; k++) { h->ring[k].param = 0; h->ring[k].status = 0; h->ring[k].control = 0; }
        h->ring[TRB_RING_SIZE - 1].param = h->ring_phys;
        h->ring[TRB_RING_SIZE - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;
        h->idx = 0;
        h->cycle = 1;
        h->dci = h->ep * 2 + 1;                    // IN
        if (h->dci > dci_max) dci_max = h->dci;
        add |= 1u << h->dci;
        n++;
    }
    if (!n) return -1;

    uint8_t* in_ctx = d->in_ctx;
    for (int i = 0; i < 4096; i++) in_ctx[i] = 0;
    uint32_t* ic = xhci_ctx_at(in_ctx, 0);
    ic[1] = add;
    // Слот — копия выходного (контроллер заполнил его при адресации), меняем число точек
    uint32_t* src = xhci_ctx_at(d->dev_ctx, 0);
    uint32_t* sc = xhci_ctx_at(in_ctx, 1);
    for (int i = 0; i < 8; i++) sc[i] = src[i];
    sc[0] = (sc[0] & 0x07FFFFFFu) | ((uint32_t)dci_max << 27);
    for (int i = 0; i < d->hid_n; i++) {
        xhci_hid_t* h = &d->hid[i];
        if (!h->ring) continue;
        uint32_t* ep = xhci_ctx_at(in_ctx, 1 + h->dci);
        ep[0] = ep_interval(d->speed, h->interval) << 16;
        ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)h->mps << 16);   // CErr=3, тип 7 — Interrupt IN
        ep[2] = (uint32_t)(h->ring_phys | 1);
        ep[3] = (uint32_t)(h->ring_phys >> 32);
        ep[4] = (uint32_t)h->mps | ((uint32_t)h->mps << 16);       // средняя длина TRB, Max ESIT Payload
    }
    __asm__ volatile("sfence" ::: "memory");
    xhci_cmd_submit(d->in_ctx_phys, 0, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24));
    xhci_trb_t ev;
    if (!xhci_wait_event(TRB_EV_CMD_DONE, &ev, 500)) { xhci_fail("HID Configure Endpoint: no answer", 0); return -1; }
    if ((int)(ev.status >> 24) != 1) { xhci_fail("HID Configure Endpoint refused, code", (int)(ev.status >> 24)); return -1; }

    if (xhci_control_nodata(d, 0x00, USB_REQ_SET_CONFIG, d->cfg_value, 0) != 0) {
        xhci_fail("HID SET_CONFIGURATION failed, cc", xhci_last_cc);
        return -1;
    }
    for (int i = 0; i < d->hid_n; i++) {
        xhci_hid_t* h = &d->hid[i];
        if (!h->ring) continue;
        // Загрузочный протокол: отчёт фиксированного вида, без разбора дескриптора.
        // SET_IDLE(0): присылать отчёт только при изменениях. Ошибки не страшны —
        // часть мышей эти запросы не знает и всё равно шлёт загрузочные отчёты
        xhci_control_nodata(d, 0x21, 0x0B, 0, h->iface);
        xhci_control_nodata(d, 0x21, 0x0A, 0, h->iface);
        h->kb = h->proto == 1 ? hid_kbd_alloc() : -1;
        if (h->proto == 2) hid_mouse_count(1);
        for (int k = 0; k < XHID_QUEUED; k++) queue_one(h);
        // В таблицу таймера — с запрещёнными прерываниями: он может прийти в любой миг
        uint64_t f = irq_save();
        for (int k = 0; k < XHID_ALL; k++)
            if (!s_eps[k]) { s_eps[k] = h; s_slot_of[k] = d->slot; break; }
        h->active = 1;
        irq_restore(f);
        __asm__ volatile("sfence" ::: "memory");
        xhci_doorbell(d->slot, (uint32_t)h->dci);
    }
    return 0;
}

void xhci_hid_detach(xhci_dev_t* d) {
    for (int i = 0; i < d->hid_n; i++) {
        xhci_hid_t* h = &d->hid[i];
        uint64_t f = irq_save();
        int was = h->active;
        h->active = 0;
        for (int k = 0; k < XHID_ALL; k++) if (s_eps[k] == h) s_eps[k] = 0;
        irq_restore(f);
        if (was) {
            if (h->kb >= 0) hid_kbd_free(h->kb);
            if (h->proto == 2) hid_mouse_count(-1);
        }
        h->kb = -1;
        xhci_dma_free(h->ring, 1);
        xhci_dma_free(h->buf, 1);
        h->ring = 0;
        h->buf = 0;
    }
    d->hid_n = 0;
}

// Из прерывания таймера: события второго кольца -> отчёты -> новые передачи
void xhci_hid_service(void) {
    if (!xhci_evt2_ready()) return;
    xhci_trb_t ev;
    int got = 0;
    while (got < 64 && xhci_evt2_pop(&ev)) {
        got++;
        if ((int)((ev.control >> 10) & 0x3F) != TRB_EV_TRANSFER) continue;
        int slot = (int)(ev.control >> 24), dci = (int)((ev.control >> 16) & 0x1F);
        xhci_hid_t* h = 0;
        for (int k = 0; k < XHID_ALL; k++)
            if (s_eps[k] && s_slot_of[k] == slot && s_eps[k]->dci == dci) { h = s_eps[k]; break; }
        if (!h || !h->active) continue;
        int cc = (int)(ev.status >> 24);
        if (cc != 1 && cc != 13) {                 // ошибка или STALL: точка встала — не заводим её дальше
            h->active = 0;
            continue;
        }
        uint64_t trb = ev.param & ~0xFull;
        uint32_t idx = (uint32_t)((trb - h->ring_phys) / sizeof(xhci_trb_t));
        uint32_t want = h->mps > 64 ? 64 : h->mps;
        uint32_t left = ev.status & 0xFFFFFF;
        int len = left <= want ? (int)(want - left) : 0;
        const uint8_t* r = h->buf + (idx % 64) * 64;
        if (h->proto == 1) hid_kbd_report(h->kb, r, len);
        else hid_mouse_report(r, len);
        queue_one(h);                              // взамен принятой
        xhci_doorbell(slot, (uint32_t)h->dci);
    }
    if (got) xhci_evt2_done();
}
