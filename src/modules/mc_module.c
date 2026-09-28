#include "../drivers/api.h"
#include <stdint.h>

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t mc_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "MC_CONSOLE_SYS",
    .flags = 0
};

static const devos_api_t* g_api = 0;
static uint8_t mc_code[512];
static int mc_code_len = 0;
static const char hex_digits[] = "0123456789ABCDEF";

static int local_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void print_hex8(uint8_t val, uint32_t color) {
    g_api->kputc(hex_digits[(val >> 4) & 0x0F], color);
    g_api->kputc(hex_digits[val & 0x0F], color);
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse_code_blocks(const char* str) {
    int i = 0;
    int blocks_added = 0;

    while (str[i] != '\0') {
        while (str[i] == ' ') i++;
        if (str[i] == '\0') break;

        int start = i;
        while (str[i] != ' ' && str[i] != '\0') i++;
        int len = i - start;

        int token_start = start;
        if (len >= 3 && str[start] == '0' && (str[start + 1] == 'x' || str[start + 1] == 'X')) {
            token_start += 2;
            len -= 2;
        }

        uint8_t byte = 0;
        int valid = 0;

        if (len == 2) {
            int h1 = hex_to_int(str[token_start]);
            int h2 = hex_to_int(str[token_start + 1]);
            if (h1 >= 0 && h2 >= 0) {
                byte = (uint8_t)((h1 << 4) | h2);
                valid = 1;
            }
        }

        if (!valid) {
            g_api->kputs("Err format! Use HEX (e.g. 90, CC, 0x90)\n", 0xFF5555);
            continue;
        }

        if (mc_code_len >= (int)sizeof(mc_code)) break;

        mc_code[mc_code_len++] = byte;
        blocks_added++;
    }

    if (blocks_added > 0) {
        g_api->kputs("Bytes loaded successfully.\n", 0x55FF55);
    }
}

static void dump_mc_buffer(void) {
    if (mc_code_len == 0) {
        g_api->kputs("Buffer is empty.\n", 0xAAAAAA);
        return;
    }
    g_api->kputs("--- MC Buffer Dump ---\n", 0x55FFFF);
    for (int i = 0; i < mc_code_len; i++) {
        print_hex8(mc_code[i], 0xFFFF55);
        g_api->kputc(' ', 0xFFFFFF);
    }
    g_api->kputc('\n', 0xFFFFFF);
}

static void run_machine_code(void) {
    if (mc_code_len == 0) {
        g_api->kputs("Nothing to run.\n", 0xAAAAAA);
        return;
    }
    g_api->kputs("Executing machine code in Ring 0...\n", 0x55FF55);
    g_api->flush_buffer();
    
    // Прямой вызов введённого бинарного буфера
    void (*exec_ptr)(void) = (void (*)(void))mc_code;
    exec_ptr();
    
    g_api->kputs("\nExecution finished.\n", 0x55FF55);
}

int driver_entry(const devos_api_t* api) {
    g_api = api;
    mc_code_len = 0;

    g_api->kputs("\n=== devOS Machine Console (MC.SYS) ===\n", 0x55FFFF);
    g_api->kputs("Commands: run, dump, reset, exit\n\n", 0xAAAAAA);
    g_api->flush_buffer();

    char input[128];
    while (1) {
        g_api->kputs("mc# ", 0xFFFF55);
        g_api->flush_buffer();

        if (!g_api->readline(input, sizeof(input))) continue;

        if (local_strcmp(input, "exit") == 0) {
            break;
        } else if (local_strcmp(input, "dump") == 0) {
            dump_mc_buffer();
        } else if (local_strcmp(input, "run") == 0) {
            run_machine_code();
        } else if (local_strcmp(input, "reset") == 0) {
            mc_code_len = 0;
            g_api->kputs("Buffer cleared.\n", 0x55FF55);
        } else {
            parse_code_blocks(input);
        }
    }

    g_api->kputs("Exiting Machine Console...\n\n", 0xAAAAAA);
    g_api->flush_buffer();
    return 0;
}