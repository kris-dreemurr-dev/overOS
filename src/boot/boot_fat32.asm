; boot_fat32.asm - Stage 2: ЗАГРУЗКА ЯДРА ИЗ ФАЙЛА /sys/kernel.bin (boot from file, FAT32)
;
; Подключается из boot_stage2.asm директивой %include (один блок сборки, поэтому метки
; boot_drive, gdt_desc и print берутся оттуда). Содержит:
;   enable_a20, enter_unreal   - A20 и Unreal Mode (лимит ES = 4 ГБ в реальном режиме)
;   fat_load_kernel            - MBR -> раздел FAT32 -> BPB -> путь -> цепочка кластеров -> 0x100000
;   данные и сообщения загрузчика, DAP для INT 13h
;
; Вход:  fat_load_kernel (без параметров; DL уже сохранён в [boot_drive], DS=ES=0)
; Выход: ядро лежит в физической памяти с KERNEL_PHYS. При любой ошибке — сообщение и остановка.
; Путь к ядру: строка kernel_path (регистр не важен).

; ---- параметры загрузки ----
KERNEL_PHYS     equ 0x100000        ; куда кладём kernel.bin (физический адрес, как раньше)
KERNEL_MAX      equ 0xE00000        ; максимум 14 МиБ (как в BOOTOS). Можно поднять до 0x1D00000
                                    ; (29 МиБ): linker.ld ограничивает образ+.bss адресом 0x1E00000
BOUNCE          equ 0x7E00          ; буфер INT 13h: 16 секторов (8 КиБ), 0x7E00-0x9DFF
BOUNCE_SECTORS  equ 16
DIRBUF          equ 0x5000          ; сектор каталога      (0x5000-0x57FF свободно:
FATBUF          equ 0x5200          ; сектор таблицы FAT    stage 2 кончается на 0x4FFF,
BPB_BUF         equ 0x5400          ; первый сектор раздела  VBE-буферы 0x7000/0x8000
MBR_BUF         equ 0x5600          ; сектор 0 диска (MBR)   заполняются уже после загрузки)
BPB_BPS         equ BPB_BUF + 0x0B  ; байт на сектор (word)
BPB_SPC         equ BPB_BUF + 0x0D  ; секторов на кластер (byte)
BPB_RSVD        equ BPB_BUF + 0x0E  ; зарезервированных секторов (word)
BPB_NFATS       equ BPB_BUF + 0x10  ; число FAT (byte)
BPB_SPF         equ BPB_BUF + 0x24  ; секторов на FAT (dword, FAT32)
BPB_ROOT        equ BPB_BUF + 0x2C  ; кластер корневого каталога (dword)


; -----------------------------------------------------------------------------
; A20 и Unreal Mode
; -----------------------------------------------------------------------------
enable_a20:
    mov ax, 0x2401                  ; способ BIOS (результат не важен)
    int 0x15
    in al, 0x92                     ; "fast A20" gate
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE                    ; бит 0 = RESET системы, его не трогаем никогда
    out 0x92, al
.done:
    ret

; enter_unreal: (пере)включает Unreal Mode — лимит ES/FS = 4 ГБ при работе в реальном режиме.
; Вызывается перед КАЖДЫМ копированием выше 1 МБ: некоторые BIOS перезагружают сегментные
; регистры внутри INT 13h и сбрасывают лимит. Сохраняет все регистры общего назначения.
enter_unreal:
    push eax
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $+2
    mov ax, 0x10
    mov fs, ax
    mov es, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0x0000:.ok
.ok:
    xor ax, ax
    mov es, ax
    mov fs, ax
    mov ds, ax
    sti
    pop eax
    ret

; =============================================================================
; FAT32: boot from file — загрузка /sys/kernel.bin
;   MBR -> раздел FAT32 -> BPB -> обход пути по каталогам -> цепочка кластеров по FAT
;   -> INT 13h читает кластер в BOUNCE, затем копия выше 1 МБ через Unreal Mode.
; =============================================================================
fat_load_kernel:
    ; ---- 1. MBR: ищем раздел FAT32 (тип 0x0B или 0x0C) ----
    xor eax, eax
    mov bx, MBR_BUF
    call read_sector
    mov si, MBR_BUF + 446
    mov cx, 4
.part:
    mov al, [si + 4]
    cmp al, 0x0B
    je .found
    cmp al, 0x0C
    je .found
    add si, 16
    loop .part
    mov si, msg_fat_nopart
    jmp fat_fatal
.found:
    mov eax, [si + 8]               ; LBA начала раздела
    mov [part_lba], eax

    ; ---- 2. BPB = первый сектор раздела ----
    mov bx, BPB_BUF
    call read_sector
    cmp word [BPB_BUF + 510], 0xAA55
    jne .bad_bpb
    cmp word [BPB_BPS], 512
    jne .bad_bpb
    cmp byte [BPB_SPC], 0
    je .bad_bpb
    cmp dword [BPB_SPF], 0          ; у FAT12/16 это поле равно 0 -> не FAT32
    je .bad_bpb

    ; fat_lba  = начало раздела + зарезервированные секторы   (абсолютный LBA первой FAT)
    ; data_lba = fat_lba + число_FAT * секторов_на_FAT         (здесь начинается кластер №2)
    movzx eax, word [BPB_RSVD]
    add eax, [part_lba]
    mov [fat_lba], eax
    movzx ecx, byte [BPB_NFATS]
    imul ecx, [BPB_SPF]
    add eax, ecx
    mov [data_lba], eax

    ; ---- 3. путь -> первый кластер файла и размер ----
    mov si, kernel_path
    call walk_path                  ; EAX = первый кластер, EDX = размер в байтах
    jc .no_file
    cmp edx, KERNEL_MAX
    ja .too_big

    ; ---- 4. читаем цепочку кластеров и копируем в 0x100000 ----
    call load_file
    ret

.bad_bpb:
    mov si, msg_fat_bpb
    jmp fat_fatal
.too_big:
    mov si, msg_fat_big
    jmp fat_fatal
.no_file:
    mov si, msg_fat_nofile
    call print
    mov si, kernel_path
    jmp fat_fatal

fat_truncated:
    mov si, msg_fat_trunc
    jmp fat_fatal
fat_disk_fail:
    mov si, msg_fat_disk
fat_fatal:
    call print
    cli
.halt:
    hlt
    jmp .halt

; walk_path: разбор пути вида "/sys/kernel.bin"
;   in : DS:SI -> путь (строка с нулём в конце)
;   out: CF=0 -> EAX = первый кластер последнего компонента, EDX = его размер
;        CF=1 -> какой-то компонент не найден
walk_path:
    mov eax, [BPB_ROOT]             ; начинаем с корневого каталога
.component:
    cmp byte [si], '/'
    jne .parse
    inc si                          ; пропускаем разделители '/'
    jmp .component
.parse:
    cmp byte [si], 0
    je .end                         ; путь закончился
    call name_to_83                 ; name83 <- следующий компонент, SI -> его конец
    push si
    mov si, name83
    call find_entry                 ; ищем его в каталоге EAX
    pop si
    jc .fail
    jmp .component                  ; EAX — теперь каталог для следующего компонента
.end:
    clc
    ret
.fail:
    stc
    ret

; name_to_83: компонент пути по DS:SI -> 11-байтное имя 8.3 (заглавные, добито пробелами) в name83.
;   После вызова SI указывает на '/' или на завершающий 0. EAX не меняется.
;   (FAT хранит короткие имена в верхнем регистре, поэтому поиск не зависит от регистра.)
name_to_83:
    push di
    push cx
    push bx
    mov di, name83
    mov cx, 11
.pad:
    mov byte [di], ' '
    inc di
    loop .pad
    mov di, name83                  ; DI — куда писать следующий символ
    mov cx, 8                       ; сколько места осталось в текущей части (имя, затем расширение)
.chr:
    mov bl, [si]
    test bl, bl
    jz .done
    cmp bl, '/'
    je .done
    inc si
    cmp bl, '.'
    jne .plain
    mov di, name83 + 8              ; '.' -> переходим к расширению
    mov cx, 3
    jmp .chr
.plain:
    cmp bl, 'a'
    jb .store
    cmp bl, 'z'
    ja .store
    sub bl, 0x20                    ; в верхний регистр
.store:
    test cx, cx
    jz .chr                         ; часть заполнена: лишние символы отбрасываем
    mov [di], bl
    inc di
    dec cx
    jmp .chr
.done:
    pop bx
    pop cx
    pop di
    ret

; find_entry: поиск 8.3-имени в каталоге
;   in : EAX = первый кластер каталога, DS:SI -> 11-байтное имя
;   out: CF=0 найдено -> EAX = первый кластер записи, EDX = размер файла
;        CF=1 не найдено
find_entry:
    mov [fe_name], si
.next_cluster:
    mov [fe_clu], eax
    call cluster_to_lba             ; EAX = LBA первого сектора кластера
    movzx ecx, byte [BPB_SPC]       ; секторов осталось в этом кластере
.next_sector:
    mov bx, DIRBUF
    call read_sector                ; сектор EAX -> DS:BX (все регистры сохраняются)
    mov di, DIRBUF
    mov dx, 16                      ; 512 / 32 записей в секторе
.entry:
    mov bl, [di]
    test bl, bl
    jz .not_found                   ; 0x00 = больше записей в каталоге нет
    cmp bl, 0xE5
    je .skip                        ; удалено
    cmp byte [di + 0x0B], 0x0F
    je .skip                        ; фрагмент длинного имени (LFN)
    push di
    push ecx
    mov si, [fe_name]
    mov cx, 11
    repe cmpsb                      ; сравниваем [DS:SI] и [ES:DI], 11 байт (ES = 0)
    pop ecx                         ; (pop флаги не трогает)
    pop di
    je .found
.skip:
    add di, 32
    dec dx
    jnz .entry
    inc eax                         ; следующий сектор кластера
    dec ecx
    jnz .next_sector
    mov eax, [fe_clu]               ; кластер закончился -> идём по цепочке FAT
    call next_cluster
    jnc .next_cluster
.not_found:
    stc
    ret
.found:
    movzx eax, word [di + 0x14]     ; первый кластер, старшие 16 бит
    shl eax, 16
    mov ax, [di + 0x1A]             ; первый кластер, младшие 16 бит
    mov edx, [di + 0x1C]            ; размер файла
    clc
    ret

; load_file: загрузить файл по цепочке кластеров в KERNEL_PHYS (1 МиБ и выше)
;   in : EAX = первый кластер, EDX = размер в байтах
;   За один раз читается до 16 секторов (кусок кластера) в BOUNCE и сразу копируется вверх.
load_file:
    add edx, 511
    shr edx, 9                      ; секторов = ceil(размер / 512)
    mov [secs_left], edx
    mov dword [hi_dest], KERNEL_PHYS
.cluster:
    mov [lf_clu], eax
    call cluster_to_lba             ; EAX = LBA первого сектора кластера
    movzx ecx, byte [BPB_SPC]       ; секторов осталось в кластере
.chunk:
    cmp dword [secs_left], 0
    je .done
    mov edx, ecx                    ; n = min(осталось в кластере, осталось в файле, 16)
    cmp edx, BOUNCE_SECTORS
    jbe .n1
    mov edx, BOUNCE_SECTORS
.n1:
    cmp edx, [secs_left]
    jbe .n2
    mov edx, [secs_left]
.n2:
    mov bx, BOUNCE
    call read_chunk                 ; EAX = LBA, BX = куда, DX = сколько секторов
    call copy_up                    ; DX секторов из BOUNCE -> [hi_dest]
    add eax, edx
    sub ecx, edx
    sub [secs_left], edx
    test ecx, ecx
    jnz .chunk                      ; в кластере ещё есть секторы
    cmp dword [secs_left], 0
    je .done
    mov eax, [lf_clu]
    call next_cluster               ; следующий кластер файла
    jnc .cluster
    jmp fat_truncated               ; цепочка кончилась, а файл ещё не закончился
.done:
    ret

; copy_up: скопировать DX секторов (<= 16) из BOUNCE в физический адрес [hi_dest] и сдвинуть его.
;   Перед копированием заново включается Unreal Mode. Сохраняет все регистры.
copy_up:
    pushad
    call enter_unreal
    cli                             ; между включением Unreal Mode и копией прерывания не нужны
    mov edi, [hi_dest]
    mov esi, BOUNCE
    movzx ecx, dx
    shl ecx, 7                      ; секторов * 512 / 4 = двойных слов
    a32 rep movsd                   ; DS:ESI -> ES:EDI (лимит ES = 4 ГБ)
    mov [hi_dest], edi
    sti
    popad
    ret

; cluster_to_lba: EAX = номер кластера -> EAX = абсолютный LBA его первого сектора
cluster_to_lba:
    push ecx
    sub eax, 2
    movzx ecx, byte [BPB_SPC]
    imul eax, ecx
    add eax, [data_lba]
    pop ecx
    ret

; next_cluster: EAX = кластер -> EAX = следующий кластер цепочки FAT
;   CF=1, если цепочка закончилась (EAX >= 0x0FFFFFF8)
next_cluster:
    push ebx
    push edx
    mov edx, eax
    shr eax, 7                      ; 128 записей FAT32 в секторе
    add eax, [fat_lba]
    cmp eax, [fat_cached]
    je .cached
    mov [fat_cached], eax
    mov bx, FATBUF
    call read_sector
.cached:
    and edx, 127
    mov eax, [FATBUF + edx * 4]
    and eax, 0x0FFFFFFF             ; FAT32 использует только 28 бит
    cmp eax, 0x0FFFFFF8
    cmc                             ; CF=1, когда EAX >= 0x0FFFFFF8
    pop edx
    pop ebx
    ret

; read_sector: прочитать ОДИН сектор (INT 13h ext) в DS:BX.  in: EAX = абсолютный LBA
;   Сохраняет все регистры. При ошибке диска — сообщение и остановка.
read_sector:
    push dx
    mov dx, 1
    call read_chunk
    pop dx
    ret

; read_chunk: прочитать DX секторов (1..16) в DS:BX.  in: EAX = абсолютный LBA
;   Сохраняет все регистры.
read_chunk:
    pushad
    mov [dap_lba], eax
    mov [dap_off], bx
    mov [dap_cnt], dx
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc fat_disk_fail
    popad
    ret

; ---- данные FAT32-загрузчика ----
; --- FAT32 loader ---
part_lba:        dd 0               ; LBA начала раздела FAT32 (из MBR)
fat_lba:         dd 0               ; LBA первой FAT
data_lba:        dd 0               ; LBA кластера №2
fat_cached:      dd 0xFFFFFFFF      ; LBA сектора FAT, который сейчас лежит в FATBUF
fe_name:         dw 0
fe_clu:          dd 0
lf_clu:          dd 0
secs_left:       dd 0               ; сколько секторов файла осталось прочитать
hi_dest:         dd 0               ; следующий свободный адрес выше 1 МиБ
name83:          times 11 db ' '

kernel_path:     db "/sys/kernel.bin", 0   ; <- путь к ядру (регистр не важен)

msg_fat_load:    db "[ fat ] Loading /sys/kernel.bin ...", 0
msg_fat_nopart:  db 13, 10, "[ fat ] ERROR: no FAT32 partition in MBR", 0
msg_fat_bpb:     db 13, 10, "[ fat ] ERROR: bad FAT32 boot sector (BPB)", 0
msg_fat_nofile:  db 13, 10, "[ fat ] ERROR: file not found: ", 0
msg_fat_big:     db 13, 10, "[ fat ] ERROR: kernel.bin is too big (max 14 MiB)", 0
msg_fat_trunc:   db 13, 10, "[ fat ] ERROR: kernel.bin truncated (broken FAT chain)", 0
msg_fat_disk:    db 13, 10, "[ fat ] ERROR: disk read failed", 0

align 4
dap:
    db 0x10
    db 0
dap_cnt:
    dw 1                ; сколько секторов читать (ставит read_chunk)
dap_off:
    dw BOUNCE           ; смещение буфера
dap_seg:
    dw 0x0000           ; сегмент буфера
dap_lba:
    dq 0                ; LBA (ставит read_chunk)
