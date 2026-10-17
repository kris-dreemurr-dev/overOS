#include "lib_loader.h"
#include "sched.h"
#include "users.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "../fs/fs.h"
#include <stdint.h>

// =============================================================================
// Загрузчик разделяемых библиотек .ll (см. lib_loader.h).
//
// Библиотека кладётся в адресное пространство процесса, который её открыл:
// LIB_BASE + слот * LIB_SLOT_SIZE. Сначала идёт образ (.text/.rodata/.data и
// обнулённый .bss), следом -- таблицы экспорта из файла, по ним lib_sym ищет
// имя. Таблицы лежат в памяти процесса, а не в ядре, поэтому загрузчику не
// нужна куча; подменить их процесс может только себе во вред: lib_sym всё равно
// возвращает адрес внутри того же слота.
// =============================================================================

extern uint8_t kernel_temp_buf[];            // общий буфер чтения файлов (prog_loader.c)
extern void fs_lock(void);
extern void fs_unlock(void);
extern const char* fs_caller_cwd(void);

#define LIB_TEMP_MAX (4u * 1024 * 1024)      // размер kernel_temp_buf

// Временный буфер под файл библиотеки: маленькие читаем в общий kernel_temp_buf,
// крупные (настоящий FFmpeg) -- в собственные страницы, которые тут же и отдаём.
static void lib_tmp_free(uint8_t* tmp, uint64_t pages) {
    if (!pages) return;
    uint64_t phys = virt_to_phys((uint64_t)tmp);
    for (uint64_t i = 0; i < pages; i++) pmm_free_page((void*)(phys + i * 4096));
}
#define LIB_ENTRIES  (LIB_MAX_SLOTS * 8)     // на всю систему

typedef struct {
    int      used;
    uint64_t pid;
    int      slot;
    uint64_t base;               // LIB_BASE + слот * LIB_SLOT_SIZE
    uint32_t pages;              // сколько страниц отображено
    uint64_t mem_size;           // образ + .bss (предел для значения символа)
    uint64_t sym_va;             // таблица ll_sym_t в памяти процесса
    uint64_t str_va;             // строки имён
    uint64_t sym_count;
    uint64_t str_size;
    int      refs;               // повторный open того же файла тем же процессом
    char     path[96];
} lib_entry_t;

static lib_entry_t s_libs[LIB_ENTRIES];

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static void str_copy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static lib_entry_t* entry_of(uint64_t pid, int handle) {
    if (handle < 0 || handle >= LIB_MAX_SLOTS) return 0;
    for (int i = 0; i < LIB_ENTRIES; i++)
        if (s_libs[i].used && s_libs[i].pid == pid && s_libs[i].slot == handle) return &s_libs[i];
    return 0;
}

static int slot_free(uint64_t pid, int slot) {
    for (int i = 0; i < LIB_ENTRIES; i++)
        if (s_libs[i].used && s_libs[i].pid == pid && s_libs[i].slot == slot) return 0;
    return 1;
}

// Проверка заголовка: данные из файла недоверенные, каждое смещение должно лежать
// внутри файла, а образ -- помещаться в слот.
static int header_ok(const ll_header_t* h, uint32_t bytes) {
    if (bytes < sizeof(ll_header_t)) return 0;
    if (h->magic[0] != LL_MAGIC0 || h->magic[1] != LL_MAGIC1 ||
        h->magic[2] != LL_MAGIC2 || h->magic[3] != LL_MAGIC3) return 0;
    if (h->version != 1) return 0;
    if (h->image_off < sizeof(ll_header_t) || h->image_off > bytes) return 0;
    if (h->image_size > LIB_MAX_IMAGE || h->mem_size > LIB_MAX_MEM) return 0;
    if (h->mem_size < h->image_size) return 0;
    if (h->image_off + h->image_size > bytes) return 0;
    if (h->reloc_off + (uint64_t)h->reloc_count * 4 > bytes) return 0;
    if (h->sym_off + h->sym_count * sizeof(ll_sym_t) > bytes) return 0;
    if (h->str_off + h->str_size > bytes) return 0;
    if (h->sym_count > 65536 || h->str_size > (1u << 20)) return 0;
    return 1;
}

int lib_open(const char* path, int uid, int gid) {
    task_t* me = sched_get_current_task();
    if (!me || !path || !path[0]) return LIB_ENOENT;

    // Уже открыта этим процессом -- отдаём тот же дескриптор
    for (int i = 0; i < LIB_ENTRIES; i++)
        if (s_libs[i].used && s_libs[i].pid == me->pid && str_eq(s_libs[i].path, path)) {
            s_libs[i].refs++;
            return s_libs[i].slot;
        }

    if (!perm_check(uid, gid, path, fs_caller_cwd(), PERM_X)) return LIB_EPERM;

    lib_entry_t* e = 0;
    for (int i = 0; i < LIB_ENTRIES; i++) if (!s_libs[i].used) { e = &s_libs[i]; break; }
    if (!e) return LIB_ENOMEM;

    int slot = -1;
    for (int i = 0; i < LIB_MAX_SLOTS; i++) if (slot_free(me->pid, i)) { slot = i; break; }
    if (slot < 0) return LIB_ENOMEM;

    // Файл читаем во временный буфер. Общий kernel_temp_buf -- всего 4 МБ, а
    // настоящий FFmpeg со всеми кодеками весит больше, и раньше чтение просто
    // обрывалось: заголовок не сходился, и библиотека считалась битой.
    fs_lock();
    uint32_t fsize = fs_file_size(path);
    if (fsize == 0 || fsize > LIB_MAX_IMAGE + 16u * 1024 * 1024) { fs_unlock(); return LIB_ENOEXEC; }

    uint8_t* tmp = kernel_temp_buf;
    uint64_t tmp_pages = 0;
    if (fsize > LIB_TEMP_MAX) {
        tmp_pages = (fsize + 4095) / 4096;
        void* ph = pmm_alloc_pages((size_t)tmp_pages);
        if (!ph) { fs_unlock(); return LIB_ENOMEM; }
        tmp = (uint8_t*)PHYS_TO_VIRT(ph);
    }

    int bytes = fs_read_file(path, tmp, tmp_pages ? (int)(tmp_pages * 4096) : LIB_TEMP_MAX);
    if (bytes <= 0) { lib_tmp_free(tmp, tmp_pages); fs_unlock(); return LIB_ENOENT; }

    const ll_header_t* h = (const ll_header_t*)tmp;
    if (!header_ok(h, (uint32_t)bytes)) { lib_tmp_free(tmp, tmp_pages); fs_unlock(); return LIB_ENOEXEC; }

    // Релокации должны попадать в файловую часть образа
    const uint32_t* rel = (const uint32_t*)(tmp + h->reloc_off);
    for (uint32_t i = 0; i < h->reloc_count; i++)
        if ((uint64_t)rel[i] + 8 > h->image_size) { lib_tmp_free(tmp, tmp_pages); fs_unlock(); return LIB_ENOEXEC; }

    uint64_t base      = LIB_BASE + (uint64_t)slot * LIB_SLOT_SIZE;
    uint64_t img_pages = (h->mem_size + 4095) / 4096;
    uint64_t tbl_bytes = h->sym_count * sizeof(ll_sym_t) + h->str_size;
    uint64_t tbl_pages = (tbl_bytes + 4095) / 4096;
    if (img_pages == 0) img_pages = 1;
    uint64_t total = img_pages + tbl_pages;
    if (total * 4096 > LIB_SLOT_SIZE) { lib_tmp_free(tmp, tmp_pages); fs_unlock(); return LIB_ENOMEM; }

    for (uint64_t i = 0; i < total; i++) {
        void* p = pmm_alloc_page();
        if (!p || !vmm_map_page((uint64_t*)me->cr3, base + i * 4096, (uint64_t)p,
                                VMM_FLAG_USER | VMM_FLAG_WRITABLE)) {
            if (p) pmm_free_page(p);
            for (uint64_t k = 0; k < i; k++) {                 // откат отображённых страниц
                uint64_t phys = vmm_unmap_page_take((uint64_t*)me->cr3, base + k * 4096);
                if (phys) pmm_free_page((void*)phys);
            }
            lib_tmp_free(tmp, tmp_pages);
            fs_unlock();
            return LIB_ENOMEM;
        }
    }

    // Копирование образа и таблиц в память процесса (cr3 уже наш -- это его сисколл)
    uint8_t* dst = (uint8_t*)base;
    const uint8_t* src = tmp + h->image_off;
    for (uint64_t i = 0; i < h->image_size; i++) dst[i] = src[i];
    for (uint64_t i = h->image_size; i < img_pages * 4096; i++) dst[i] = 0;   // .bss и хвост страницы

    uint64_t sym_va = base + img_pages * 4096;
    uint64_t str_va = sym_va + h->sym_count * sizeof(ll_sym_t);
    uint8_t* tdst = (uint8_t*)sym_va;
    const uint8_t* ssrc = tmp + h->sym_off;
    for (uint64_t i = 0; i < h->sym_count * sizeof(ll_sym_t); i++) tdst[i] = ssrc[i];
    uint8_t* strdst = (uint8_t*)str_va;
    const uint8_t* strsrc = tmp + h->str_off;
    for (uint64_t i = 0; i < h->str_size; i++) strdst[i] = strsrc[i];

    // Перемещение: к каждой 64-битной ячейке из таблицы прибавляем базу
    for (uint32_t i = 0; i < h->reloc_count; i++) {
        uint64_t* cell = (uint64_t*)(base + rel[i]);
        *cell += base;
    }

    e->used = 1;
    e->pid = me->pid;
    e->slot = slot;
    e->base = base;
    e->pages = (uint32_t)total;
    e->mem_size = h->mem_size;
    e->sym_va = sym_va;
    e->str_va = str_va;
    e->sym_count = h->sym_count;
    e->str_size = h->str_size;
    e->refs = 1;
    str_copy(e->path, path, sizeof(e->path));

    me->mem_size += total * 4096;
    lib_tmp_free(tmp, tmp_pages);
    fs_unlock();
    return slot;
}

uint64_t lib_sym(int handle, const char* name) {
    task_t* me = sched_get_current_task();
    if (!me || !name) return 0;
    lib_entry_t* e = entry_of(me->pid, handle);
    if (!e) return 0;

    const ll_sym_t* syms = (const ll_sym_t*)e->sym_va;
    const char* str = (const char*)e->str_va;
    for (uint64_t i = 0; i < e->sym_count; i++) {
        if (syms[i].name_off >= e->str_size) continue;
        if (!str_eq(str + syms[i].name_off, name)) continue;
        if (syms[i].value >= e->mem_size) return 0;        // таблицу испортили -- наружу не пускаем
        return e->base + syms[i].value;
    }
    return 0;
}

static void lib_drop(lib_entry_t* e) {
    task_t* me = sched_get_current_task();
    if (me && me->pid == e->pid) {
        for (uint32_t i = 0; i < e->pages; i++) {
            uint64_t phys = vmm_unmap_page_take((uint64_t*)me->cr3, e->base + (uint64_t)i * 4096);
            if (phys) pmm_free_page((void*)phys);
            if (me->mem_size >= 4096) me->mem_size -= 4096;
        }
    }
    e->used = 0;
}

int lib_close(int handle) {
    task_t* me = sched_get_current_task();
    if (!me) return -1;
    lib_entry_t* e = entry_of(me->pid, handle);
    if (!e) return -1;
    if (--e->refs > 0) return 0;
    lib_drop(e);
    return 0;
}

// fork(): образы библиотек скопировались вместе с адресным пространством и лежат
// по тем же адресам, поэтому ребёнку достаются такие же записи. Без этого слоты
// ребёнка считались бы свободными и следующая его библиотека легла бы поверх.
void lib_fork(uint64_t parent_pid, uint64_t child_pid) {
    for (int i = 0; i < LIB_ENTRIES; i++) {
        if (!s_libs[i].used || s_libs[i].pid != parent_pid) continue;
        for (int j = 0; j < LIB_ENTRIES; j++) {
            if (s_libs[j].used) continue;
            s_libs[j] = s_libs[i];
            s_libs[j].pid = child_pid;
            s_libs[j].refs = 1;
            break;
        }
    }
}

// Процесс завершился: страницы освободит vmm_destroy_address_space, нам остаётся
// отпустить записи таблицы, иначе слоты кончились бы после нескольких запусков.
void lib_release_pid(uint64_t pid) {
    for (int i = 0; i < LIB_ENTRIES; i++)
        if (s_libs[i].used && s_libs[i].pid == pid) s_libs[i].used = 0;
}
