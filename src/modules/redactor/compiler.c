#include "redactor.h"

// Предварительное объявление внешней функции редактора
extern int get_tab_file_content(const char* filename, char* out_buf, int max_len);

jit_guard_t g_jit_guard = { 0, 0, 0 };
uint8_t jit_buffer[16384] __attribute__((aligned(4096)));
static int jit_idx = 0;
char string_pool[4096];
int string_pool_idx = 0;
char compile_error_msg[64] = "";
static int compile_failed = 0;
static int g_target_mode = TARGET_JIT;

void set_compiler_target(int target) {
    g_target_mode = target;
}

void set_compiler_target_sys(int is_sys) {
    g_target_mode = is_sys ? TARGET_SYS : TARGET_JIT;
}

static void jit_dummy_console_op(void) {
    return;
}

static int my_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void to_fat83(const char* in, char* out83) {
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    out83[11] = '\0';
    int i = 0;
    while (*in && *in != '.' && i < 8) { char c = *in++; if (c >= 'a' && c <= 'z') c -= 32; out83[i++] = c; }
    while (*in && *in != '.') in++; if (*in == '.') in++; int j = 8;
    while (*in && j < 11) { char c = *in++; if (c >= 'a' && c <= 'z') c -= 32; out83[j++] = c; }
}

static int helper_jit_open(const char* filename) {
    if (!fat16_is_mounted()) return -1;
    fat16_go_root();
    char name83[12];
    to_fat83(filename, name83);
    uint8_t dummy[16];
    int r = fat16_read_file(name83, dummy, 1);
    return (r > 0) ? 1 : -1;
}

static uint64_t resolve_kernel_symbol(const char* name) {
    if (g_target_mode == TARGET_PRG) return 0;

    if (g_target_mode == TARGET_JIT) {
        if (my_strcmp(name, "console_save_state") == 0 || 
            my_strcmp(name, "console_restore_state") == 0) {
            return (uint64_t)jit_dummy_console_op;
        }
    }

    if (!g_redactor_api || !g_redactor_api->kernel_symbols) return 0;
    const ksym_api_t* syms = g_redactor_api->kernel_symbols;
    for (int i = 0; syms[i].name != 0; i++) {
        if (my_strcmp(syms[i].name, name) == 0) {
            return syms[i].addr;
        }
    }
    return 0;
}

static void helper_print_num(int val) {
    if (val == 0) { kputc('0', 0x00FFFFFF); return; }
    if (val < 0) { kputc('-', 0x00FFFFFF); val = -val; }
    char temp[16]; int t = 0;
    while (val > 0) { temp[t++] = '0' + (val % 10); val /= 10; }
    for (int j = t - 1; j >= 0; j--) kputc(temp[j], 0x00FFFFFF);
}

static void helper_print_hex(uint64_t val) {
    kputs("0x", 0x00AAAAAA); char hex_chars[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) kputc(hex_chars[(val >> (i * 4)) & 0xF], 0x00FFFF55);
}

static char scancode_to_char(uint8_t sc, int shift) {
    static const char norm[128] = {
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
      '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
       '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
    };
    static const char shft[128] = {
        0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
      '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0,
        '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0, '*',   0, ' '
    };
    if (sc >= 128) return 0;
    return shift ? shft[sc] : norm[sc];
}

static int kbd_shift_state = 0;

static int helper_get_char(void) {
    flush_buffer(); static int ext_key = 0;
    while (1) {
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            if (status & 0x20) continue;
            if (sc == 0xE0) { ext_key = 1; continue; }
            if (ext_key) { ext_key = 0; continue; }
            if (sc == 0x2A || sc == 0x36) { kbd_shift_state = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { kbd_shift_state = 0; continue; }
            if (sc & 0x80) continue;
            if (sc == 0x01) return 27; 
            if (sc == 0x1C) { kputc('\n', 0x00FFFFFF); flush_buffer(); return '\n'; }
            if (sc == 0x0E) return '\b'; 
            char c = scancode_to_char(sc, kbd_shift_state);
            if (c >= 32 && c <= 126) { kputc(c, 0x00FFFFFF); flush_buffer(); return (int)c; }
        }
        sleep_ms(10);
    }
}

static inline void emit_byte(uint8_t b) { if (jit_idx < (int)sizeof(jit_buffer)) jit_buffer[jit_idx++] = b; }
static inline void emit_dword(uint32_t dw) {
    emit_byte((uint8_t)(dw & 0xFF)); emit_byte((uint8_t)((dw >> 8) & 0xFF));
    emit_byte((uint8_t)((dw >> 16) & 0xFF)); emit_byte((uint8_t)((dw >> 24) & 0xFF));
}
static inline void emit_qword(uint64_t qw) {
    emit_dword((uint32_t)(qw & 0xFFFFFFFF));
    emit_dword((uint32_t)(qw >> 32));
}
static inline void emit_mov_rax_imm64(uint64_t imm) {
    emit_byte(0x48); emit_byte(0xB8); emit_qword(imm);
}

typedef struct { char name[32]; uint32_t val; } macro_t;
static macro_t macros[64];
static int macro_count = 0;
static int find_macro(const char* name, uint32_t* out_val) {
    for (int i = 0; i < macro_count; i++) {
        if (my_strcmp(macros[i].name, name) == 0) { *out_val = macros[i].val; return 1; }
    }
    return 0;
}

typedef struct { char field_name[32]; int offset; } field_t;
static field_t struct_fields[128];
static int field_count = 0;
static int find_field_offset(const char* field_name) {
    for (int i = 0; i < field_count; i++) {
        if (my_strcmp(struct_fields[i].field_name, field_name) == 0) return struct_fields[i].offset;
    }
    return 0;
}

typedef struct { char name[32]; int stack_offset; int size; } var_t;
static var_t vars[64];
static int var_count = 0;
static int current_stack_size = 0;

static int find_var_size(const char* name) {
    for (int i = 0; i < var_count; i++) { if (my_strcmp(vars[i].name, name) == 0) return vars[i].size; }
    return 8;
}

static int find_var(const char* name) {
    for (int i = 0; i < var_count; i++) { if (my_strcmp(vars[i].name, name) == 0) return vars[i].stack_offset; }
    return 0;
}

static int find_or_add_var_typed(const char* name, int size) {
    int voff = find_var(name); if (voff != 0) return voff;
    if (var_count < 64) {
        var_count++; 
        int alloc_sz = (size < 8) ? 8 : ((size + 7) & ~7);
        current_stack_size += alloc_sz;
        
        vars[var_count - 1].stack_offset = -current_stack_size;
        vars[var_count - 1].size = size;
        int i = 0; while (name[i] && i < 31) { vars[var_count - 1].name[i] = name[i]; i++; }
        vars[var_count - 1].name[i] = '\0';
        return -current_stack_size;
    }
    return -8;
}
static int find_or_add_var(const char* name) { return find_or_add_var_typed(name, 8); }

typedef struct { char name[32]; int offset; int arg_count; } user_func_t;
static user_func_t user_funcs[32];
static int user_func_count = 0;
static int main_entry_offset = -1;

static int find_user_func(const char* name) {
    for (int i = 0; i < user_func_count; i++) { if (my_strcmp(user_funcs[i].name, name) == 0) return i; }
    return -1;
}

static int func_return_slots[32];
static int func_return_count = 0;

#define MAX_LOOP_DEPTH 8
#define MAX_BREAKS 16
typedef struct { int break_slots[MAX_BREAKS]; int break_count; int continue_target; } loop_ctx_t;
static loop_ctx_t loop_stack[MAX_LOOP_DEPTH];
static int loop_depth = 0;

#define MAX_CASES 32
#define MAX_SWITCH_DEPTH 4
typedef struct { int case_vals[MAX_CASES]; int case_targets[MAX_CASES]; int case_count; int default_target; int dispatch_jmp_slot; } switch_ctx_t;
static switch_ctx_t switch_stack[MAX_SWITCH_DEPTH];
static int switch_depth = 0;

static inline int is_ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static void skip_ws_and_comments(const char** p) {
    while (**p) {
        if (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') { (*p)++; } 
        else if (**p == '/' && *(*p + 1) == '/') { *p += 2; while (**p && **p != '\n') (*p)++; } 
        else if (**p == '/' && *(*p + 1) == '*') { *p += 2; while (**p && !(**p == '*' && *(*p + 1) == '/')) (*p)++; if (**p) *p += 2; } 
        else { break; }
    }
}

static int str_starts_with(const char* str, const char* prefix) {
    while (*prefix) { if (*str++ != *prefix++) return 0; } return 1;
}

static int parse_ident(const char** p, char* out, int max_len) {
    skip_ws_and_comments(p); int len = 0;
    if ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || **p == '_') {
        while (is_ident_char(**p) && len < max_len - 1) { out[len++] = *(*p)++; }
        out[len] = '\0'; return 1;
    }
    return 0;
}

static uint32_t parse_literal(const char** p) {
    skip_ws_and_comments(p); uint32_t val = 0;
    if (**p == '\'') {
        (*p)++;
        if (**p == '\\') {
            (*p)++;
            if (**p == 'n') val = '\n'; else if (**p == 't') val = '\t';
            else if (**p == 'r') val = '\r'; else if (**p == '0') val = 0; else val = (uint8_t)**p;
        } else { val = (uint8_t)**p; }
        (*p)++; if (**p == '\'') (*p)++; return val;
    }
    if (**p == '0' && (*(*p + 1) == 'x' || *(*p + 1) == 'X')) {
        *p += 2;
        while ((**p >= '0' && **p <= '9') || (**p >= 'a' && **p <= 'f') || (**p >= 'A' && **p <= 'F')) {
            val <<= 4;
            if (**p >= '0' && **p <= '9') val |= (**p - '0');
            else if (**p >= 'a' && **p <= 'f') val |= (**p - 'a' + 10); else val |= (**p - 'A' + 10);
            (*p)++;
        }
        return val;
    }
    while (**p >= '0' && **p <= '9') { val = val * 10 + (**p - '0'); (*p)++; }
    return val;
}

static int match_type_prefix(const char** p) {
    skip_ws_and_comments(p); const char* probe = *p; int element_size = 0;
    if (str_starts_with(probe, "volatile") && !is_ident_char(probe[8])) { probe += 8; skip_ws_and_comments(&probe); }
    if (str_starts_with(probe, "static") && !is_ident_char(probe[6]))   { probe += 6; skip_ws_and_comments(&probe); }
    if (str_starts_with(probe, "const") && !is_ident_char(probe[5]))    { probe += 5; skip_ws_and_comments(&probe); }

    if (str_starts_with(probe, "uint8_t") && !is_ident_char(probe[7])) { element_size = 1; probe += 7; }
    else if (str_starts_with(probe, "char") && !is_ident_char(probe[4])) { element_size = 1; probe += 4; }
    else if (str_starts_with(probe, "uint16_t") && !is_ident_char(probe[8])) { element_size = 2; probe += 8; }
    else if (str_starts_with(probe, "short") && !is_ident_char(probe[5])) { element_size = 2; probe += 5; }
    else if (str_starts_with(probe, "uint32_t") && !is_ident_char(probe[8])) { element_size = 4; probe += 8; }
    else if (str_starts_with(probe, "int") && !is_ident_char(probe[3])) { element_size = 8; probe += 3; }
    else if (str_starts_with(probe, "void") && !is_ident_char(probe[4])) { element_size = 8; probe += 4; }
    else if (str_starts_with(probe, "struct") && !is_ident_char(probe[6])) {
        probe += 6; skip_ws_and_comments(&probe);
        char sname[32]; parse_ident(&probe, sname, 32);
        element_size = 8;
    }

    if (element_size > 0) {
        skip_ws_and_comments(&probe);
        while (*probe == '*') { probe++; skip_ws_and_comments(&probe); element_size = 8; }
        *p = probe; return element_size;
    }
    return 0;
}

static int parse_cast_size(const char** p) {
    const char* probe = *p; skip_ws_and_comments(&probe);
    if (*probe == '(') {
        probe++; skip_ws_and_comments(&probe); int size = 0;
        if (str_starts_with(probe, "volatile")) { probe += 8; skip_ws_and_comments(&probe); }
        if (str_starts_with(probe, "uint8_t") || str_starts_with(probe, "char")) { size = 1; probe += (probe[0]=='c'?4:7); }
        else if (str_starts_with(probe, "uint16_t") || str_starts_with(probe, "short")) { size = 2; probe += (probe[0]=='s'?5:8); }
        else if (str_starts_with(probe, "uint32_t") || str_starts_with(probe, "int")) { size = 8; probe += (probe[0]=='i'?3:8); }
        if (size > 0) {
            skip_ws_and_comments(&probe);
            while (*probe == '*') { probe++; skip_ws_and_comments(&probe); size = 8; }
            if (*probe == ')') { probe++; *p = probe; return size; }
        }
    }
    return 8;
}

static void compile_expr(const char** p);
static void compile_push_operand(const char** p);

static int compile_call(const char** p, const char* name) {
    if (**p == '(') (*p)++; uint32_t argc = 0;
    while (**p && **p != ')') {
        skip_ws_and_comments(p); if (**p == ')') break;
        compile_push_operand(p); argc++; skip_ws_and_comments(p); if (**p == ',') (*p)++;
    }
    if (**p == ')') (*p)++;

    // 1. Системные вызовы INT 0x80 для программ Ring 3 (.PRG)
    if (g_target_mode == TARGET_PRG) {
        int is_sys_call = 0;
        uint32_t call_num = 0;

        if (my_strcmp(name, "exit") == 0)             { is_sys_call = 1; call_num = 0; }
        else if (my_strcmp(name, "print") == 0)        { is_sys_call = 1; call_num = 1; }
        else if (my_strcmp(name, "print_color") == 0)  { is_sys_call = 1; call_num = 1; }
        else if (my_strcmp(name, "put_pixel") == 0)    { is_sys_call = 1; call_num = 2; }
        else if (my_strcmp(name, "flush") == 0)        { is_sys_call = 1; call_num = 3; }
        else if (my_strcmp(name, "sleep") == 0)        { is_sys_call = 1; call_num = 4; }
        else if (my_strcmp(name, "clear") == 0)        { is_sys_call = 1; call_num = 5; }
        else if (my_strcmp(name, "get_key") == 0)      { is_sys_call = 1; call_num = 6; }
        else if (my_strcmp(name, "print_num") == 0)    { is_sys_call = 1; call_num = 7; }
        else if (my_strcmp(name, "open") == 0)         { is_sys_call = 1; call_num = 10; }
        else if (my_strcmp(name, "read") == 0)         { is_sys_call = 1; call_num = 11; }
        else if (my_strcmp(name, "close") == 0)        { is_sys_call = 1; call_num = 12; }
        else if (my_strcmp(name, "set_palette") == 0)  { is_sys_call = 1; call_num = 20; }
        else if (my_strcmp(name, "blit_frame") == 0)   { is_sys_call = 1; call_num = 21; }

        if (is_sys_call) {
            if (argc == 1) {
                emit_byte(0x5B); // pop rbx
            } else if (argc == 2) {
                emit_byte(0x59); // pop rcx
                emit_byte(0x5B); // pop rbx
            } else if (argc >= 3) {
                emit_byte(0x5A); // pop rdx
                emit_byte(0x59); // pop rcx
                emit_byte(0x5B); // pop rbx
                if (argc > 3) {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)((argc - 3) * 8));
                }
            }

            emit_byte(0xB8); emit_dword(call_num); 
            emit_byte(0xCD); emit_byte(0x80);     
            return 1;
        }
    }

    // 2. Встроенные системные функции для тестирования через JIT (F5)
    if (g_target_mode == TARGET_JIT) {
        if (my_strcmp(name, "exit") == 0) {
            if (argc > 0) { emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)(argc * 8)); }
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); 
            emit_byte(0x5D);                   
            emit_byte(0xC3);                   
            return 1;
        }
        if (my_strcmp(name, "open") == 0) {
            emit_mov_rax_imm64((uint64_t)&helper_jit_open);
            emit_byte(0xFF); emit_byte(0xD0);
            if (argc > 0) { emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)(argc * 8)); }
            return 1;
        }
        if (my_strcmp(name, "close") == 0 || my_strcmp(name, "read") == 0 ||
            my_strcmp(name, "set_palette") == 0 || my_strcmp(name, "blit_frame") == 0) {
            if (argc > 0) { emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)(argc * 8)); }
            emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0); 
            return 1;
        }
    }

    // 3. Встроенный хелпер чтения символа
    if (my_strcmp(name, "get_char") == 0) {
        emit_mov_rax_imm64((uint64_t)&helper_get_char);
        emit_byte(0xFF); emit_byte(0xD0);
        if (argc > 0) { emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)(argc * 8)); }
        return 1;
    }

    // 4. Пользовательские функции внутри модуля
    int uidx = find_user_func(name);
    if (uidx >= 0) {
        emit_byte(0xE8); emit_dword((uint32_t)(user_funcs[uidx].offset - (jit_idx + 4)));
        if (argc > 0) { emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)(argc * 8)); }
        return 1;
    }

    // 5. Вызовы ядра (System V ABI: аргументы в rdi, rsi, rdx, rcx, r8, r9)
    uint64_t kaddr = resolve_kernel_symbol(name);
    if (kaddr) {
        if (argc == 1) {
            emit_byte(0x5F); // pop rdi
        } else if (argc == 2) {
            emit_byte(0x5E); // pop rsi
            emit_byte(0x5F); // pop rdi
        } else if (argc == 3) {
            emit_byte(0x5A); // pop rdx
            emit_byte(0x5E); // pop rsi
            emit_byte(0x5F); // pop rdi
        } else if (argc == 4) {
            emit_byte(0x59); // pop rcx
            emit_byte(0x5A); // pop rdx
            emit_byte(0x5E); // pop rsi
            emit_byte(0x5F); // pop rdi
        } else if (argc >= 5) {
            if (argc >= 6) { emit_byte(0x41); emit_byte(0x51); } // pop r9
            if (argc >= 5) { emit_byte(0x41); emit_byte(0x50); } // pop r8
            emit_byte(0x59); // pop rcx
            emit_byte(0x5A); // pop rdx
            emit_byte(0x5E); // pop rsi
            emit_byte(0x5F); // pop rdi
            if (argc > 6) {
                emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)((argc - 6) * 8));
            }
        }
        emit_mov_rax_imm64(kaddr);
        emit_byte(0xFF); emit_byte(0xD0);
        return 1;
    }

    // 6. Указатели на функции
    int v_off = find_var(name);
    if (v_off != 0) {
        emit_byte(0x48); emit_byte(0x8B); emit_byte(0x45); emit_byte((uint8_t)v_off);
        emit_byte(0xFF); emit_byte(0xD0);
        if (argc > 0) { emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte((uint8_t)(argc * 8)); }
        return 1;
    }

    compile_failed = 1; int k = 0; const char* pref = "undefined function: ";
    while (pref[k]) { compile_error_msg[k] = pref[k]; k++; }
    int m = 0; while (name[m] && k < 60) compile_error_msg[k++] = name[m++];
    compile_error_msg[k] = '\0';
    return 0;
}

static void compile_factor(const char** p) {
    skip_ws_and_comments(p);

    if (str_starts_with(*p, "sizeof") && !is_ident_char((*p)[6])) {
        *p += 6; skip_ws_and_comments(p); int size = 8;
        if (**p == '(') {
            (*p)++; skip_ws_and_comments(p);
            if (str_starts_with(*p, "uint8_t") || str_starts_with(*p, "char")) size = 1;
            else if (str_starts_with(*p, "uint16_t") || str_starts_with(*p, "short")) size = 2;
            else size = 8; 
            while (**p && **p != ')') (*p)++; if (**p == ')') (*p)++;
        }
        emit_byte(0x48); emit_byte(0xB8); emit_qword(size); return;
    }

    if (**p == '(') {
        const char* probe = *p + 1;
        if (match_type_prefix(&probe)) { skip_ws_and_comments(&probe); if (*probe == ')') { *p = probe + 1; skip_ws_and_comments(p); } }
    }

    if (**p == '(') {
        (*p)++; compile_expr(p); skip_ws_and_comments(p); if (**p == ')') (*p)++; return;
    } else if (**p == '*') {
        (*p)++; int access_size = parse_cast_size(p); compile_factor(p);
        if (access_size == 1) { emit_byte(0x0F); emit_byte(0xB6); emit_byte(0x00); } 
        else if (access_size == 2) { emit_byte(0x0F); emit_byte(0xB7); emit_byte(0x00); } 
        else { emit_byte(0x48); emit_byte(0x8B); emit_byte(0x00); } return;
    } else if (**p == '&' && *(*p + 1) != '&') {
        (*p)++; char vname[32]; parse_ident(p, vname, 32);
        int off = find_or_add_var(vname); emit_byte(0x48); emit_byte(0x8D); emit_byte(0x45); emit_byte((uint8_t)off);
    } else if (**p == '~') {
        (*p)++; compile_factor(p); emit_byte(0x48); emit_byte(0xF7); emit_byte(0xD0);
    } else if (**p == '!' && *(*p + 1) != '=') {
        (*p)++; compile_factor(p); emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
        emit_byte(0x0F); emit_byte(0x94); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
    } else if (**p == '-') {
        (*p)++; compile_factor(p); emit_byte(0x48); emit_byte(0xF7); emit_byte(0xD8);
    } else if ((**p >= '0' && **p <= '9') || **p == '\'') {
        uint32_t val = parse_literal(p); emit_mov_rax_imm64(val);
    } else if (is_ident_char(**p)) {
        char id[32]; parse_ident(p, id, 32); skip_ws_and_comments(p);
        if (**p == '(') { compile_call(p, id); return; }
        
        uint32_t mval = 0; if (find_macro(id, &mval)) { emit_mov_rax_imm64(mval); return; }

        int uidx = find_user_func(id);
        if (uidx >= 0) { emit_mov_rax_imm64((uint64_t)(jit_buffer + user_funcs[uidx].offset)); return; }
        uint64_t kaddr = resolve_kernel_symbol(id);
        if (kaddr) { emit_mov_rax_imm64(kaddr); return; }

        int off = find_or_add_var(id);
        int access_size = find_var_size(id);

        skip_ws_and_comments(p);
        if (**p == '.') {
            (*p)++; char fname[32]; parse_ident(p, fname, 32);
            int f_off = find_field_offset(fname); off += f_off;
            if (str_starts_with(fname, "low") || str_starts_with(fname, "byte")) access_size = 1;
            else if (str_starts_with(fname, "word") || str_starts_with(fname, "short")) access_size = 2;
            else access_size = 8;
        }

        skip_ws_and_comments(p);
        if (**p == '[') {
            (*p)++;
            emit_byte(0x48); emit_byte(0x8B); emit_byte(0x45); emit_byte((uint8_t)off);
            emit_byte(0x50);
            compile_expr(p);
            skip_ws_and_comments(p); if (**p == ']') (*p)++;

            if (access_size == 2) { emit_byte(0x48); emit_byte(0xD1); emit_byte(0xE0); }
            else if (access_size == 4) { emit_byte(0x48); emit_byte(0xC1); emit_byte(0xE0); emit_byte(0x02); }
            else if (access_size == 8) { emit_byte(0x48); emit_byte(0xC1); emit_byte(0xE0); emit_byte(0x03); }

            emit_byte(0x59); emit_byte(0x48); emit_byte(0x01); emit_byte(0xC8);

            if (access_size == 1) { emit_byte(0x0F); emit_byte(0xB6); emit_byte(0x00); }
            else if (access_size == 2) { emit_byte(0x0F); emit_byte(0xB7); emit_byte(0x00); }
            else { emit_byte(0x48); emit_byte(0x8B); emit_byte(0x00); }
        } else {
            if (access_size == 1) {
                emit_byte(0x0F); emit_byte(0xB6); emit_byte(0x45); emit_byte((uint8_t)off);
            } else if (access_size == 2) {
                emit_byte(0x0F); emit_byte(0xB7); emit_byte(0x45); emit_byte((uint8_t)off);
            } else {
                emit_byte(0x48); emit_byte(0x8B); emit_byte(0x45); emit_byte((uint8_t)off);
            }
        }
    }
}

static void compile_logical_or(const char** p);

static void compile_multiplicative(const char** p) {
    compile_factor(p); skip_ws_and_comments(p);
    while (**p == '*' || **p == '/' || **p == '%') {
        char op = *(*p)++; emit_byte(0x50); compile_factor(p);
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xC1); emit_byte(0x58);
        if (op == '*') { emit_byte(0x48); emit_byte(0x0F); emit_byte(0xAF); emit_byte(0xC1); } 
        else if (op == '/') { emit_byte(0x48); emit_byte(0x99); emit_byte(0x48); emit_byte(0xF7); emit_byte(0xF9); } 
        else if (op == '%') { emit_byte(0x48); emit_byte(0x99); emit_byte(0x48); emit_byte(0xF7); emit_byte(0xF9); emit_byte(0x48); emit_byte(0x89); emit_byte(0xD0); }
        skip_ws_and_comments(p);
    }
}

static void compile_additive(const char** p) {
    compile_multiplicative(p); skip_ws_and_comments(p);
    while (**p == '+' || **p == '-') {
        char op = *(*p)++; emit_byte(0x50); compile_multiplicative(p); emit_byte(0x59);
        if (op == '+') { emit_byte(0x48); emit_byte(0x01); emit_byte(0xC8); } 
        else { emit_byte(0x48); emit_byte(0x29); emit_byte(0xC1); emit_byte(0x48); emit_byte(0x89); emit_byte(0xC8); }
        skip_ws_and_comments(p);
    }
}

static void compile_shift(const char** p) {
    compile_additive(p); skip_ws_and_comments(p);
    while ((**p == '<' && *(*p + 1) == '<') || (**p == '>' && *(*p + 1) == '>')) {
        char op = **p; *p += 2; emit_byte(0x50); compile_additive(p);
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xC1); emit_byte(0x58);
        if (op == '<') { emit_byte(0x48); emit_byte(0xD3); emit_byte(0xE0); } else { emit_byte(0x48); emit_byte(0xD3); emit_byte(0xE8); }
        skip_ws_and_comments(p);
    }
}

static void compile_relational(const char** p) {
    compile_shift(p); skip_ws_and_comments(p);
    while ((**p == '<' || **p == '>') && *(*p + 1) != '<' && *(*p + 1) != '>') {
        char op1 = *(*p)++; int is_eq = (**p == '='); if (is_eq) (*p)++;
        emit_byte(0x50); compile_shift(p); emit_byte(0x59); emit_byte(0x48); emit_byte(0x39); emit_byte(0xC1);
        if (op1 == '<' && !is_eq) { emit_byte(0x0F); emit_byte(0x9C); emit_byte(0xC0); }
        else if (op1 == '<' && is_eq) { emit_byte(0x0F); emit_byte(0x9E); emit_byte(0xC0); }
        else if (op1 == '>' && !is_eq) { emit_byte(0x0F); emit_byte(0x9F); emit_byte(0xC0); }
        else if (op1 == '>' && is_eq) { emit_byte(0x0F); emit_byte(0x9D); emit_byte(0xC0); }
        emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0); skip_ws_and_comments(p);
    }
}

static void compile_equality(const char** p) {
    compile_relational(p); skip_ws_and_comments(p);
    while ((**p == '=' && *(*p + 1) == '=') || (**p == '!' && *(*p + 1) == '=')) {
        int is_not = (**p == '!'); *p += 2; emit_byte(0x50); compile_relational(p);
        emit_byte(0x59); emit_byte(0x48); emit_byte(0x39); emit_byte(0xC1);
        if (!is_not) { emit_byte(0x0F); emit_byte(0x94); emit_byte(0xC0); } else { emit_byte(0x0F); emit_byte(0x95); emit_byte(0xC0); }
        emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0); skip_ws_and_comments(p);
    }
}

static void compile_bitwise_and(const char** p) {
    compile_equality(p); skip_ws_and_comments(p);
    while (**p == '&' && *(*p + 1) != '&') {
        (*p)++; emit_byte(0x50); compile_equality(p); emit_byte(0x59); emit_byte(0x48); emit_byte(0x21); emit_byte(0xC8); skip_ws_and_comments(p);
    }
}

static void compile_bitwise_xor(const char** p) {
    compile_bitwise_and(p); skip_ws_and_comments(p);
    while (**p == '^') {
        (*p)++; emit_byte(0x50); compile_bitwise_and(p); emit_byte(0x59); emit_byte(0x48); emit_byte(0x31); emit_byte(0xC8); skip_ws_and_comments(p);
    }
}

static void compile_bitwise_or(const char** p) {
    compile_bitwise_xor(p); skip_ws_and_comments(p);
    while (**p == '|' && *(*p + 1) != '|') {
        (*p)++; emit_byte(0x50); compile_bitwise_xor(p); emit_byte(0x59); emit_byte(0x48); emit_byte(0x09); emit_byte(0xC8); skip_ws_and_comments(p);
    }
}

static void compile_logical_and(const char** p) {
    compile_bitwise_or(p); skip_ws_and_comments(p);
    while (**p == '&' && *(*p + 1) == '&') {
        *p += 2; emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0x84);
        int jz_slot = jit_idx; emit_dword(0);
        compile_bitwise_or(p); emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0x95); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
        emit_byte(0xE9); int jmp_slot = jit_idx; emit_dword(0);
        *(uint32_t*)&jit_buffer[jz_slot] = (uint32_t)(jit_idx - (jz_slot + 4)); emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0);
        *(uint32_t*)&jit_buffer[jmp_slot] = (uint32_t)(jit_idx - (jmp_slot + 4)); skip_ws_and_comments(p);
    }
}

static void compile_logical_or(const char** p) {
    compile_logical_and(p); skip_ws_and_comments(p);
    while (**p == '|' && *(*p + 1) == '|') {
        *p += 2; emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0x85);
        int jnz_slot = jit_idx; emit_dword(0);
        compile_logical_and(p); emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0x95); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
        emit_byte(0xE9); int jmp_slot = jit_idx; emit_dword(0);
        *(uint32_t*)&jit_buffer[jnz_slot] = (uint32_t)(jit_idx - (jnz_slot + 4)); emit_mov_rax_imm64(1);
        *(uint32_t*)&jit_buffer[jmp_slot] = (uint32_t)(jit_idx - (jmp_slot + 4)); skip_ws_and_comments(p);
    }
}

static void compile_expr(const char** p) { compile_logical_or(p); }

static void compile_push_operand(const char** p) {
    skip_ws_and_comments(p);
    if (**p == '"') {
        (*p)++; char* str_start = &string_pool[string_pool_idx];
        int pool_offset = string_pool_idx;
        while (**p && **p != '"' && string_pool_idx < (int)sizeof(string_pool) - 2) {
            if (**p == '\\' && *(*p + 1) == 'n') { string_pool[string_pool_idx++] = '\n'; *p += 2; } 
            else { string_pool[string_pool_idx++] = **p; (*p)++; }
        }
        string_pool[string_pool_idx++] = '\0'; if (**p == '"') (*p)++;

        uint64_t final_addr;
        if (g_target_mode == TARGET_PRG) {
            final_addr = PRG_LOAD_BASE + 0x2000 + pool_offset;
        } else {
            final_addr = (uint64_t)str_start;
        }

        emit_mov_rax_imm64(final_addr);
        emit_byte(0x50); // push rax
    } else { compile_expr(p); emit_byte(0x50); }
}

static void compile_step_expr(const char** p) {
    skip_ws_and_comments(p); const char* save_p = *p; char id[32];
    if (parse_ident(p, id, 32)) {
        skip_ws_and_comments(p);
        if (**p == '+' && *(*p + 1) == '+') {
            *p += 2; int off = find_or_add_var(id); emit_byte(0x48); emit_byte(0xFF); emit_byte(0x45); emit_byte((uint8_t)off); return;
        } else if (**p == '-' && *(*p + 1) == '-') {
            *p += 2; int off = find_or_add_var(id); emit_byte(0x48); emit_byte(0xFF); emit_byte(0x4D); emit_byte((uint8_t)off); return;
        } else if (**p == '+' && *(*p + 1) == '=') {
            *p += 2; int off = find_or_add_var(id); compile_expr(p); emit_byte(0x48); emit_byte(0x01); emit_byte(0x45); emit_byte((uint8_t)off); return;
        } else if (**p == '-' && *(*p + 1) == '=') {
            *p += 2; int off = find_or_add_var(id); compile_expr(p); emit_byte(0x48); emit_byte(0x29); emit_byte(0x45); emit_byte((uint8_t)off); return;
        } else if (**p == '=' && *(*p + 1) != '=') {
            (*p)++; int off = find_or_add_var(id); compile_expr(p); emit_byte(0x48); emit_byte(0x89); emit_byte(0x45); emit_byte((uint8_t)off); return;
        }
    }
    *p = save_p; compile_expr(p);
}

static void compile_statement(const char** p) {
    skip_ws_and_comments(p);
    if (!**p) return;

    if (**p == '{') {
        (*p)++; while (**p) { skip_ws_and_comments(p); if (**p == '}') { (*p)++; break; } compile_statement(p); }
        return;
    }

    if (str_starts_with(*p, "__asm__") || str_starts_with(*p, "asm")) {
        while (is_ident_char(**p)) (*p)++; skip_ws_and_comments(p);
        if (**p == '(') {
            (*p)++; skip_ws_and_comments(p);
            if (**p == '"') {
                (*p)++;
                if (str_starts_with(*p, "cli")) emit_byte(0xFA); else if (str_starts_with(*p, "sti")) emit_byte(0xFB);
                else if (str_starts_with(*p, "hlt")) emit_byte(0xF4); else if (str_starts_with(*p, "nop")) emit_byte(0x90);
                else if (str_starts_with(*p, "ret")) emit_byte(0xC3);
                while (**p && **p != '"') (*p)++; if (**p == '"') (*p)++;
            }
            skip_ws_and_comments(p); if (**p == ')') (*p)++;
        }
        skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
    }

    if (str_starts_with(*p, "return") && !is_ident_char((*p)[6])) {
        *p += 6; skip_ws_and_comments(p);
        if (**p != ';') { compile_expr(p); } else { emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0); }
        skip_ws_and_comments(p); if (**p == ';') (*p)++;
        emit_byte(0xE9); int slot = jit_idx; emit_dword(0);
        if (func_return_count < 32) func_return_slots[func_return_count++] = slot;
        return;
    }

    if (str_starts_with(*p, "break") && !is_ident_char((*p)[5])) {
        *p += 5; skip_ws_and_comments(p); if (**p == ';') (*p)++;
        if (loop_depth > 0 && loop_stack[loop_depth - 1].break_count < MAX_BREAKS) {
            emit_byte(0xE9); int slot = jit_idx; emit_dword(0);
            loop_stack[loop_depth - 1].break_slots[loop_stack[loop_depth - 1].break_count++] = slot;
        }
        return;
    }

    if (str_starts_with(*p, "continue") && !is_ident_char((*p)[8])) {
        *p += 8; skip_ws_and_comments(p); if (**p == ';') (*p)++;
        int target = -1;
        for (int i = loop_depth - 1; i >= 0; i--) { if (loop_stack[i].continue_target != -1) { target = loop_stack[i].continue_target; break; } }
        if (target != -1) { emit_byte(0xE9); emit_dword((uint32_t)(target - (jit_idx + 4))); }
        return;
    }

    if (str_starts_with(*p, "while") && !is_ident_char((*p)[5])) {
        *p += 5; skip_ws_and_comments(p); if (**p == '(') (*p)++;
        int cond_pos = jit_idx; compile_expr(p); skip_ws_and_comments(p); if (**p == ')') (*p)++;
        emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0x84); int exit_slot = jit_idx; emit_dword(0);
        int current_loop = loop_depth;
        if (loop_depth < MAX_LOOP_DEPTH) { loop_stack[loop_depth].break_count = 0; loop_stack[loop_depth].continue_target = cond_pos; loop_depth++; }
        compile_statement(p);
        emit_byte(0xE9); emit_dword((uint32_t)(cond_pos - (jit_idx + 4)));
        *(uint32_t*)&jit_buffer[exit_slot] = (uint32_t)(jit_idx - (exit_slot + 4));
        for (int b = 0; b < loop_stack[current_loop].break_count; b++) {
            int bslot = loop_stack[current_loop].break_slots[b]; *(uint32_t*)&jit_buffer[bslot] = (uint32_t)(jit_idx - (bslot + 4));
        }
        if (loop_depth > 0) loop_depth--; return;
    }

    if (str_starts_with(*p, "if") && !is_ident_char((*p)[2])) {
        *p += 2; skip_ws_and_comments(p); if (**p == '(') (*p)++; compile_expr(p); skip_ws_and_comments(p); if (**p == ')') (*p)++;
        emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0); emit_byte(0x0F); emit_byte(0x84); int jz_slot = jit_idx; emit_dword(0);
        compile_statement(p); skip_ws_and_comments(p);
        if (str_starts_with(*p, "else") && !is_ident_char((*p)[4])) {
            *p += 4; emit_byte(0xE9); int jmp_slot = jit_idx; emit_dword(0);
            *(uint32_t*)&jit_buffer[jz_slot] = (uint32_t)(jit_idx - (jz_slot + 4));
            compile_statement(p); *(uint32_t*)&jit_buffer[jmp_slot] = (uint32_t)(jit_idx - (jmp_slot + 4));
        } else { *(uint32_t*)&jit_buffer[jz_slot] = (uint32_t)(jit_idx - (jz_slot + 4)); }
        return;
    }

    const char* save_p = *p; int t_sz = match_type_prefix(p);
    if (t_sz > 0) {
        char vname[32]; skip_ws_and_comments(p);
        if (parse_ident(p, vname, 32)) {
            int off = find_or_add_var_typed(vname, t_sz); skip_ws_and_comments(p);
            if (**p == '=') { (*p)++; compile_expr(p); emit_byte(0x48); emit_byte(0x89); emit_byte(0x45); emit_byte((uint8_t)off); }
            skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
        }
    }
    *p = save_p;

    if (str_starts_with(*p, "print") && !is_ident_char((*p)[5])) {
        *p += 5; skip_ws_and_comments(p); if (**p == '(') (*p)++; skip_ws_and_comments(p);
        if (**p == '"') {
            (*p)++; char* str_start = &string_pool[string_pool_idx];
            int pool_offset = string_pool_idx;
            while (**p && **p != '"' && string_pool_idx < (int)sizeof(string_pool) - 2) {
                if (**p == '\\' && *(*p + 1) == 'n') { string_pool[string_pool_idx++] = '\n'; *p += 2; } 
                else { string_pool[string_pool_idx++] = **p; (*p)++; }
            }
            string_pool[string_pool_idx++] = '\0'; if (**p == '"') (*p)++;

            skip_ws_and_comments(p);
            uint32_t text_color = 0x00FFFFFF;
            if (**p == ',') {
                (*p)++;
                skip_ws_and_comments(p);
                text_color = parse_literal(p);
            }

            if (g_target_mode == TARGET_PRG) {
                uint64_t final_addr = PRG_LOAD_BASE + 0x2000 + pool_offset;
                emit_mov_rax_imm64(final_addr);
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xC3); // mov rbx, rax
                emit_byte(0xB9); emit_dword(text_color);          // mov ecx, color
                emit_byte(0xB8); emit_dword(1);                   // mov eax, 1
                emit_byte(0xCD); emit_byte(0x80);                 // int 0x80
            } else {
                emit_mov_rax_imm64((uint64_t)str_start);
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xC7); // mov rdi, rax
                emit_byte(0x48); emit_byte(0xBE); emit_qword(text_color); // mov rsi, color
                emit_mov_rax_imm64((uint64_t)&kputs);
                emit_byte(0xFF); emit_byte(0xD0);                 // call rax
                emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte(0x08);
            }
        }
        skip_ws_and_comments(p); if (**p == ')') (*p)++; skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
    }

    if (str_starts_with(*p, "print_num") && !is_ident_char((*p)[9])) {
        *p += 9; skip_ws_and_comments(p); if (**p == '(') (*p)++; compile_expr(p); skip_ws_and_comments(p); if (**p == ')') (*p)++; skip_ws_and_comments(p); if (**p == ';') (*p)++;
        
        if (g_target_mode == TARGET_PRG) {
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xC3);  // mov rbx, rax
            emit_byte(0xB8); emit_dword(7);                     // mov eax, 7
            emit_byte(0xCD); emit_byte(0x80);                   // int 0x80
        } else {
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xC7); // mov rdi, rax
            emit_mov_rax_imm64((uint64_t)&helper_print_num);
            emit_byte(0xFF); emit_byte(0xD0);
        }
        return;
    }

    if (str_starts_with(*p, "print_hex") && !is_ident_char((*p)[9])) {
        *p += 9; skip_ws_and_comments(p); if (**p == '(') (*p)++; compile_expr(p); skip_ws_and_comments(p); if (**p == ')') (*p)++; skip_ws_and_comments(p); if (**p == ';') (*p)++;
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xC7); // mov rdi, rax
        emit_mov_rax_imm64((uint64_t)&helper_print_hex);
        emit_byte(0xFF); emit_byte(0xD0);
        return;
    }

    char ident[48];
    if (parse_ident(p, ident, 48)) {
        skip_ws_and_comments(p);
        if (**p == '(') { compile_call(p, ident); skip_ws_and_comments(p); if (**p == ';') (*p)++; return; }

        int off = find_or_add_var(ident);
        int access_size = find_var_size(ident);

        skip_ws_and_comments(p);
        if (**p == '=' && *(*p + 1) != '=') {
            (*p)++; compile_expr(p);
            if (access_size == 1) {
                emit_byte(0x88); emit_byte(0x45); emit_byte((uint8_t)off);
            } else if (access_size == 2) {
                emit_byte(0x66); emit_byte(0x89); emit_byte(0x45); emit_byte((uint8_t)off);
            } else {
                emit_byte(0x48); emit_byte(0x89); emit_byte(0x45); emit_byte((uint8_t)off);
            }
            skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
        } else if (**p == '+' && *(*p + 1) == '=') {
            *p += 2; compile_expr(p); emit_byte(0x48); emit_byte(0x01); emit_byte(0x45); emit_byte((uint8_t)off); skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
        } else if (**p == '-' && *(*p + 1) == '=') {
            *p += 2; compile_expr(p); emit_byte(0x48); emit_byte(0x29); emit_byte(0x45); emit_byte((uint8_t)off); skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
        } else if (**p == '+' && *(*p + 1) == '+') {
            *p += 2; emit_byte(0x48); emit_byte(0xFF); emit_byte(0x45); emit_byte((uint8_t)off); skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
        } else if (**p == '-' && *(*p + 1) == '-') {
            *p += 2; emit_byte(0x48); emit_byte(0xFF); emit_byte(0x4D); emit_byte((uint8_t)off); skip_ws_and_comments(p); if (**p == ';') (*p)++; return;
        }
    }
    if (**p == ';') (*p)++; else (*p)++;
}

static void preprocess_source(const char* src, char* out, int max_len) {
    const char* p = src; int out_idx = 0; static char inc_file_buf[4096];
    char included_files[32][32]; int included_count = 0;

    while (*p && out_idx < max_len - 1) {
        const char* probe = p; while (*probe == ' ' || *probe == '\t') probe++;

        if (*probe == '#') {
            probe++; while (*probe == ' ' || *probe == '\t') probe++;
            if (str_starts_with(probe, "pragma")) { while (*probe && *probe != '\n') probe++; if (*probe == '\n') probe++; p = probe; continue; }
            if (str_starts_with(probe, "include")) {
                probe += 7; while (*probe == ' ' || *probe == '\t') probe++; char quote = *probe;
                if (quote == '"' || quote == '<') {
                    probe++; char inc_name[32]; int nlen = 0; char end_quote = (quote == '<') ? '>' : '"';
                    while (*probe && *probe != end_quote && *probe != '\n' && *probe != '\r' && nlen < 31) { inc_name[nlen++] = *probe++; }
                    inc_name[nlen] = '\0'; if (*probe == end_quote) probe++;

                    int already_included = 0;
                    for (int f = 0; f < included_count; f++) { if (my_strcmp(included_files[f], inc_name) == 0) { already_included = 1; break; } }
                    if (already_included) { while (*probe && *probe != '\n') probe++; if (*probe == '\n') probe++; p = probe; continue; }

                    if (included_count < 32) {
                        int ci = 0; while (inc_name[ci] && ci < 31) { included_files[included_count][ci] = inc_name[ci]; ci++; }
                        included_files[included_count][ci] = '\0'; included_count++;
                    }

                    int loaded = get_tab_file_content(inc_name, inc_file_buf, sizeof(inc_file_buf));
                    if (!loaded && fat16_is_mounted()) {
                        char name83[12]; to_fat83(inc_name, name83); fat16_go_root(); fat16_change_dir("CODE       ");
                        int r = fat16_read_file(name83, inc_file_buf, sizeof(inc_file_buf) - 1); if (r > 0) { inc_file_buf[r] = '\0'; loaded = 1; }
                    }

                    if (loaded) {
                        int k = 0; while (inc_file_buf[k] && out_idx < max_len - 1) { out[out_idx++] = inc_file_buf[k++]; }
                        if (out_idx < max_len - 1) out[out_idx++] = '\n';
                    }
                    while (*probe && *probe != '\n') probe++; if (*probe == '\n') probe++; p = probe; continue;
                }
            }
        }
        out[out_idx++] = *p++;
    }
    out[out_idx] = '\0';
}

int get_jit_code_size(void) {
    return jit_idx;
}

int compile_source_to_x86(const char* raw_src) {
    static char src[32768]; preprocess_source(raw_src, src, sizeof(src));
    jit_idx = 0; string_pool_idx = 0; compile_failed = 0; compile_error_msg[0] = '\0';
    macro_count = 0; field_count = 0; user_func_count = 0; main_entry_offset = -1;
    loop_depth = 0; switch_depth = 0;

    emit_byte(0xE9); int entry_jmp_slot = jit_idx; emit_dword(0);

    const char* p = src;
    while (*p) {
        skip_ws_and_comments(&p); if (!*p) break;
        if (*p == '#') {
            p++; skip_ws_and_comments(&p);
            if (str_starts_with(p, "define")) {
                p += 6; char mname[32];
                if (parse_ident(&p, mname, 32)) {
                    skip_ws_and_comments(&p); uint32_t val = 0;
                    if (*p == '(' || (*p >= '0' && *p <= '9') || *p == '\'') { val = parse_literal(&p); }
                    if (macro_count < 64) {
                        int mi = 0; while (mname[mi] && mi < 31) { macros[macro_count].name[mi] = mname[mi]; mi++; }
                        macros[macro_count].name[mi] = '\0'; macros[macro_count].val = val; macro_count++;
                    }
                }
            }
            while (*p && *p != '\n') p++; continue;
        }
        p++;
    }

    p = src;
    while (*p) {
        skip_ws_and_comments(&p); if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (str_starts_with(p, "extern") && !is_ident_char(p[6])) { while (*p && *p != ';') p++; if (*p == ';') p++; continue; }

        const char* probe = p;
        if (match_type_prefix(&probe)) {
            char func_name[32];
            if (parse_ident(&probe, func_name, 32)) {
                skip_ws_and_comments(&probe);
                if (*probe == '(') {
                    probe++; char param_names[8][32]; int param_sizes[8]; int param_count = 0;
                    while (*probe && *probe != ')') {
                        skip_ws_and_comments(&probe); if (*probe == ')') break;
                        int p_sz = match_type_prefix(&probe);
                        if (p_sz > 0) {
                            char pname[32];
                            if (parse_ident(&probe, pname, 32)) {
                                int pi = 0; while (pname[pi] && pi < 31) { param_names[param_count][pi] = pname[pi]; pi++; }
                                param_names[param_count][pi] = '\0'; param_sizes[param_count] = p_sz; param_count++;
                            }
                        }
                        skip_ws_and_comments(&probe); if (*probe == ',') probe++;
                    }
                    if (*probe == ')') probe++; skip_ws_and_comments(&probe);

                    if (*probe == ';') { p = probe + 1; continue; }
                    if (*probe == '{') {
                        p = probe; int is_main = (my_strcmp(func_name, "main") == 0); int func_start = jit_idx;
                        if (is_main) { main_entry_offset = func_start; } 
                        else {
                            if (user_func_count < 32) {
                                int fi = 0; while (func_name[fi] && fi < 31) { user_funcs[user_func_count].name[fi] = func_name[fi]; fi++; }
                                user_funcs[user_func_count].name[fi] = '\0'; user_funcs[user_func_count].offset = func_start;
                                user_funcs[user_func_count].arg_count = param_count; user_func_count++;
                            }
                        }

                        // 64-bit function prologue (push rbp; mov rbp, rsp; sub rsp, ...)
                        emit_byte(0x55);                               // push rbp
                        emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5); // mov rbp, rsp
                        emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC); int sub_esp_slot = jit_idx; emit_dword(0);
                        var_count = 0; current_stack_size = 0; func_return_count = 0;

                        // System V ABI arguments mapping (rdi, rsi, rdx, rcx, r8, r9) to stack offsets
                        for (int i = 0; i < param_count && i < 6; i++) {
                            int off = find_or_add_var_typed(param_names[i], param_sizes[i]);
                            if (i == 0) { emit_byte(0x48); emit_byte(0x89); emit_byte(0x7D); emit_byte((uint8_t)off); } // mov [rbp+off], rdi
                            else if (i == 1) { emit_byte(0x48); emit_byte(0x89); emit_byte(0x75); emit_byte((uint8_t)off); } // mov [rbp+off], rsi
                            else if (i == 2) { emit_byte(0x48); emit_byte(0x89); emit_byte(0x55); emit_byte((uint8_t)off); } // mov [rbp+off], rdx
                            else if (i == 3) { emit_byte(0x48); emit_byte(0x89); emit_byte(0x4D); emit_byte((uint8_t)off); } // mov [rbp+off], rcx
                            else if (i == 4) { emit_byte(0x4C); emit_byte(0x89); emit_byte(0x45); emit_byte((uint8_t)off); } // mov [rbp+off], r8
                            else if (i == 5) { emit_byte(0x4C); emit_byte(0x89); emit_byte(0x4D); emit_byte((uint8_t)off); } // mov [rbp+off], r9
                        }

                        compile_statement(&p); if (compile_failed) return 0;

                        for (int r = 0; r < func_return_count; r++) { 
                            int rslot = func_return_slots[r]; 
                            *(uint32_t*)&jit_buffer[rslot] = (uint32_t)(jit_idx - (rslot + 4)); 
                        }

                        int aligned_stack = (current_stack_size + 15) & ~15; 
                        if (aligned_stack < 256) aligned_stack = 256;
                        *(uint32_t*)&jit_buffer[sub_esp_slot] = aligned_stack;

                        if (is_main && g_target_mode == TARGET_PRG) {
                            emit_byte(0x48); emit_byte(0x31); emit_byte(0xDB); // xor rbx, rbx
                            emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0); // xor rax, rax
                            emit_byte(0xCD); emit_byte(0x80);                 // int 0x80
                            emit_byte(0xF4);                                  // hlt
                        } else {
                            if (is_main) {
                                emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0); // xor rax, rax
                            }
                            emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
                            emit_byte(0x5D);                                  // pop rbp
                            emit_byte(0xC3);                                  // ret
                        }
                        continue;
                    }
                }
            }
        }
        p++;
    }

    if (main_entry_offset == -1) {
        compile_failed = 1; const char* err = "function 'main' not found!";
        int i = 0; while (err[i]) { compile_error_msg[i] = err[i]; i++; } compile_error_msg[i] = '\0'; return 0;
    }
    *(uint32_t*)&jit_buffer[entry_jmp_slot] = (uint32_t)(main_entry_offset - (entry_jmp_slot + 4));
    return 1;
}

void run_compiled_code(void) {
    clear_screen(0x000000); flush_buffer();
    static char full_src[EDITOR_MAX_LINES * EDITOR_MAX_COLS]; int pos = 0;
    for (int r = 0; r < EDITOR_MAX_LINES; r++) {
        for (int c = 0; text_buf[r][c] != '\0'; c++) { full_src[pos++] = text_buf[r][c]; } full_src[pos++] = '\n';
    }
    full_src[pos] = '\0';

    if (!compile_source_to_x86(full_src)) {
        kputs("[Compiler Error] ", 0xFF5555); kputs(compile_error_msg, 0xFFFF55);
        kputs("\n\nPress ANY key to return to editor...", 0xAAAAAA); flush_buffer(); sleep_ms(250);
        while (inb(0x64) & 1) inb(0x60);
        while (1) { if (inb(0x64) & 1) { uint8_t sc = inb(0x60); if (!(sc & 0x80)) break; } sleep_ms(10); } return;
    }

    kputs("=== devOS JIT: Execution Start ===\n\n", 0x55FF55); flush_buffer(); sleep_ms(150);
    kbd_shift_state = 0; while (inb(0x64) & 1) inb(0x60);

    __asm__ volatile ("movq %%rsp, %0\n\tmovq %%rbp, %1\n\t" : "=m"(g_jit_guard.saved_esp), "=m"(g_jit_guard.saved_ebp));
    g_jit_guard.is_running_jit = 1;

    void (*entry_func)(void) = (void (*)(void))jit_buffer; entry_func();

    g_jit_guard.is_running_jit = 0;
    flush_buffer(); kputs("\n=== Finished (Press ANY key) ===", 0x55FFFF); flush_buffer(); sleep_ms(250);
    while (inb(0x64) & 1) inb(0x60);
    while (1) { if (inb(0x64) & 1) { uint8_t sc = inb(0x60); if (!(sc & 0x80)) break; } sleep_ms(10); }
}