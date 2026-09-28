#include "redactor/redactor.h"

const devos_api_t* g_redactor_api = 0;

int module_main(const devos_api_t* api, const char* args) {
    g_redactor_api = api;
    run_editor(args);
    return 0;
}

__attribute__((section(".header"), used))
const sys_header_t sys_hdr = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = (int (*)(const devos_api_t*))module_main
};