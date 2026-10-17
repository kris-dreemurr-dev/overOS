// =============================================================================
// xhci.c — контроллер USB 3.0 (eXtensible Host Controller Interface).
//
//   1. найти контроллер на шине PCI (класс 0x0C, подкласс 0x03, prog_if 0x30);
//   2. забрать его у BIOS (расширенная возможность "USB Legacy Support");
//   3. сбросить и настроить: массив контекстов устройств (DCBAA), запасные
//      страницы (scratchpad), кольцо команд и кольцо событий;
//   4. запустить, опросить порты и поднять каждое устройство: слот, адрес,
//      дескрипторы. Накопители дальше доводит xhci-msc.c;
//   5. xhci_poll -- горячее подключение: что выдернули, что воткнули.
//
// Почему именно так, а не "как в EHCI": xHCI не опрашивают регистрами на
// каждый чих. Хост и контроллер общаются КОЛЬЦАМИ дескрипторов (TRB):
// мы кладём команду в кольцо команд и звоним в дверной звонок (doorbell),
// контроллер отвечает событием в кольце событий. Поэтому почти весь код
// ниже — это аккуратная работа с памятью под DMA, а не с портами ввода-вывода.
//
// Память под кольца берём страницами PMM: они физически непрерывны и лежат
// ниже 4 ГБ (pmm_alloc_pages), а контроллеру нужен именно физический адрес.
// Процессор ходит туда через прямую карту ядра (PHYS_TO_VIRT).
//
// Выбор между xHCI и EHCI -- не наше дело, его делает usb.c. Этот драйвер про
// EHCI ничего не знает.
// =============================================================================

#include "xhci.h"
#include "xhci-priv.h"
#include "usb.h"
#include "../blk.h"
#include "../pci.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"
#include "../../kernel/config.h"
#include <stdint.h>

extern void kputs(const char* str, uint32_t color);
extern void sleep_ms(uint32_t ms);
extern void itoa(int n, char* str);


// ------------------------------------------------------------------ состояние
static uintptr_t s_mmio = 0;        // BAR0 (физический == виртуальный: низ 4 ГБ отображён 1:1)
static uintptr_t s_op   = 0;
static uintptr_t s_rt   = 0;
static uintptr_t s_db   = 0;
static int s_max_slots = 0, s_max_ports = 0;
int xhci_ctx64 = 0;             // размер контекста 64 байта вместо 32
static int s_ready = 0;
static int s_connected = 0;
static pci_device_t s_pci;

// ---- найденные устройства: слот, адрес, дескрипторы -------------------------
xhci_dev_t xhci_devs[XDEV_MAX];
static int s_dev_n = 0;                 // сколько записей таблицы когда-либо занималось
static xhci_dev_t* s_last_dev = 0;      // последнее поднятое устройство (для отчёта)
// Порт, на котором устройство не поднялось: не мучаем его на каждом опросе,
// пока его не выдернут (иначе раз в полсекунды -- новый слот и сто миллисекунд)
static uint8_t s_port_failed[256];

static uint64_t* s_dcbaa = 0;        uint64_t s_dcbaa_phys = 0;
static xhci_trb_t* s_cmd_ring = 0;   static uint64_t s_cmd_phys = 0;
static xhci_trb_t* s_evt_ring = 0;   static uint64_t s_evt_phys = 0;
static xhci_erst_t* s_erst = 0;      static uint64_t s_erst_phys = 0;
static uint32_t s_cmd_idx = 0, s_cmd_cycle = 1;
static uint32_t s_evt_idx = 0, s_evt_cycle = 1;
static xhci_trb_t* s_evt2 = 0;       static uint64_t s_evt2_phys = 0;   // прерыватель 1 (HID)
static uint32_t s_evt2_idx = 0, s_evt2_cycle = 1;

static inline uint32_t rd32(uintptr_t a)            { return *(volatile uint32_t*)a; }
static inline void     wr32(uintptr_t a, uint32_t v){ *(volatile uint32_t*)a = v; }
static inline uint64_t rd64(uintptr_t a)            { return *(volatile uint64_t*)a; }
// 64-битные регистры пишем ДВУМЯ 32-битными словами, младшее вперёд. Так это
// делают все настоящие драйверы: немало контроллеров не принимают одно
// 64-битное обращение (на этом же чипе двухбайтовое чтение версии уже вернуло
// ноль -- к размеру обращений он придирчив).
static inline void     wr64(uintptr_t a, uint64_t v){
    *(volatile uint32_t*)a = (uint32_t)v;
    *(volatile uint32_t*)(a + 4) = (uint32_t)(v >> 32);
}

// Болтливость драйвера. По умолчанию ноль: загрузочный экран оформлен как у
// systemd, и подробности контроллера в нём только мешают -- итог всё равно
// попадает в строку "Started USB Host Controllers". Ошибки печатаются всегда.
// Включается командой ядра (как loglevel в Linux).
static int s_verbose = 0;
void xhci_set_verbose(int v) { s_verbose = v; }

void xhci_log(const char* s, uint32_t c) { if (s_verbose) kputs(s, c); }
void xhci_err(const char* s) { kputs(s, 0xFF5555); }
// Шестнадцатеричное число: на чужом железе только оно и объясняет, что ответил
// чип. Печатается всегда, не глядя на болтливость.
void xhci_err_hex(const char* pre, uint64_t v, const char* post) {
    char b[19]; int n = 0;
    b[n++] = '0'; b[n++] = 'x';
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)((v >> sh) & 0xF);
        if (!d && !started && sh) continue;
        started = 1;
        b[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    b[n] = 0;
    kputs(pre, 0xAAAAAA); kputs(b, 0xFFFF55); kputs(post, 0xAAAAAA);
}
void xhci_log_num(const char* pre, int v, const char* post) {
    char b[16];
    if (!s_verbose) return;
    itoa(v, b);
    kputs(pre, 0xAAAAAA); kputs(b, 0xFFFFFF); kputs(post, 0xAAAAAA);
}

// ------------------------------------------------------- память под структуры
// Страницы PMM: физически непрерывные и ниже 4 ГБ (контроллеру нужен физадрес)
void* xhci_dma_alloc(uint32_t pages, uint64_t* out_phys) {
    void* p = pmm_alloc_pages(pages);
    if (!p) return 0;
    uint64_t phys = (uint64_t)(uintptr_t)p;
    uint8_t* virt = (uint8_t*)PHYS_TO_VIRT(phys);
    for (uint32_t i = 0; i < pages * 4096u; i++) virt[i] = 0;
    if (out_phys) *out_phys = phys;
    return virt;
}

void xhci_dma_free(void* virt, uint32_t pages) {
    if (!virt) return;
    uint64_t phys = virt_to_phys((uint64_t)(uintptr_t)virt);
    for (uint32_t i = 0; i < pages; i++) pmm_free_page((void*)(uintptr_t)(phys + (uint64_t)i * 4096));
}

// ------------------------------------------------------ забрать у прошивки
// Расширенные возможности xHCI: список по HCCPARAMS1[31:16] (в двойных словах).
// ID 1 — USB Legacy Support: пока у BIOS стоит его бит владения, трогать
// контроллер нельзя, он может перехватывать SMI на каждый наш доступ.
static void xhci_bios_handoff(uint32_t hccparams1) {
    uint32_t off = ((hccparams1 >> 16) & 0xFFFF) * 4;
    if (!off) return;
    uintptr_t p = s_mmio + off;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t cap = rd32(p);
        uint8_t id = cap & 0xFF;
        uint8_t next = (cap >> 8) & 0xFF;
        if (id == 1) {
            if (cap & (1u << 16)) {                 // владеет BIOS
                wr32(p, cap | (1u << 24));          // просим себе
                int ok = 0;
                for (int i = 0; i < 1000; i++) {
                    if (!(rd32(p) & (1u << 16))) { ok = 1; break; }
                    sleep_ms(1);
                }
                if (ok) xhci_log("[xHCI] taken over from firmware\n", 0xFFFF55);
                else    xhci_err("[xHCI] firmware did NOT release the controller\n");
            }
            // Запрещаем SMI на наши действия (регистр управления сразу за cap)
            uint32_t ctl = rd32(p + 4);
            ctl &= ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15));
            ctl |= (0xE0000000u);                   // сбрасываем висящие SMI (RW1C)
            wr32(p + 4, ctl);
            return;
        }
        if (!next) return;
        p += next * 4;
    }
}

// ------------------------------------------------------------------ сброс
static int xhci_reset(void) {
    // Остановить, если он работает
    uint32_t cmd = rd32(s_op + XOP_USBCMD);
    if (cmd & USBCMD_RS) {
        wr32(s_op + XOP_USBCMD, cmd & ~USBCMD_RS);
        for (int i = 0; i < 1000; i++) {
            if (rd32(s_op + XOP_USBSTS) & USBSTS_HCH) break;
            sleep_ms(1);
        }
    }

    wr32(s_op + XOP_USBCMD, rd32(s_op + XOP_USBCMD) | USBCMD_HCRST);
    for (int i = 0; i < 1000; i++) {
        uint32_t c = rd32(s_op + XOP_USBCMD);
        uint32_t s = rd32(s_op + XOP_USBSTS);
        if (!(c & USBCMD_HCRST) && !(s & USBSTS_CNR)) return 0;
        sleep_ms(1);
    }
    return -1;
}

// ------------------------------------------------------- кольца и контексты
static int xhci_setup_rings(void) {
    // 1. Массив контекстов устройств: по слоту на устройство + запись 0 под scratchpad
    s_dcbaa = (uint64_t*)xhci_dma_alloc(1, &s_dcbaa_phys);
    if (!s_dcbaa) return -1;

    // 2. Scratchpad: контроллер просит себе страницы под собственные нужды
    uint32_t hcs2 = rd32(s_mmio + XCAP_HCSPARAMS2);
    uint32_t spb = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (spb) {
        uint64_t arr_phys = 0;
        uint64_t* arr = (uint64_t*)xhci_dma_alloc(1, &arr_phys);
        if (!arr) return -1;
        for (uint32_t i = 0; i < spb && i < 512; i++) {
            uint64_t ph = 0;
            if (!xhci_dma_alloc(1, &ph)) return -1;
            arr[i] = ph;
        }
        s_dcbaa[0] = arr_phys;
        xhci_log_num("[xHCI] scratchpad pages: ", (int)spb, "\n");
    }
    wr64(s_op + XOP_DCBAAP, s_dcbaa_phys);

    // 3. Кольцо команд: последний TRB — Link обратно на начало
    s_cmd_ring = (xhci_trb_t*)xhci_dma_alloc(1, &s_cmd_phys);
    if (!s_cmd_ring) return -1;
    s_cmd_ring[TRB_RING_SIZE - 1].param   = s_cmd_phys;
    s_cmd_ring[TRB_RING_SIZE - 1].status  = 0;
    s_cmd_ring[TRB_RING_SIZE - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;  // Toggle Cycle
    s_cmd_idx = 0;
    s_cmd_cycle = 1;
    wr64(s_op + XOP_CRCR, s_cmd_phys | 1);        // бит 0 — Ring Cycle State

    // 4. Кольцо событий и таблица его сегментов (ERST)
    s_evt_ring = (xhci_trb_t*)xhci_dma_alloc(1, &s_evt_phys);
    s_erst     = (xhci_erst_t*)xhci_dma_alloc(1, &s_erst_phys);
    if (!s_evt_ring || !s_erst) return -1;
    s_erst[0].base = s_evt_phys;
    s_erst[0].size = TRB_RING_SIZE;
    s_evt_idx = 0;
    s_evt_cycle = 1;

    wr32(s_rt + XRT_ERSTSZ(0), 1);
    wr64(s_rt + XRT_ERDP(0),   s_evt_phys);
    wr64(s_rt + XRT_ERSTBA(0), s_erst_phys);
    wr32(s_rt + XRT_IMOD(0),   4000);              // не чаще ~1 прерывания на 1 мс

    // 5. Второе кольцо событий — для клавиатур и мышей (если прерывателей
    // больше одного; их почти всегда 8 и больше). Его читает таймер
    uint32_t max_intrs = (rd32(s_mmio + XCAP_HCSPARAMS1) >> 8) & 0x7FF;
    if (max_intrs >= 2) {
        uint64_t erst2_phys = 0;
        s_evt2 = (xhci_trb_t*)xhci_dma_alloc(1, &s_evt2_phys);
        xhci_erst_t* erst2 = (xhci_erst_t*)xhci_dma_alloc(1, &erst2_phys);
        if (s_evt2 && erst2) {
            erst2[0].base = s_evt2_phys;
            erst2[0].size = TRB_RING_SIZE;
            s_evt2_idx = 0;
            s_evt2_cycle = 1;
            wr32(s_rt + XRT_ERSTSZ(1), 1);
            wr64(s_rt + XRT_ERDP(1),   s_evt2_phys);
            wr64(s_rt + XRT_ERSTBA(1), erst2_phys);
        } else {
            s_evt2 = 0;
        }
    }
    return 0;
}

int xhci_evt2_ready(void) { return s_evt2 != 0; }

// Следующее событие второго кольца или 0. Зовётся только из таймера
int xhci_evt2_pop(xhci_trb_t* out) {
    if (!s_evt2) return 0;
    volatile xhci_trb_t* e = (volatile xhci_trb_t*)&s_evt2[s_evt2_idx];
    if ((e->control & 1u) != (uint32_t)s_evt2_cycle) return 0;
    out->param = e->param; out->status = e->status; out->control = e->control;
    s_evt2_idx++;
    if (s_evt2_idx == TRB_RING_SIZE) { s_evt2_idx = 0; s_evt2_cycle ^= 1; }
    return 1;
}

void xhci_evt2_done(void) {
    uint64_t erdp = s_evt2_phys + (uint64_t)s_evt2_idx * sizeof(xhci_trb_t);
    wr64(s_rt + XRT_ERDP(1), erdp | (1ull << 3));
}

// Положить TRB в кольцо команд и позвонить в звонок 0
void xhci_cmd_submit(uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t* t = &s_cmd_ring[s_cmd_idx];
    t->param  = param;
    t->status = status;
    t->control = control | (s_cmd_cycle ? 1u : 0u);

    s_cmd_idx++;
    if (s_cmd_idx == TRB_RING_SIZE - 1) {          // дошли до Link TRB
        s_cmd_ring[TRB_RING_SIZE - 1].control =
            (TRB_LINK << 10) | (1u << 1) | (s_cmd_cycle ? 1u : 0u);
        s_cmd_idx = 0;
        s_cmd_cycle ^= 1;
    }
    __asm__ volatile("sfence" ::: "memory");
    wr32(s_db + 0, 0);                             // звонок слота 0 = кольцо команд
}

// Дождаться события в кольце событий. Возвращает 1 и копию TRB, 0 — таймаут.
static int xhci_event_wait(xhci_trb_t* out, uint32_t ms) {
    // Время считаем по таймеру ядра (1000 Гц), а не счётом пустых циклов:
    // задержка через чтение порта 0x80 на разных машинах разная, и "100 мс"
    // на ноутбуке запросто оказываются парой миллисекунд. Счётчик оборотов
    // оставлен на случай, если нас позовут с запрещёнными прерываниями.
    extern uint32_t timer_ticks;
    uint32_t start = *(volatile uint32_t*)&timer_ticks;
    uint64_t spins = 0, spin_cap = (uint64_t)ms * 200000ull + 100000ull;

    for (;;) {
        // volatile обязателен: TRB сюда кладёт контроллер по DMA, и без этого
        // компилятор вправе прочитать поле один раз и крутиться с копией.
        volatile xhci_trb_t* e = (volatile xhci_trb_t*)&s_evt_ring[s_evt_idx];
        if ((e->control & 1u) == (uint32_t)s_evt_cycle) {
            if (out) { out->param = e->param; out->status = e->status; out->control = e->control; }
            s_evt_idx++;
            if (s_evt_idx == TRB_RING_SIZE) { s_evt_idx = 0; s_evt_cycle ^= 1; }
            uint64_t erdp = s_evt_phys + (uint64_t)s_evt_idx * sizeof(xhci_trb_t);
            wr64(s_rt + XRT_ERDP(0), erdp | (1ull << 3));   // EHB: событие обработано
            return 1;
        }
        if ((uint32_t)(*(volatile uint32_t*)&timer_ticks - start) > ms) return 0;
        if (++spins > spin_cap) return 0;
        __asm__ volatile("pause");
    }
}

// ------------------------------------------------------------------ порты
static const char* speed_name(uint32_t psiv) {
    switch (psiv) {
        case 1: return "Full-speed (12 Mbit/s)";
        case 2: return "Low-speed (1.5 Mbit/s)";
        case 3: return "High-speed (480 Mbit/s)";
        case 4: return "SuperSpeed (5 Gbit/s)";
        case 5: return "SuperSpeed+ (10 Gbit/s)";
        default: return "unknown speed";
    }
}

// Дождаться события нужного типа, пропуская остальные (например, события
// смены состояния порта прилетают в то же кольцо и ждать их не надо).
int xhci_wait_event(int type, xhci_trb_t* out, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t++) {
        xhci_trb_t ev;
        if (xhci_event_wait(&ev, 1)) {
            if ((int)((ev.control >> 10) & 0x3F) == type) { if (out) *out = ev; return 1; }
            continue;                               // чужое событие — дальше
        }
    }
    return 0;
}

// ======================= этап 2: слот, адрес, дескрипторы ====================
// Логика та же, что у любого хоста USB: контроллеру говорят "дай слот",
// заполняют контекст устройства (куда воткнуто, на какой скорости, какое
// кольцо у нулевой точки), говорят "адресуй", и дальше с устройством можно
// разговаривать стандартными запросами по нулевой точке.


// Максимальный размер пакета нулевой точки зависит от скорости (USB 3 — 512,
// High-speed — 64, Low/Full — 8: для FS реальный размер узнаём позже из
// дескриптора, 8 байт понимают все).
static uint16_t ep0_mps(int psiv) {
    switch (psiv) {
        case 4: case 5: return 512;
        case 3:         return 64;
        default:        return 8;
    }
}

// Положить TRB в кольцо. Кольцо любое: нулевой точки или bulk -- у всех
// одинаковое устройство, последний TRB это Link обратно на начало.
void xhci_ring_push(xhci_trb_t* ring, uint64_t phys, uint32_t* idx, uint32_t* cycle,
                      uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t* t = &ring[*idx];
    t->param = param;
    t->status = status;
    t->control = control | (*cycle ? 1u : 0u);

    (*idx)++;
    if (*idx == TRB_RING_SIZE - 1) {
        ring[TRB_RING_SIZE - 1].param   = phys;
        ring[TRB_RING_SIZE - 1].status  = 0;
        ring[TRB_RING_SIZE - 1].control = (TRB_LINK << 10) | (1u << 1) | (*cycle ? 1u : 0u);
        *idx = 0;
        *cycle ^= 1;
    }
}

void xhci_doorbell(int slot, uint32_t target) {
    wr32(s_db + (uintptr_t)slot * 4, target);
}

static void ep_push(xhci_dev_t* d, uint64_t param, uint32_t status, uint32_t control) {
    xhci_ring_push(d->ep0_ring, d->ep0_phys, &d->ep0_idx, &d->ep0_cycle, param, status, control);
}

// Код завершения последней передачи (для строки отчёта по порту)
int xhci_last_cc = 0;

// Дождаться события передачи ИМЕННО от нашего TRB: тот же слот, та же точка,
// тот же адрес TRB. Раньше бралось первое попавшееся событие передачи, и одно
// лишнее (например, о коротком пакете) сдвигало все дальнейшие ответы на один.
int xhci_wait_transfer(int slot, int dci, uint64_t trb_phys, xhci_trb_t* out, uint32_t ms) {
    extern uint32_t timer_ticks;
    uint32_t start = *(volatile uint32_t*)&timer_ticks;
    for (;;) {
        xhci_trb_t ev;
        if (xhci_event_wait(&ev, 1)) {
            if ((int)((ev.control >> 10) & 0x3F) != TRB_EV_TRANSFER) continue;
            if ((int)(ev.control >> 24) != slot) continue;
            if ((int)((ev.control >> 16) & 0x1F) != dci) continue;
            if ((ev.param & ~0xFull) != trb_phys) continue;   // событие от другого TRB этой точки
            if (out) *out = ev;
            return 1;
        }
        if ((uint32_t)(*(volatile uint32_t*)&timer_ticks - start) > ms) return 0;
    }
}

// Управляющий запрос по нулевой точке (IN). Возвращает число принятых байт
// или <0. Буфер — страница DMA, её даёт вызывающий.
static int xhci_control_in(xhci_dev_t* d, uint8_t rtype, uint8_t req, uint16_t value,
                           uint16_t index, uint16_t len, uint64_t buf_phys) {
    uint64_t setup = (uint64_t)rtype | ((uint64_t)req << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)len << 48);

    // Setup: сам пакет лежит прямо в TRB (бит IDT), TRT=3 — дальше данные IN
    ep_push(d, setup, 8, (TRB_SETUP << 10) | (1u << 6) | (3u << 16));
    // ISP: о коротком пакете пусть скажет сразу -- иначе остаток длины теряется
    if (len) ep_push(d, buf_phys, len, (TRB_DATA << 10) | (1u << 16) | (1u << 2));
    uint64_t st_phys = d->ep0_phys + (uint64_t)d->ep0_idx * sizeof(xhci_trb_t);
    ep_push(d, 0, 0, (TRB_STATUS << 10) | (1u << 5));                       // IOC, DIR = OUT

    __asm__ volatile("sfence" ::: "memory");
    xhci_doorbell(d->slot, 1);                   // звонок слота, точка 1 = EP0

    // Событие о коротком пакете (от TRB данных) приходит раньше события статуса;
    // ждём событие статуса, а остаток длины берём из короткого, если оно было.
    int left = 0;
    xhci_trb_t ev;
    extern uint32_t timer_ticks;
    uint32_t start = *(volatile uint32_t*)&timer_ticks;
    for (;;) {
        if (!xhci_event_wait(&ev, 1)) {
            if ((uint32_t)(*(volatile uint32_t*)&timer_ticks - start) > 500) { xhci_last_cc = -1; return -1; }
            continue;
        }
        if ((int)((ev.control >> 10) & 0x3F) != TRB_EV_TRANSFER) continue;
        if ((int)(ev.control >> 24) != d->slot || (int)((ev.control >> 16) & 0x1F) != 1) continue;
        int code = (int)(ev.status >> 24);
        xhci_last_cc = code;
        if ((ev.param & ~0xFull) == st_phys) {
            if (code != 1) return -2;
            break;
        }
        if (code == 13) { left = (int)(ev.status & 0xFFFFFF); continue; }   // короткий пакет данных
        if (code != 1) return -2;                  // STALL или ошибка на данных
    }
    return (int)len - left;
}

// Управляющий запрос без данных (SET_CONFIGURATION, CLEAR_FEATURE ...)
int xhci_control_nodata(xhci_dev_t* d, uint8_t rtype, uint8_t req, uint16_t value, uint16_t index) {
    uint64_t setup = (uint64_t)rtype | ((uint64_t)req << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32);
    ep_push(d, setup, 8, (TRB_SETUP << 10) | (1u << 6));                    // TRT = 0, данных нет
    uint64_t st_phys = d->ep0_phys + (uint64_t)d->ep0_idx * sizeof(xhci_trb_t);
    ep_push(d, 0, 0, (TRB_STATUS << 10) | (1u << 5) | (1u << 16));          // IOC, статус IN
    __asm__ volatile("sfence" ::: "memory");
    xhci_doorbell(d->slot, 1);

    xhci_trb_t ev;
    if (!xhci_wait_transfer(d->slot, 1, st_phys, &ev, 500)) { xhci_last_cc = -1; return -1; }
    xhci_last_cc = (int)(ev.status >> 24);
    return xhci_last_cc == 1 ? 0 : -2;
}

// Разобрать конфигурационный дескриптор: ищем интерфейс-накопитель (класс 8,
// протокол 0x50 — Bulk-Only) и его две bulk-точки.
static void parse_config(xhci_dev_t* d, const uint8_t* cfg, int len) {
    int i = 0, in_msc = 0;
    xhci_hid_t* in_hid = 0;                        // загрузочный HID-интерфейс, чьи точки читаем
    while (i + 2 <= len) {
        int dlen = cfg[i], dtype = cfg[i + 1];
        if (dlen < 2 || i + dlen > len) break;
        if (dtype == 2 && dlen >= 9) {             // Configuration
            d->cfg_value = cfg[i + 5];
        } else if (dtype == 4 && dlen >= 9) {      // Interface
            in_msc = (cfg[i + 5] == 8);
            if (in_msc) { d->cls = cfg[i + 5]; d->sub = cfg[i + 6]; d->proto = cfg[i + 7]; }
            // Клавиатура или мышь с загрузочным протоколом (подкласс 1): такие
            // отчёты понятны без разбора дескриптора отчёта
            in_hid = 0;
            if (cfg[i + 5] == USB_CLASS_HID && cfg[i + 6] == 1 && (cfg[i + 7] == 1 || cfg[i + 7] == 2) &&
                d->hid_n < XHID_MAX) {
                in_hid = &d->hid[d->hid_n++];
                in_hid->iface = cfg[i + 2];
                in_hid->proto = cfg[i + 7];
                in_hid->ep = 0;
            }
        } else if (dtype == 5 && dlen >= 7 && in_msc) {   // Endpoint
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            if ((attr & 3) == 2) {                 // bulk
                if (addr & 0x80) d->ep_in = addr & 0x0F;
                else             d->ep_out = addr & 0x0F;
            }
        } else if (dtype == 5 && dlen >= 7 && in_hid && !in_hid->ep) {
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            if ((attr & 3) == 3 && (addr & 0x80)) {   // interrupt IN — отсюда идут отчёты
                in_hid->ep = addr & 0x0F;
                in_hid->mps = (uint16_t)((cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FF);
                in_hid->interval = cfg[i + 6];
            }
        }
        i += dlen;
    }
}

// Есть ли устройство на своём корневом порту. Одно чтение регистра -- можно
// звать перед каждой командой: так выдернутая флешка даёт ошибку сразу, а не
// после секундных таймаутов на каждой из трёх стадий BOT.
int xhci_dev_present(xhci_dev_t* d) {
    if (d->gone || !d->used) return 0;
    if (!(rd32(s_op + XOP_PORTSC(d->port)) & PORTSC_CCS)) return 0;
    return 1;
}

// Отдать слот контроллеру и вернуть страницы устройства. Сначала Disable
// Slot: после него контроллер в эти контексты и кольца уже не смотрит, и
// страницы можно отдавать PMM без риска, что их перепишет DMA.
static void dev_release(xhci_dev_t* d) {
    xhci_hid_detach(d);                            // таймер больше не трогает её кольца
    if (d->slot > 0) {
        xhci_cmd_submit(0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)d->slot << 24));
        xhci_trb_t ev;
        xhci_wait_event(TRB_EV_CMD_DONE, &ev, 100);
        s_dcbaa[d->slot] = 0;
    }
    xhci_dma_free(d->in_ctx, 1);
    xhci_dma_free(d->dev_ctx, 1);
    xhci_dma_free(d->ep0_ring, 1);
    xhci_dma_free(d->in_ring, 1);
    xhci_dma_free(d->out_ring, 1);
    d->in_ctx = d->dev_ctx = 0;
    d->ep0_ring = d->in_ring = d->out_ring = 0;
    d->slot = 0;
    d->used = 0;
}

// Устройство пропало: реестр дисков узнаёт об этом сразу, слот возвращается
// контроллеру, и на шину к нему больше никто не ходит.
void xhci_dev_gone(xhci_dev_t* d) {
    if (d->gone || !d->used) return;
    d->gone = 1;
    if (d->disk >= 0) blk_gone(d->disk);
    d->disk = -1;
    dev_release(d);
}

// Почему последнее устройство не поднялось: шаг и код завершения. Печатается
// одной строкой на порт (см. xhci_scan_ports) -- на чужом ноутбуке это
// единственный способ узнать, на каком шаге адресации споткнулось железо.
static const char* s_fail_step = 0;
static int s_fail_code = 0;
void xhci_fail(const char* step, int code) { s_fail_step = step; s_fail_code = code; }
#define FAIL(step, code) do { xhci_fail((step), (code)); goto fail; } while (0)

// Поднять одно устройство на порту port (нумерация с нуля)
static int xhci_setup_device(int port, int psiv) {
    s_fail_step = 0; s_fail_code = 0;
    // Свободная запись: после выдёргивания флешки её место освобождается,
    // иначе пара переподключений исчерпала бы таблицу.
    int di = -1;
    xhci_dev_t* d = 0;
    uint8_t* buf = 0;
    uint64_t buf_phys = 0;
    for (int i = 0; i < XDEV_MAX; i++) if (!xhci_devs[i].used) { di = i; break; }
    if (di < 0) { xhci_fail("too many devices", 0); return -1; }
    d = &xhci_devs[di];
    for (int i = 0; i < (int)sizeof(*d); i++) ((uint8_t*)d)[i] = 0;
    d->disk = -1;

    // 1. Слот
    xhci_cmd_submit(0, 0, TRB_ENABLE_SLOT << 10);
    xhci_trb_t ev;
    if (!xhci_wait_event(TRB_EV_CMD_DONE, &ev, 300)) FAIL("Enable Slot: no answer", 0);
    if ((int)(ev.status >> 24) != 1) FAIL("Enable Slot refused, code", (int)(ev.status >> 24));
    int slot = (int)(ev.control >> 24);
    if (slot <= 0 || slot > s_max_slots) FAIL("Enable Slot: bad slot", slot);

    // 2. Память: входной контекст, контекст устройства, кольцо нулевой точки
    uint64_t in_phys = 0, dev_phys = 0, ring_phys = 0;
    uint8_t* in_ctx  = (uint8_t*)xhci_dma_alloc(1, &in_phys);
    uint8_t* dev_ctx = (uint8_t*)xhci_dma_alloc(1, &dev_phys);
    xhci_trb_t* ring = (xhci_trb_t*)xhci_dma_alloc(1, &ring_phys);
    d->slot = slot;
    d->in_ctx = in_ctx; d->dev_ctx = dev_ctx; d->ep0_ring = ring;
    if (!in_ctx || !dev_ctx || !ring) FAIL("out of memory", 0);

    for (int i = 0; i < 4096; i++) { in_ctx[i] = 0; dev_ctx[i] = 0; }
    ring[TRB_RING_SIZE - 1].param   = ring_phys;
    ring[TRB_RING_SIZE - 1].status  = 0;
    ring[TRB_RING_SIZE - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    d->slot = slot; d->port = port; d->speed = psiv;
    d->ep0_ring = ring; d->ep0_phys = ring_phys; d->ep0_idx = 0; d->ep0_cycle = 1;
    d->in_ctx = in_ctx; d->in_ctx_phys = in_phys;
    d->dev_ctx = dev_ctx; d->dev_ctx_phys = dev_phys;

    // 3. Входной контекст: добавляем слот (A0) и нулевую точку (A1)
    uint32_t* ic = xhci_ctx_at(in_ctx, 0);
    ic[1] = 0x3;

    uint32_t* sc = xhci_ctx_at(in_ctx, 1);              // контекст слота
    sc[0] = ((uint32_t)psiv << 20) | (1u << 27);   // скорость, одна точка в контексте
    sc[1] = ((uint32_t)(port + 1) << 16);          // корневой порт считается с единицы

    uint32_t* ep = xhci_ctx_at(in_ctx, 2);              // контекст EP0
    ep[1] = (3u << 1) | (4u << 3) | ((uint32_t)ep0_mps(psiv) << 16);  // CErr=3, тип Control
    ep[2] = (uint32_t)(ring_phys | 1);             // DCS = 1
    ep[3] = (uint32_t)(ring_phys >> 32);
    ep[4] = 8;                                     // средняя длина TRB

    s_dcbaa[slot] = dev_phys;

    // 4. Адресация
    __asm__ volatile("sfence" ::: "memory");
    xhci_cmd_submit(in_phys, 0, (TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24));
    if (!xhci_wait_event(TRB_EV_CMD_DONE, &ev, 500)) FAIL("Address Device: no answer", 0);
    if ((int)(ev.status >> 24) != 1) FAIL("Address Device refused, code", (int)(ev.status >> 24));

    // 5. Дескрипторы: кто это и есть ли у него интерфейс-накопитель
    buf = (uint8_t*)xhci_dma_alloc(1, &buf_phys);
    if (!buf) FAIL("out of memory", 0);

    // У Low/Full-speed размер пакета нулевой точки заранее неизвестен (8, 16,
    // 32 или 64). Мы начали с 8; если у устройства больше, оно пришлёт пакет
    // крупнее ожидаемого, и контроллер ответит Babble (cc 3) -- ровно так на
    // ноутбуке отваливался Full-speed порт 7. Поэтому сначала только первые
    // 8 байт дескриптора: в байте 7 настоящий размер, и его надо сообщить
    // контроллеру командой Evaluate Context.
    if (psiv == 1 || psiv == 2) {
        int g8 = xhci_control_in(d, 0x80, 6, 0x0100, 0, 8, buf_phys);
        if (g8 < 8) FAIL("device descriptor (8 bytes) failed, cc", xhci_last_cc);
        uint16_t mps = buf[7];
        if (mps != 8 && mps != 16 && mps != 32 && mps != 64) mps = 8;
        if (mps != 8) {
            for (int i = 0; i < 4096; i++) in_ctx[i] = 0;
            uint32_t* ic2 = xhci_ctx_at(in_ctx, 0);
            ic2[1] = 0x2;                          // меняем только EP0
            uint32_t* ep2 = xhci_ctx_at(in_ctx, 2);
            ep2[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
            ep2[2] = (uint32_t)(d->ep0_phys | 1);
            ep2[3] = (uint32_t)(d->ep0_phys >> 32);
            ep2[4] = 8;
            __asm__ volatile("sfence" ::: "memory");
            xhci_cmd_submit(in_phys, 0, (TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24));
            if (!xhci_wait_event(TRB_EV_CMD_DONE, &ev, 300)) FAIL("Evaluate Context: no answer", 0);
            if ((int)(ev.status >> 24) != 1) FAIL("Evaluate Context refused, code", (int)(ev.status >> 24));
        }
    }

    int got = xhci_control_in(d, 0x80, 6, 0x0100, 0, 18, buf_phys);   // GET_DESCRIPTOR(device)
    if (got < 8) FAIL("device descriptor failed, cc", xhci_last_cc);
    d->vid = (uint16_t)(buf[8] | (buf[9] << 8));
    d->pid = (uint16_t)(buf[10] | (buf[11] << 8));

    got = xhci_control_in(d, 0x80, 6, 0x0200, 0, 255, buf_phys);      // GET_DESCRIPTOR(config)
    if (got <= 0) FAIL("config descriptor failed, cc", xhci_last_cc);
    parse_config(d, buf, got);

    d->used = 1;
    if (di + 1 > s_dev_n) s_dev_n = di + 1;
    s_last_dev = d;


    xhci_log("[xHCI] device ", 0x55FFFF);
    { char b[16]; itoa(d->vid, b); xhci_log(b, 0xFFFFFF); xhci_log(":", 0xAAAAAA); itoa(d->pid, b); xhci_log(b, 0xFFFFFF); }
    if (d->cls == 8) xhci_log(", mass storage (bulk-only)\n", 0x55FF55);
    else if (d->hid_n) xhci_log(", keyboard/mouse (HID boot)\n", 0x55FF55);
    else             xhci_log(", not a storage device\n", 0xAAAAAA);

    // Накопитель доводит до дела xhci-msc.c: точки, SCSI, регистрация диска
    if (d->cls == USB_CLASS_STORAGE && xhci_msc_attach(d, buf, buf_phys) != 0) goto fail;
    // Клавиатура/мышь: точки interrupt IN, загрузочный протокол, очередь отчётов
    if (d->cls != USB_CLASS_STORAGE && d->hid_n && xhci_hid_attach(d) != 0) FAIL("HID setup failed, cc", xhci_last_cc);
    xhci_dma_free(buf, 1);
    return 0;

fail:
    xhci_dma_free(buf, 1);
    if (d) dev_release(d);
    return -1;
}

int xhci_device_count(void) { return s_dev_n; }

static void report_port(int port, uint32_t psiv, int enabled, int rc);
static int  xhci_port_reset(int port);

// Горячее подключение (зовёт usb_poll раз в полсекунды, под usb_lock).
//   * за портом числится устройство, а подключения нет или порт выключился
//     -- устройство пропало;
//   * подключение есть, а устройства нет -- поднимаем новое.
// Порт, на котором подъём не удался, не трогаем до следующего выдёргивания:
// иначе каждые полсекунды -- новый слот, сброс порта и сто миллисекунд ожидания.
void xhci_poll(void) {
    if (!s_ready) return;
    for (int i = 0; i < s_max_ports && i < 256; i++) {
        uintptr_t p = s_op + XOP_PORTSC(i);
        uint32_t sc = rd32(p);
        int changed = (sc & PORTSC_CSC) != 0;
        if (changed) wr32(p, portsc_neutral(sc) | PORTSC_CSC);

        xhci_dev_t* bound = 0;
        for (int k = 0; k < s_dev_n; k++)
            if (xhci_devs[k].used && !xhci_devs[k].gone && xhci_devs[k].port == i) bound = &xhci_devs[k];

        // Пропажу узнаём по самому порту: нет подключения или порт выключен
        // (USB 2 после выдёргивания и обратной вставки ждёт нового сброса).
        // Одну только отметку CSC признаком не считаем: на части контроллеров
        // она взводится и при переговорах линии USB 3, и живая флешка с корнем
        // начала бы "пропадать" каждые полсекунды.
        if (bound && (!(sc & PORTSC_CCS) || !(sc & PORTSC_PED))) {
            xhci_dev_gone(bound);
            bound = 0;
            if (s_connected > 0) s_connected--;
        }
        if (!(sc & PORTSC_CCS) || changed) s_port_failed[i] = 0;
        if (!(sc & PORTSC_CCS) || bound || s_port_failed[i]) continue;

        sleep_ms(100);                              // вставили только что: пусть устоится
        if (!(rd32(p) & PORTSC_CCS)) continue;
        int enabled = xhci_port_reset(i);
        uint32_t psiv = (rd32(p) >> 10) & 0x0F;
        int rc = enabled ? xhci_setup_device(i, psiv) : -1;
        report_port(i, psiv, enabled, rc);
        if (rc == 0) s_connected++;
        else s_port_failed[i] = 1;
    }
}

int xhci_storage_found(void) {
    for (int i = 0; i < s_dev_n; i++) if (xhci_devs[i].used && !xhci_devs[i].gone && xhci_devs[i].cls == 8) return 1;
    return 0;
}

// Сброс порта: для USB 2 это обязательный шаг, порт включается только после
// него. Порты USB 3 договариваются о связи сами, их только проверяем.
static int xhci_port_reset(int port) {
    uintptr_t p = s_op + XOP_PORTSC(port);
    uint32_t sc = rd32(p);
    if (!(sc & PORTSC_CCS)) return 0;
    if (sc & PORTSC_PED) return 1;                 // уже включён (USB 3)

    wr32(p, portsc_neutral(sc) | PORTSC_PR);
    for (int i = 0; i < 500; i++) {
        sc = rd32(p);
        if (sc & PORTSC_PRC) {
            wr32(p, portsc_neutral(sc) | PORTSC_PRC);   // подтверждаем
            return (rd32(p) & PORTSC_PED) ? 1 : 0;
        }
        sleep_ms(1);
    }
    return 0;
}

// Одна строка на подключённый порт: скорость, кто там (VID:PID) и что из этого
// вышло, либо на каком шаге и с каким кодом не вышло.
// Строки портов -- отладочный вывод: на загрузочном экране они лишние, когда
// всё работает. Видны в отладочной сборке (DEBUG_MODE, через kdebug) или после
// команды usb-debug. Отказы самого контроллера печатаются по-прежнему всегда.
extern void kdebug(const char* str, uint32_t color);
static void dbg(const char* str, uint32_t color) {
    if (s_verbose) kputs(str, color);
    else kdebug(str, color);
}

static void hex4(char* b, uint16_t v) {
    const char* h = "0123456789abcdef";
    b[0] = h[(v >> 12) & 15]; b[1] = h[(v >> 8) & 15]; b[2] = h[(v >> 4) & 15]; b[3] = h[v & 15]; b[4] = 0;
}
static void report_port(int port, uint32_t psiv, int enabled, int rc) {
    char b[16];
    dbg("[xHCI] port ", 0xAAAAAA);
    itoa(port + 1, b); dbg(b, 0xFFFFFF);
    dbg(": ", 0xAAAAAA);
    dbg(speed_name(psiv), 0xFFFFFF);
    if (!enabled) { dbg(", port did not enable\n", 0xFF5555); return; }
    if (rc != 0) {
        dbg(", ", 0xAAAAAA);
        dbg(s_fail_step ? s_fail_step : "failed", 0xFF5555);
        if (s_fail_code) { dbg(" ", 0xFF5555); itoa(s_fail_code, b); dbg(b, 0xFF5555); }
        dbg("\n", 0xFF5555);
        return;
    }
    xhci_dev_t* d = s_last_dev;
    dbg(", ", 0xAAAAAA);
    hex4(b, d->vid); dbg(b, 0xFFFFFF); dbg(":", 0xAAAAAA); hex4(b, d->pid); dbg(b, 0xFFFFFF);
    if (d->cls == 8) {
        uint64_t mb = ((uint64_t)d->blocks * d->block_size) / (1024 * 1024);
        dbg(", storage ", 0x55FF55); itoa((int)mb, b); dbg(b, 0x55FF55); dbg(" MiB\n", 0x55FF55);
    } else {
        dbg(", not storage\n", 0xAAAAAA);
    }
}

static void xhci_scan_ports(void) {
    // После настоящего сброса контроллера порты не видят устройства сразу:
    // питание надо подать, линии -- устояться, а USB 3 ещё и договориться о
    // скорости. Поэтому сначала включаем питание ВСЕМ портам, ждём, и только
    // потом смотрим, кто откликнулся -- и смотрим несколько раз.
    //
    // Именно на этом месте ноутбук показывал "13 портов, 0 подключено", хотя
    // устройства там были: мы спрашивали через доли миллисекунды после старта.
    for (int i = 0; i < s_max_ports; i++) {
        uintptr_t p = s_op + XOP_PORTSC(i);
        uint32_t sc = rd32(p);
        if (!(sc & PORTSC_PP)) wr32(p, portsc_neutral(sc) | PORTSC_PP);
    }
    sleep_ms(120);                                  // устоялось питание

    int handled[256];
    for (int i = 0; i < s_max_ports && i < 256; i++) handled[i] = 0;

    s_connected = 0;
    // Пять заходов по 200 мс: медленная флешка на USB 2 объявляется не мгновенно
    for (int pass = 0; pass < 5; pass++) {
        for (int i = 0; i < s_max_ports && i < 256; i++) {
            if (handled[i]) continue;
            uintptr_t p = s_op + XOP_PORTSC(i);
            uint32_t sc = rd32(p);

            if (sc & PORTSC_CSC) wr32(p, portsc_neutral(sc) | PORTSC_CSC);  // отметку о смене снимаем
            if (!(sc & PORTSC_CCS)) continue;

            handled[i] = 1;
            s_connected++;

            int enabled = xhci_port_reset(i);
            sc = rd32(p);
            uint32_t psiv = (sc >> 10) & 0x0F;

            // Порт включён -- значит, с устройством уже можно разговаривать
            int rc = enabled ? xhci_setup_device(i, psiv) : -1;
            report_port(i, psiv, enabled, rc);
            if (rc != 0) s_port_failed[i] = 1;
        }
        // Накопитель нашли -- дальше ждать нечего
        if (xhci_storage_found()) break;
        sleep_ms(200);
    }
    if (!s_connected) {
        // Ни одного устройства -- показываем, что именно говорят порты. Без
        // этих чисел на чужой машине остаётся только гадать: нет питания,
        // не сняты отметки о смене состояния или там правда пусто.
        // В режиме Smart Auto BIOS отдаёт порты EHCI, и пустой xHCI -- это
        // норма, а не ошибка. Кричим, только если EHCI на шине нет.
        pci_device_t eh;
        if (pci_find_usb_by_progif(0x20, &eh) && !s_verbose) return;
        xhci_err("[xHCI] no devices on any port\n");
        for (int i = 0; i < s_max_ports && i < 8; i++) {
            xhci_err_hex("[xHCI] PORTSC", i + 1, " = ");
            xhci_err_hex("", rd32(s_op + XOP_PORTSC(i)), "\n");
        }
    }
}

// ------------------------------------------------------------------ запуск
int xhci_present(void) {
    pci_device_t d;
    return pci_find_usb_by_progif(0x30, &d);
}

int xhci_port_count(void)      { return s_max_ports; }
int xhci_ports_connected(void) { return s_connected; }

// Intel PCH: какие порты USB 2 и USB 3 отдать xHCI. Пока это не сделано, на
// ноутбуке в режиме BIOS "Smart Auto" все внешние гнёзда живут на EHCI
// (за его хабом RMH), и xHCI видит пустые порты. Маски XUSB2PRM/USB3PRM
// говорят, что вообще можно переключить; Linux делает то же самое
// (usb_enable_intel_xhci_ports). Устройства на этих портах отключаются от
// EHCI и подключаются к нам -- поэтому до перечисления, а не после.
static void xhci_route_intel_ports(void) {
#if USB_ROUTE_TO_XHCI
    if (s_pci.vendor_id != 0x8086) return;
    uint32_t usb2_mask = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0xD4);   // XUSB2PRM
    uint32_t usb3_mask = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0xDC);   // USB3PRM
    if (!usb2_mask && !usb3_mask) return;
    uint32_t before = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0xD0);
    pci_config_write(s_pci.bus, s_pci.slot, s_pci.func, 0xD8, usb3_mask);           // USB3_PSSEN: SuperSpeed
    pci_config_write(s_pci.bus, s_pci.slot, s_pci.func, 0xD0, usb2_mask);           // XUSB2PR: USB 2 -> xHCI
    uint32_t after = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0xD0);
    if (after != before) {
        xhci_log("[xHCI] Intel ports switched from EHCI to xHCI\n", 0x55FFFF);
        if (s_verbose) xhci_err_hex("[xHCI] XUSB2PR = ", after, "\n");
        sleep_ms(50);                               // пусть EHCI заметит, что устройства ушли
    }
#endif
}

int xhci_init(void) {
    if (!pci_find_usb_by_progif(0x30, &s_pci)) return -1;

    // BAR0 у xHCI 64-битный и выровнен; старшую половину читаем из BAR1
    uint32_t bar0 = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0x10);
    uint64_t base = (uint64_t)(bar0 & ~0xFULL);
    if (((bar0 >> 1) & 3) == 2) {                   // 64-битный BAR
        uint64_t hi = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0x14);
        base |= hi << 32;
    }
    if (!base) { xhci_err("[xHCI] BAR0 is empty, the firmware gave the controller no registers\n"); return -2; }
    if (base >= 0x100000000ULL) {
        xhci_err("[xHCI] MMIO above 4 GiB is not supported yet\n");
        return -2;
    }

    // Память контроллера и ведущий режим шины: без Bus Master он не сможет
    // читать наши кольца
    // Память + ведущий режим шины; бит 10 запрещает старую линию прерывания:
    // обработчика у нас нет, события мы опрашиваем сами, а необслуженная линия
    // на настоящей машине встанет колом.
    uint32_t cmd = pci_config_read(s_pci.bus, s_pci.slot, s_pci.func, 0x04);
    pci_config_write(s_pci.bus, s_pci.slot, s_pci.func, 0x04, (cmd | 0x06) | (1u << 10));

    s_mmio = (uintptr_t)base;                       // низ 4 ГБ отображён 1:1
    // Первое двойное слово содержит и CAPLENGTH, и HCIVERSION. Читать его
    // надо целиком: не всякий контроллер отвечает на обращение по 2 байта
    // (у QEMU версия так и читалась нулём).
    uint32_t cap0  = rd32(s_mmio + XCAP_CAPLENGTH);
    uint8_t caplen = (uint8_t)(cap0 & 0xFF);
    uint16_t ver   = (uint16_t)(cap0 >> 16);
    uint32_t hcs1  = rd32(s_mmio + XCAP_HCSPARAMS1);
    uint32_t hcc1  = rd32(s_mmio + XCAP_HCCPARAMS1);
    // Если чип отвечает нулями или единицами, дальше идти нельзя: раньше в этом
    // случае инициализация "успешно" завершалась с нулём портов и слотов, а
    // система потом молча висла на монтировании корня.
    if (cap0 == 0xFFFFFFFFu || cap0 == 0) {
        xhci_err("[xHCI] registers do not answer (controller not mapped?)\n");
        xhci_err_hex("[xHCI] BAR0 = ", base, "");
        xhci_err_hex(", CAPLENGTH dword = ", cap0, "\n");
        return -2;
    }
    // Длина блока возможностей бывает большой: у Intel это обычно 0x80, и
    // спецификация разрешает до 0xFF. Прежняя проверка "не больше 0x40"
    // отвергала совершенно здоровый контроллер (ноутбук ASUS: BAR0 0xF7A00000,
    // dword 0x01000080 -- то есть caplen 0x80, версия 1.0).
    if (caplen < 0x20) {
        xhci_err("[xHCI] bad CAPLENGTH\n");
        xhci_err_hex("[xHCI] BAR0 = ", base, "");
        xhci_err_hex(", dword = ", cap0, "\n");
        return -2;
    }

    s_op = s_mmio + caplen;
    s_rt = s_mmio + (rd32(s_mmio + XCAP_RTSOFF) & ~0x1Fu);
    s_db = s_mmio + (rd32(s_mmio + XCAP_DBOFF)  & ~0x3u);

    s_max_slots = hcs1 & 0xFF;
    s_max_ports = (hcs1 >> 24) & 0xFF;
    xhci_ctx64 = (hcc1 & (1u << 2)) ? 1 : 0;

    if (!s_max_ports || !s_max_slots) {
        xhci_err("[xHCI] controller reports no ports/slots -- giving up\n");
        xhci_err_hex("[xHCI] HCSPARAMS1 = ", hcs1, "");
        xhci_err_hex(", HCCPARAMS1 = ", hcc1, "");
        xhci_err_hex(", BAR0 = ", base, "\n");
        return -2;
    }

    xhci_log("[xHCI] controller found, version ", 0x55FFFF);
    if (s_verbose) { char b[16]; itoa((ver >> 8) & 0xFF, b); kputs(b, 0xFFFFFF); kputs(".", 0xFFFFFF);
      itoa(ver & 0xFF, b); kputs(b, 0xFFFFFF); kputs("\n", 0); }
    xhci_log_num("[xHCI] ports: ", s_max_ports, "");
    xhci_log_num(", slots: ", s_max_slots, "");
    xhci_log(xhci_ctx64 ? ", 64-byte context\n" : ", 32-byte context\n", 0xAAAAAA);

    xhci_bios_handoff(hcc1);

    if (xhci_reset() < 0) { xhci_err("[xHCI] controller reset failed\n"); return -3; }
    if (xhci_setup_rings() < 0) { xhci_err("[xHCI] out of memory for rings\n"); return -4; }

    // Сколько слотов устройств разрешаем
    wr32(s_op + XOP_CONFIG, (uint32_t)s_max_slots);

    // Поехали
    wr32(s_op + XOP_USBCMD, rd32(s_op + XOP_USBCMD) | USBCMD_RS);
    for (int i = 0; i < 100; i++) {
        if (!(rd32(s_op + XOP_USBSTS) & USBSTS_HCH)) break;
        sleep_ms(1);
    }
    if (rd32(s_op + XOP_USBSTS) & USBSTS_HCH) {
        xhci_err("[xHCI] controller did not start\n");
        return -3;
    }

    // Проверка живости колец: пустая команда должна вернуть событие.
    xhci_cmd_submit(0, 0, TRB_CMD_NOOP << 10);
    xhci_trb_t ev;
    if (xhci_event_wait(&ev, 500)) {
        int code = (int)(ev.status >> 24);
        xhci_log_num("[xHCI] command ring answers, code ", code, "\n");
    } else {
        xhci_err("[xHCI] command ring does not answer\n");
        // Что именно молчит -- видно только по регистрам. USBSTS скажет, бежит
        // ли контроллер и не упал ли он с ошибкой хоста; CRCR -- подхватил ли он
        // кольцо команд (бит 3 CRR); ERDP -- дошёл ли он до кольца событий.
        xhci_err_hex("[xHCI] USBSTS = ", rd32(s_op + XOP_USBSTS), "");
        xhci_err_hex(", USBCMD = ", rd32(s_op + XOP_USBCMD), "\n");
        xhci_err_hex("[xHCI] CRCR = ", rd32(s_op + XOP_CRCR), "");
        xhci_err_hex(":", rd32(s_op + XOP_CRCR + 4), "");
        xhci_err_hex(", ERDP = ", rd32(s_rt + XRT_ERDP(0)), "");
        xhci_err_hex(", ERSTBA = ", rd32(s_rt + XRT_ERSTBA(0)), "\n");
        xhci_err_hex("[xHCI] cmd ring phys = ", s_cmd_phys, "");
        xhci_err_hex(", evt ring phys = ", s_evt_phys, "\n");
    }

    s_ready = 1;
    xhci_route_intel_ports();
    xhci_scan_ports();
    return 0;
}
