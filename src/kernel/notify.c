// =============================================================================
// notify.c — очередь уведомлений от имени системы.
//
// Кто пишет: монтирование ("подключено устройство", "можно извлечь", "флешка с
// системой отключена"). Кто читает: рабочий стол, раз в четверть секунды
// (sys_notify_poll, только root). Если рабочего стола нет -- очередь просто
// переполняется и старые уведомления вытесняются новыми: ядру всё равно,
// смотрит ли кто-нибудь.
// =============================================================================

#include "notify.h"

#define KNOTIFY_QUEUE 16

static knotify_t s_q[KNOTIFY_QUEUE];
static int       s_head = 0, s_count = 0;
static uint32_t  s_next_id = 100;      // младшие id -- постоянные (см. mount.c)

// В ядре нет memcpy, а присваивание структуры GCC превращает именно в него
static void ncopy(knotify_t* d, const knotify_t* s) {
    for (int i = 0; i < (int)sizeof(*d); i++) ((uint8_t*)d)[i] = ((const uint8_t*)s)[i];
}

// Копия строки с обрезкой по размеру поля -- не посреди символа UTF-8
static void scopy(char* d, const char* s, int max) {
    int i = 0;
    if (s) while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    if (s && s[i]) while (i > 0 && (((uint8_t)s[i]) & 0xC0) == 0x80) i--;
    d[i] = '\0';
}

void knotify_fill(knotify_t* n, const char* icon, const char* title, const char* body) {
    for (int i = 0; i < (int)sizeof(*n); i++) ((uint8_t*)n)[i] = 0;
    n->urgency = KNOTIFY_NORMAL;
    scopy(n->icon, icon, sizeof(n->icon));
    scopy(n->title, title, sizeof(n->title));
    scopy(n->body, body, sizeof(n->body));
}

void knotify_action(knotify_t* n, int i, const char* label, const char* cmd) {
    if (i < 0 || i > 1) return;
    scopy(n->act_label[i], label, sizeof(n->act_label[i]));
    scopy(n->act_cmd[i], cmd, sizeof(n->act_cmd[i]));
}

uint32_t knotify_post(const knotify_t* n) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int slot;
    if (s_count == KNOTIFY_QUEUE) {           // полно -- вытесняем самое старое
        slot = s_head;
        s_head = (s_head + 1) % KNOTIFY_QUEUE;
    } else {
        slot = (s_head + s_count) % KNOTIFY_QUEUE;
        s_count++;
    }
    ncopy(&s_q[slot], n);
    if (!s_q[slot].id) s_q[slot].id = s_next_id++;
    uint32_t id = s_q[slot].id;
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return id;
}

void knotify_close(uint32_t id) {
    knotify_t n;
    for (int i = 0; i < (int)sizeof(n); i++) ((uint8_t*)&n)[i] = 0;
    n.id = id;
    n.close = 1;
    knotify_post(&n);
}

int knotify_pop(knotify_t* out) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int ok = 0;
    if (s_count) {
        ncopy(out, &s_q[s_head]);
        s_head = (s_head + 1) % KNOTIFY_QUEUE;
        s_count--;
        ok = 1;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return ok;
}
