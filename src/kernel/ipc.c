#include "ipc.h"
#include "sched.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

extern uint32_t timer_ticks;

#define MAX_BOXES   64
#define MAX_NAMES   16
#define SHM_GRANTS  8
#define SHM_MAPS    16

// -----------------------------------------------------------------------------
// Почтовые ящики
// -----------------------------------------------------------------------------
typedef struct {
    int       pid;
    uint32_t  head, tail;              // head - tail = число сообщений
    ipc_msg_t msgs[IPC_QUEUE_LEN];
} ipc_box_t;

#define BOX_PAGES ((sizeof(ipc_box_t) + 4095) / 4096)

static ipc_box_t* g_boxes[MAX_BOXES];

static ipc_box_t* box_find(int pid) {
    for (int i = 0; i < MAX_BOXES; i++)
        if (g_boxes[i] && g_boxes[i]->pid == pid) return g_boxes[i];
    return 0;
}

static ipc_box_t* box_get(int pid) {
    ipc_box_t* b = box_find(pid);
    if (b) return b;
    for (int i = 0; i < MAX_BOXES; i++) {
        if (g_boxes[i]) continue;
        void* phys = pmm_alloc_pages(BOX_PAGES);
        if (!phys) return 0;
        b = (ipc_box_t*)PHYS_TO_VIRT(phys);
        b->pid = pid;
        b->head = b->tail = 0;
        g_boxes[i] = b;
        return b;
    }
    return 0;
}

static void box_free(int pid) {
    for (int i = 0; i < MAX_BOXES; i++) {
        if (!g_boxes[i] || g_boxes[i]->pid != pid) continue;
        uint64_t phys = virt_to_phys((uint64_t)g_boxes[i]);
        for (uint32_t p = 0; p < BOX_PAGES; p++) pmm_free_page((void*)(phys + (uint64_t)p * 4096));
        g_boxes[i] = 0;
    }
}

static void k_copy(void* d, const void* s, uint64_t n) {
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

int ipc_send(task_t* from, int dst_pid, const void* data, uint32_t len) {
    if (!from || len > IPC_MSG_MAX) return -1;
    if (!sched_pid_is_process((uint64_t)dst_pid)) return -1;

    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    ipc_box_t* b = box_get(dst_pid);
    int r = -1;
    if (b && b->head - b->tail < IPC_QUEUE_LEN) {
        ipc_msg_t* m = &b->msgs[b->head % IPC_QUEUE_LEN];
        m->sender_pid = (int32_t)from->pid;
        m->sender_uid = from->uid;
        m->len = len;
        m->reserved = 0;
        k_copy(m->data, data, len);
        b->head++;
        sched_wake_ipc((uint64_t)dst_pid);     // получатель спит в ipc_recv — будим
        r = 0;
    } else if (b) {
        r = -2;                                 // очередь получателя полна
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return r;
}

int ipc_has_message(int pid) {
    ipc_box_t* b = box_find(pid);
    return b && b->head != b->tail;
}

static int box_pop(task_t* me, ipc_msg_t* out) {
    ipc_box_t* b = box_find((int)me->pid);
    if (!b || b->head == b->tail) { me->ipc_pending = 0; return 0; }
    k_copy(out, &b->msgs[b->tail % IPC_QUEUE_LEN], sizeof(ipc_msg_t));
    b->tail++;
    me->ipc_pending = (b->head != b->tail);
    return 1;
}

int ipc_recv(task_t* me, ipc_msg_t* out, uint32_t timeout_ms) {
    if (!me) return 0;
    uint32_t start = *(volatile uint32_t*)&timer_ticks;
    while (1) {
        if (box_pop(me, out)) return 1;
        if (timeout_ms == 0) return 0;
        uint32_t elapsed = *(volatile uint32_t*)&timer_ticks - start;
        if (timeout_ms != 0xFFFFFFFFu && elapsed >= timeout_ms) return 0;

        // Спим, пока не придёт сообщение или не выйдет время: планировщик пропускает
        // спящую задачу, процессор достаётся другим (или простаивает в hlt)
        me->wake_tick  = start + timeout_ms;
        me->wait_flags = WAIT_IPC | (timeout_ms != 0xFFFFFFFFu ? WAIT_TIMER : 0);
        me->state = TASK_SLEEPING;
        __asm__ volatile("sti");
        sched_yield();
        __asm__ volatile("cli");
        me->wait_flags = 0;
        if (me->state == TASK_SLEEPING) me->state = TASK_RUNNING;
        if (me->state == TASK_ZOMBIE || me->state == TASK_DEAD) return 0;
    }
}

// -----------------------------------------------------------------------------
// Имена сервисов
// -----------------------------------------------------------------------------
typedef struct {
    char name[IPC_NAME_MAX];
    int  pid;
} ipc_name_t;

static ipc_name_t g_names[MAX_NAMES];

static int n_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int ipc_register(task_t* me, const char* name) {
    // Регистрировать сервисы может только root: иначе обычный пользователь занял бы имя
    // "desktop" (пока его нет) и получал бы окна и ввод чужих программ
    if (!me || me->uid != 0 || !name[0]) return -1;
    for (int i = 0; i < MAX_NAMES; i++) {
        if (g_names[i].pid && n_eq(g_names[i].name, name)) {
            if (sched_pid_is_process((uint64_t)g_names[i].pid)) return -1;   // занято живым
            g_names[i].pid = 0;
        }
    }
    for (int i = 0; i < MAX_NAMES; i++) {
        if (g_names[i].pid) continue;
        int k = 0;
        while (name[k] && k < IPC_NAME_MAX - 1) { g_names[i].name[k] = name[k]; k++; }
        g_names[i].name[k] = '\0';
        g_names[i].pid = (int)me->pid;
        return 0;
    }
    return -1;
}

int ipc_lookup(const char* name) {
    for (int i = 0; i < MAX_NAMES; i++)
        if (g_names[i].pid && n_eq(g_names[i].name, name) && sched_pid_is_process((uint64_t)g_names[i].pid))
            return g_names[i].pid;
    return -1;
}

// -----------------------------------------------------------------------------
// Разделяемая память
// -----------------------------------------------------------------------------
typedef struct {
    int      used;
    int      owner_pid;
    int      owner_uid;
    int      destroyed;            // владелец удалил/умер — освободить, когда никто не отображает
    uint64_t phys;
    uint32_t pages;
    int      grants[SHM_GRANTS];
    int      maps[SHM_MAPS];       // pid, у кого объект отображён
} shm_t;

static shm_t g_shm[SHM_MAX];

static shm_t* shm_get(int id) {
    if (id < 1 || id > SHM_MAX || !g_shm[id - 1].used) return 0;
    return &g_shm[id - 1];
}

static uint64_t shm_vaddr(int id) {
    return SHM_BASE + (uint64_t)(id - 1) * SHM_SLOT_SIZE;
}

static void shm_maybe_free(shm_t* s) {
    if (!s->destroyed) return;
    for (int i = 0; i < SHM_MAPS; i++) if (s->maps[i]) return;
    for (uint32_t p = 0; p < s->pages; p++) pmm_free_page((void*)(s->phys + (uint64_t)p * 4096));
    s->used = 0;
}

int shm_create(task_t* me, uint64_t size) {
    if (!me || size == 0 || size > SHM_MAX_SIZE) return -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (g_shm[i].used) continue;
        uint32_t pages = (uint32_t)((size + 4095) / 4096);
        void* phys = pmm_alloc_pages(pages);
        if (!phys) return -1;
        uint8_t* v = (uint8_t*)PHYS_TO_VIRT(phys);
        for (uint64_t b = 0; b < (uint64_t)pages * 4096; b++) v[b] = 0;
        shm_t* s = &g_shm[i];
        s->used = 1;
        s->owner_pid = (int)me->pid;
        s->owner_uid = me->uid;
        s->destroyed = 0;
        s->phys = (uint64_t)phys;
        s->pages = pages;
        for (int k = 0; k < SHM_GRANTS; k++) s->grants[k] = 0;
        for (int k = 0; k < SHM_MAPS; k++) s->maps[k] = 0;
        return i + 1;
    }
    return -1;
}

static int shm_allowed(shm_t* s, task_t* me) {
    if (me->uid == 0 || (int)me->pid == s->owner_pid) return 1;
    for (int k = 0; k < SHM_GRANTS; k++) if (s->grants[k] == (int)me->pid) return 1;
    return 0;
}

uint64_t shm_map(task_t* me, int id) {
    shm_t* s = shm_get(id);
    if (!me || !s || s->destroyed || !shm_allowed(s, me)) return 0;
    uint64_t va = shm_vaddr(id);
    for (int k = 0; k < SHM_MAPS; k++) if (s->maps[k] == (int)me->pid) return va;   // уже отображён

    int slot = -1;
    for (int k = 0; k < SHM_MAPS; k++) if (!s->maps[k]) { slot = k; break; }
    if (slot < 0) return 0;

    for (uint32_t p = 0; p < s->pages; p++) {
        if (!vmm_map_page((uint64_t*)me->cr3, va + (uint64_t)p * 4096, s->phys + (uint64_t)p * 4096,
                          VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_SHARED)) {
            for (uint32_t q = 0; q < p; q++) vmm_unmap_page((uint64_t*)me->cr3, va + (uint64_t)q * 4096);
            return 0;
        }
    }
    s->maps[slot] = (int)me->pid;
    return va;
}

int shm_unmap(task_t* me, int id) {
    shm_t* s = shm_get(id);
    if (!me || !s) return -1;
    for (int k = 0; k < SHM_MAPS; k++) {
        if (s->maps[k] != (int)me->pid) continue;
        uint64_t va = shm_vaddr(id);
        for (uint32_t p = 0; p < s->pages; p++) vmm_unmap_page((uint64_t*)me->cr3, va + (uint64_t)p * 4096);
        s->maps[k] = 0;
        shm_maybe_free(s);
        return 0;
    }
    return -1;
}

int shm_grant(task_t* me, int id, int pid) {
    shm_t* s = shm_get(id);
    if (!me || !s || (s->owner_pid != (int)me->pid && me->uid != 0)) return -1;
    for (int k = 0; k < SHM_GRANTS; k++) if (s->grants[k] == pid) return 0;
    for (int k = 0; k < SHM_GRANTS; k++) if (!s->grants[k]) { s->grants[k] = pid; return 0; }
    return -1;
}

int shm_destroy(task_t* me, int id) {
    shm_t* s = shm_get(id);
    if (!me || !s || (s->owner_pid != (int)me->pid && me->uid != 0)) return -1;
    s->destroyed = 1;
    shm_maybe_free(s);
    return 0;
}

uint64_t shm_size(int id) {
    shm_t* s = shm_get(id);
    return s ? (uint64_t)s->pages * 4096 : 0;
}

void ipc_task_exit(int pid) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    box_free(pid);
    for (int i = 0; i < MAX_NAMES; i++) if (g_names[i].pid == pid) g_names[i].pid = 0;
    for (int i = 0; i < SHM_MAX; i++) {
        shm_t* s = &g_shm[i];
        if (!s->used) continue;
        // Его адресное пространство уничтожится целиком; страницы shm там помечены SHARED
        // и не освобождаются вместе с ним — освобождаем здесь, когда их больше никто не видит
        for (int k = 0; k < SHM_MAPS; k++) if (s->maps[k] == pid) s->maps[k] = 0;
        for (int k = 0; k < SHM_GRANTS; k++) if (s->grants[k] == pid) s->grants[k] = 0;
        if (s->owner_pid == pid) s->destroyed = 1;
        shm_maybe_free(s);
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
}
