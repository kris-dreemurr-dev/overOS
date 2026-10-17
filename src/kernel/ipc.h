#ifndef IPC_H
#define IPC_H

#include <stdint.h>

// =============================================================================
// IPC: сообщения между процессами и разделяемая память — общие механизмы ядра,
// поверх которых живут сервисы userspace (оконный протокол desktop и др.).
// Ядро ничего не знает об окнах: оно только доставляет сообщения и делит страницы.
//
//   Сообщения: у каждого процесса почтовый ящик (очередь до IPC_QUEUE_LEN сообщений).
//   Ядро само подписывает отправителя (pid, uid) — подделать нельзя (как SO_PEERCRED).
//   Имена: сервис (только root) регистрирует имя, клиенты находят его pid.
//   Разделяемая память: объект из страниц, отображается в процессы по одному и тому же
//   адресу (SHM_BASE + слот). Отобразить может владелец, root или тот, кому дали grant.
// =============================================================================

#define IPC_MSG_MAX    256
#define IPC_QUEUE_LEN  64
#define IPC_NAME_MAX   32

typedef struct {
    int32_t  sender_pid;
    int32_t  sender_uid;
    uint32_t len;
    uint32_t reserved;
    uint8_t  data[IPC_MSG_MAX];
} ipc_msg_t;

#define SHM_BASE       0x0000500000000000ULL
#define SHM_SLOT_SIZE  (64ULL * 1024 * 1024)
#define SHM_MAX        64
#define SHM_MAX_SIZE   (32ULL * 1024 * 1024)

struct task;

int      ipc_send(struct task* from, int dst_pid, const void* data, uint32_t len);
// 1 — сообщение получено, 0 — таймаут (timeout_ms: 0 — не ждать, 0xFFFFFFFF — бесконечно)
int      ipc_recv(struct task* me, ipc_msg_t* out, uint32_t timeout_ms);
int      ipc_has_message(int pid);

int      ipc_register(struct task* me, const char* name);
int      ipc_lookup(const char* name);

int      shm_create(struct task* me, uint64_t size);
uint64_t shm_map(struct task* me, int id);           // 0 — ошибка
int      shm_unmap(struct task* me, int id);
int      shm_grant(struct task* me, int id, int pid);
int      shm_destroy(struct task* me, int id);
uint64_t shm_size(int id);

// Процесс завершился (exit/kill/сбой): ящик, имена, его объекты shm
void     ipc_task_exit(int pid);

#endif
