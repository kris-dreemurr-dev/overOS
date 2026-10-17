; ap_boot.asm — трамплин рабочих ядер (AP) для smp.c.
;
; Ядро копирует блок [ap_tr_start, ap_tr_end) по физическому адресу 0x8000 и
; рассылает INIT-SIPI-SIPI с вектором 0x08. Каждое проснувшееся ядро проходит
; 16 -> 32 -> 64 бит на таблицах страниц ядра, берёт себе номер (lock xadd),
; ставит свой стек и прыгает в ap_main(номер) в высокой половине.
;
; Регистры управления (CR0, CR4, EFER) не придумываются здесь, а копируются у
; основного ядра: smp.c записывает их в поля ниже перед рассылкой. Так у
; рабочих ядер те же NX, SSE и WP, что и у основного.
;
; Адреса внутри блока считаются от 0x8000 (TR), потому что исполняется он не
; там, где его положил компоновщик.

%define TR(x) ((x) - ap_tr_start + 0x8000)

section .rodata
global ap_tr_start, ap_tr_end
global ap_tr_cr0, ap_tr_cr3, ap_tr_cr4, ap_tr_efer
global ap_tr_stack_base, ap_tr_stack_size, ap_tr_entry, ap_tr_counter, ap_tr_max

[BITS 16]
ap_tr_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [TR(gdt32_desc)]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:TR(ap_pm32)

[BITS 32]
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [TR(ap_tr_cr4)]          ; PAE, OSFXSR, ... как у основного ядра
    mov cr4, eax
    mov eax, [TR(ap_tr_cr3)]          ; PML4 ядра (ниже 4 ГБ)
    mov cr3, eax
    mov ecx, 0xC0000080               ; EFER: LME (+NXE, если есть у основного)
    mov eax, [TR(ap_tr_efer)]
    mov edx, [TR(ap_tr_efer) + 4]
    wrmsr
    mov eax, [TR(ap_tr_cr0)]          ; PG|PE|MP|NE|WP — включает пейджинг
    mov cr0, eax
    lgdt [TR(gdt64_desc)]
    jmp 0x08:TR(ap_lm64)

[BITS 64]
default abs                           ; адреса блока абсолютные (0x8xxx), не от RIP
ap_lm64:
    xor eax, eax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, 1
    lock xadd [TR(ap_tr_counter)], eax
    mov edi, eax                      ; номер этого ядра
    cmp eax, [TR(ap_tr_max)]
    jae .park                         ; ядер больше, чем слотов: лишнее спит
    lea rax, [rdi + 1]
    imul rax, [TR(ap_tr_stack_size)]
    add rax, [TR(ap_tr_stack_base)]
    mov rsp, rax
    mov rax, [TR(ap_tr_entry)]
    call rax                          ; ap_main(номер) не возвращается
.park:
    cli
    hlt
    jmp .park

align 8
gdt32_start:
    dq 0
    dq 0x00CF9A000000FFFF             ; 0x08: код 32
    dq 0x00CF92000000FFFF             ; 0x10: данные 32
gdt32_desc:
    dw gdt32_desc - gdt32_start - 1
    dd TR(gdt32_start)

align 8
gdt64_start:
    dq 0
    dq 0x00209A0000000000             ; 0x08: код 64, Ring 0
    dq 0x0000920000000000             ; 0x10: данные 64
gdt64_desc:
    dw gdt64_desc - gdt64_start - 1
    dq TR(gdt64_start)

align 8
ap_tr_cr0:        dd 0
ap_tr_cr3:        dd 0
ap_tr_cr4:        dd 0
ap_tr_max:        dd 0
ap_tr_efer:       dq 0
ap_tr_stack_base: dq 0
ap_tr_stack_size: dq 0
ap_tr_entry:      dq 0
ap_tr_counter:    dd 0
ap_tr_end:
