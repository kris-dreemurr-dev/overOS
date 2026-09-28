// memedit_module.c — Game Trainer + VRAM Glitcher для devOS (Ring 0).

#include "../drivers/api.h"
#include <stdint.h>

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t memedit_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "MEMEDIT_SYS",
    .flags = 0
};

static const devos_api_t* g_api = 0;

#define MAX_MATCHES 2048
static uintptr_t matches[MAX_MATCHES];
static int match_count = 0;

#define MAX_GROUPS 8
#define MAX_GROUP_ADDRS 32
#define MAX_NAME_LEN 24

typedef struct {
    int in_use;
    char name[MAX_NAME_LEN];
    uintptr_t addrs[MAX_GROUP_ADDRS];
    int count;
} mem_group_t;

static mem_group_t g_groups[MAX_GROUPS];
static const char hex_digits[] = "0123456789ABCDEF";

// Простой генератор псевдослучайных чисел (Xorshift32)
static uint32_t g_seed = 0x12345678;
static inline uint32_t rand_u32(void) {
    uint32_t x = g_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_seed = x;
    return x;
}

static int local_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void local_strcpy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

static void print_hex32(uint32_t val, uint32_t color) {
    for (int i = 7; i >= 0; i--) g_api->kputc(hex_digits[(val >> (i * 4)) & 0x0F], color);
}

static void print_hex64(uint64_t val, uint32_t color) {
    print_hex32((uint32_t)(val >> 32), color);
    print_hex32((uint32_t)val, color);
}

static void print_dec(int val, uint32_t color) {
    if (val < 0) { g_api->kputc('-', color); val = -val; }
    char buf[16];
    int idx = 0;
    if (val == 0) buf[idx++] = '0';
    while (val > 0) { buf[idx++] = '0' + (val % 10); val /= 10; }
    while (idx > 0) g_api->kputc(buf[--idx], color);
}

static uint64_t parse_hex(const char* str) {
    while (*str == ' ') str++;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    uint64_t val = 0;
    while (*str) {
        char c = *str;
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        val = (val << 4) | d;
        str++;
    }
    return val;
}

static int parse_dec(const char* str) {
    while (*str == ' ') str++;
    int sign = 1;
    if (*str == '-') { sign = -1; str++; }
    int val = 0;
    while (*str >= '0' && *str <= '9') { val = val * 10 + (*str - '0'); str++; }
    return val * sign;
}

static void sanitize_name(const char* src, char* dest, int max_len) {
    while (*src == ' ' || *src == '"' || *src == '\'') src++;
    int i = 0;
    while (*src && *src != ' ' && *src != '"' && *src != '\'' && i < max_len - 1) {
        dest[i++] = *src++;
    }
    dest[i] = '\0';
}

// ---------------------------------------------------------------- Логика GOD MODE (вечная фиксация)

static void cmd_god_dispatch(const char* target, int val) {
    char clean_target[MAX_NAME_LEN];
    sanitize_name(target, clean_target, sizeof(clean_target));

    int is_group = -1;
    for (int g = 0; g < MAX_GROUPS; g++) {
        if (g_groups[g].in_use && local_strcmp(g_groups[g].name, clean_target) == 0) {
            is_group = g;
            break;
        }
    }

    uintptr_t single_addr = 0;
    if (is_group == -1) {
        single_addr = parse_hex(clean_target);
        if (single_addr == 0) {
            g_api->kputs("Invalid group or address.\n", 0xFF5555);
            return;
        }
    }

    g_api->kputs("[PERMANENT GOD ACTIVE] Value locked to ", 0x55FF55);
    print_dec(val, 0xFFFF55);
    g_api->kputs(".\nPress ESC or any key to stop freeze.\n", 0xAAAAAA);
    g_api->flush_buffer();

    // Непрерывный цикл: удерживает адреса до ввода символа с клавиатуры
    while (1) {
        if (is_group != -1) {
            for (int i = 0; i < g_groups[is_group].count; i++) {
                *(volatile int32_t*)g_groups[is_group].addrs[i] = val;
            }
        } else {
            *(volatile int32_t*)single_addr = val;
        }

        // Небольшая разгрузка шины памяти
        for (int p = 0; p < 200; p++) {
            __asm__ volatile ("pause");
        }

        // Выход при нажатии клавиши
        char probe[2];
        if (g_api->readline && g_api->readline(probe, 0)) break;
    }

    g_api->kputs("God freeze stopped.\n", 0xAAAAAA);
}

// ---------------------------------------------------------------- Шалости: порча VRAM и памяти

// Эффект помех аналогового ТВ и полос в видеопамяти
static void cmd_vram_glitch(int duration_frames) {
    g_api->kputs("Corrupting display buffers...\n", 0xFF5555);
    g_api->flush_buffer();

    // Поиск кадровых буферов в пространстве Ring 3 / кучи
    volatile uint32_t* fb = (volatile uint32_t*)0x01600000;

    for (int f = 0; f < duration_frames * 1000; f++) {
        int offset = rand_u32() % (320 * 200);
        uint32_t glitch_color = (rand_u32() & 0x01) ? 0x00FF00FF : (rand_u32() & 0x00FFFFFF);
        
        // Рисуем артефактные полосы
        fb[offset] = glitch_color;
        if (offset + 1 < 320 * 200) fb[offset + 1] = glitch_color;
    }

    g_api->kputs("Glitch injection complete.\n", 0x55FF55);
}

// Прямая порча заданного диапазона ОЗУ
static void cmd_corrupt(uintptr_t addr, int size_bytes) {
    g_api->kputs("Flooding memory at 0x", 0xFF5555);
    print_hex64(addr, 0xFFFF55);
    g_api->kputs(" with garbage...\n", 0xFF5555);

    volatile uint8_t* ptr = (volatile uint8_t*)addr;
    for (int i = 0; i < size_bytes; i++) {
        ptr[i] = (uint8_t)(rand_u32() & 0xFF);
    }

    g_api->kputs("Corrupted ", 0x55FF55);
    print_dec(size_bytes, 0xFFFF55);
    g_api->kputs(" bytes.\n", 0x55FF55);
}

// ---------------------------------------------------------------- Сканирование и группы

static void cmd_find(int val, uintptr_t start_addr, uintptr_t end_addr) {
    g_api->kputs("Scanning [0x", 0x55FFFF);
    print_hex64(start_addr, 0x55FFFF);
    g_api->kputs(" - 0x", 0x55FFFF);
    print_hex64(end_addr, 0x55FFFF);
    g_api->kputs("] for: ", 0x55FFFF);
    print_dec(val, 0xFFFF55);
    g_api->kputs("...\n", 0x55FFFF);
    g_api->flush_buffer();

    match_count = 0;
    for (uintptr_t addr = start_addr; addr < end_addr - 4; addr += 4) {
        if (*(volatile int32_t*)addr == val) {
            if (match_count < MAX_MATCHES) matches[match_count++] = addr;
        }
    }

    g_api->kputs("Matches found: ", 0x55FF55);
    print_dec(match_count, 0xFFFF55);
    g_api->kputs("\n", 0xFFFFFF);

    int limit = match_count < 10 ? match_count : 10;
    for (int i = 0; i < limit; i++) {
        g_api->kputs(" [", 0xAAAAAA);
        print_dec(i, 0xAAAAAA);
        g_api->kputs("] 0x", 0xAAAAAA);
        print_hex64(matches[i], 0xFFFF55);
        g_api->kputs("\n", 0xFFFFFF);
    }
}

static void cmd_filter(int new_val) {
    int filtered = 0;
    for (int i = 0; i < match_count; i++) {
        if (*(volatile int32_t*)matches[i] == new_val) matches[filtered++] = matches[i];
    }
    match_count = filtered;

    g_api->kputs("Remaining: ", 0x55FF55);
    print_dec(match_count, 0xFFFF55);
    g_api->kputs("\n", 0xFFFFFF);
    for (int i = 0; i < match_count; i++) {
        g_api->kputs(" -> 0x", 0x55FFFF);
        print_hex64(matches[i], 0xFFFF55);
        g_api->kputs("\n", 0xFFFFFF);
    }
}

static void cmd_group(const char* raw_name) {
    if (match_count == 0) return;
    char name[MAX_NAME_LEN];
    sanitize_name(raw_name, name, sizeof(name));
    
    int slot = -1;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (!g_groups[i].in_use) { slot = i; break; }
    }
    if (slot == -1) return;

    g_groups[slot].in_use = 1;
    local_strcpy(g_groups[slot].name, name, MAX_NAME_LEN);
    g_groups[slot].count = match_count < MAX_GROUP_ADDRS ? match_count : MAX_GROUP_ADDRS;
    for (int i = 0; i < g_groups[slot].count; i++) g_groups[slot].addrs[i] = matches[i];

    g_api->kputs("Group '", 0x55FF55);
    g_api->kputs(name, 0xFFFF55);
    g_api->kputs("' saved!\n", 0x55FF55);
}

static void cmd_set(const char* target, int val) {
    char clean[MAX_NAME_LEN];
    sanitize_name(target, clean, sizeof(clean));

    for (int g = 0; g < MAX_GROUPS; g++) {
        if (g_groups[g].in_use && local_strcmp(g_groups[g].name, clean) == 0) {
            for (int i = 0; i < g_groups[g].count; i++) {
                *(volatile int32_t*)g_groups[g].addrs[i] = val;
            }
            g_api->kputs("Group updated.\n", 0x55FF55);
            return;
        }
    }
    uintptr_t addr = parse_hex(clean);
    if (addr) {
        *(volatile int32_t*)addr = val;
        g_api->kputs("Address updated.\n", 0x55FF55);
    }
}

// Красивая гибель системы в стиле аппаратного сбоя VRAM / DMA
static void cmd_meltdown(void) {
    g_api->kputs("\n[!] CRITICAL KERNEL INTEGRITY FAILURE...\n", 0xFF5555);
    g_api->flush_buffer();

    // Кадровый буфер консоли/TTY (для QEMU/devOS обычно в районе видеопамяти или буфера вывода)
    // Если у вас есть точный адрес LFB — укажите его, либо берем буфер TTY
    volatile uint32_t* vram = (volatile uint32_t*)0x01600000; 

    // 1. Фаза агонии: горизонтальный разрыв строк и шум (как на фото с EHCI)
    for (int frame = 0; frame < 120; frame++) {
        // Выбираем случайную горизонтальную полосу экрана
        int start_y = (rand_u32() % 180) + 10;
        int height  = (rand_u32() % 25) + 5;
        int shift   = (rand_u32() % 40) - 20;

        for (int y = start_y; y < start_y + height && y < 200; y++) {
            for (int x = 0; x < 320; x++) {
                int src_x = (x + shift + 320) % 320;
                
                // Характерные цвета аппаратного глитча с фото: медь, охра, белый шум
                uint32_t glitch_color;
                uint32_t r = rand_u32() & 0x0F;
                if (r < 6) {
                    glitch_color = 0x00D06818; // Медно-оранжевый (как на фото)
                } else if (r < 9) {
                    glitch_color = 0x008A4010; // Тёмная охра
                } else if (r < 11) {
                    glitch_color = 0x00E8B060; // Песочный
                } else {
                    glitch_color = (rand_u32() & 1) ? 0x00FFFFFF : 0x001A1A1A; // Статический шум
                }

                // Перемешиваем старое изображение с глитчем
                if ((rand_u32() % 100) < 65) {
                    vram[y * 320 + x] = glitch_color;
                } else {
                    vram[y * 320 + x] = vram[y * 320 + src_x];
                }
            }
        }

        // Небольшая задержка между волнами искажений
        for (volatile int pause = 0; pause < 80000; pause++) {
            __asm__ volatile ("pause");
        }
    }

    // 2. Фатальный удар: уничтожаем таблицу страниц PML4 (0x70000)
    g_api->kputs("[!] COLLAPSING MMU TRANSLATION TABLES...\n", 0xFF5555);
    volatile uint64_t* pml4 = (volatile uint64_t*)0x00070000;
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0xDEADBEEFCAFEBABE;
    }

    // 3. Вызываем Triple Fault через сброс IDTR и прерывание
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idtr = { 0, 0 };
    __asm__ volatile (
        "lidt %0\n"
        "int3\n"
        : : "m"(null_idtr)
    );

    // Если процессор каким-то чудом выжил
    while (1) { __asm__ volatile ("cli; hlt"); }
}

// ---------------------------------------------------------------- Точка входа

int driver_entry(const devos_api_t* api) {
    g_api = api;
    g_api->kputs("\n=== devOS Memory Trainer & Glitch Injector ===\n", 0x55FFFF);
    g_api->kputs("Commands: find, filter, group, set, god, glitch, corrupt, meltdown, exit\n\n", 0xAAAAAA);
    g_api->flush_buffer();

    char input[128];
    while (1) {
        g_api->kputs("trainer# ", 0xFFFF55);
        g_api->flush_buffer();

        if (!g_api->readline(input, sizeof(input))) continue;

        if (local_strcmp(input, "exit") == 0) {
            break;
        } else if (local_strcmp(input, "glitch") == 0) {
            cmd_vram_glitch(50);
        } else if (input[0] == 'c' && input[1] == 'o' && input[2] == 'r' && input[3] == 'r') {
            const char* p = input + 7;
            while (*p == ' ') p++;
            uintptr_t addr = parse_hex(p);
            while (*p != ' ' && *p != '\0') p++;
            int size = parse_dec(p);
            if (size <= 0) size = 256;
            cmd_corrupt(addr, size);
        } else if (input[0] == 'f' && input[1] == 'i' && input[2] == 'n' && input[3] == 'd') {
            cmd_find(parse_dec(input + 4), 0x01000000, 0x05000000);
        } else if (input[0] == 'f' && input[1] == 'i' && input[2] == 'l' && input[3] == 't') {
            cmd_filter(parse_dec(input + 6));
        } else if (input[0] == 'g' && input[1] == 'r' && input[2] == 'o' && input[3] == 'u' && input[4] == 'p') {
            cmd_group(input + 5);
        } else if (input[0] == 's' && input[1] == 'e' && input[2] == 't') {
            const char* p = input + 3;
            while (*p == ' ') p++;
            const char* t_start = p;
            while (*p != ' ' && *p != '\0') p++;
            int val = parse_dec(p);
            char target[MAX_NAME_LEN];
            int len = (int)(p - t_start);
            if (len >= MAX_NAME_LEN) len = MAX_NAME_LEN - 1;
            for (int i = 0; i < len; i++) target[i] = t_start[i];
            target[len] = '\0';
            cmd_set(target, val);
        } else if (input[0] == 'g' && input[1] == 'o' && input[2] == 'd') {
            const char* p = input + 3;
            while (*p == ' ') p++;
            const char* t_start = p;
            while (*p != ' ' && *p != '\0') p++;
            int val = parse_dec(p);
            char target[MAX_NAME_LEN];
            int len = (int)(p - t_start);
            if (len >= MAX_NAME_LEN) len = MAX_NAME_LEN - 1;
            for (int i = 0; i < len; i++) target[i] = t_start[i];
            target[len] = '\0';
            cmd_god_dispatch(target, val);
        } else if (local_strcmp(input, "meltdown") == 0) {
            cmd_meltdown();
        } else {
            g_api->kputs("Unknown command.\n", 0x55FF55);
        }
        
    }

    g_api->kputs("Trainer closed.\n", 0xAAAAAA);
    g_api->flush_buffer();
    return 0;
}

