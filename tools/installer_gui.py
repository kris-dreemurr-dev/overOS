#!/usr/bin/env python3
import os
import sys
import time
import tty
import termios
import subprocess
import re

# 24-битная TrueColor палитра Windows XP Setup
BG_BLUE    = "\033[48;2;0;0;170m"
FG_WHITE   = "\033[38;2;255;255;255m"
FG_YELLOW  = "\033[38;2;255;255;85m"
FG_CYAN    = "\033[38;2;85;255;255m"
FG_GREEN   = "\033[38;2;85;255;85m"
FG_RED     = "\033[38;2;255;85;85m"
BAR_TOP    = "\033[48;2;170;170;170m\033[38;2;0;0;0m"
BAR_BOTTOM = "\033[48;2;170;170;170m\033[38;2;0;0;0m"
RESET      = "\033[0m"

IMG_PATH   = "build/os.img"
KERNEL_BIN = "build/kernel.bin"
ANSI_REGEX = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')

def visible_len(s):
    return len(ANSI_REGEX.sub('', s))

def get_os_version():
    search_files = ["src/kernel/config.h", "config.h", "src/kernel/kernel.h", "src/kernel/kernel.c"]
    for path in search_files:
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                    m = re.search(r'#define\s+(?:OS_VERSION|VERSION|KERNEL_VERSION|OS_VER)\s+["\']?([^"\'\r\n]+)["\']?', content)
                    if m: return m.group(1).strip()
                    m2 = re.search(r'overOS\s+(?:x86_64\s+)?([0-9]+\.[0-9]+(?:\.[0-9]+)?)', content)
                    if m2: return m2.group(1).strip()
            except Exception: pass
    return "0.0.1"

def get_available_modules():
    default_mods = ["mc", "redactor", "memedit", "stress", "gpu", "cpu", "hwinfo", "cloak"]
    if os.path.isdir("src/modules"):
        mods = []
        for f in sorted(os.listdir("src/modules")):
            if f.endswith("_module.c"):
                mods.append(f[:-9])
        if mods: return mods
    return default_mods

def get_available_programs():
    progs = []
    if os.path.isdir("src/programs"):
        for f in sorted(os.listdir("src/programs")):
            if f.endswith(".c"):
                progs.append(f[:-2])
    if not progs:
        progs = ["math", "psx", "fork", "test", "pkill"]
    return progs

def get_key():
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        ch = sys.stdin.read(1)
        if ch == '\x1b':
            ch2 = sys.stdin.read(1)
            if ch2 == '[':
                ch3 = sys.stdin.read(1)
                if ch3 == 'A': return 'UP'
                if ch3 == 'B': return 'DOWN'
                if ch3 in ('1', 'O'):
                    sys.stdin.read(1)
                    return 'F3'
            elif ch2 == 'O':
                sys.stdin.read(1)
                return 'F3'
            return 'ESC'
        elif ch in ('\r', '\n'): return 'ENTER'
        elif ch == ' ': return 'SPACE'
        elif ch in ('\x7f', '\x08'): return 'BACKSPACE'
        elif ch == '\x03': return 'F3'
        return ch
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)

def render_frame(title, content_lines, footer="ENTER=Выбрать   F3=Выход"):
    cols = os.get_terminal_size().columns
    rows = os.get_terminal_size().lines

    out = ["\033[?25l\033[H"]
    vis_title_len = visible_len(title) + 1
    pad_top = max(0, cols - vis_title_len)
    out.append(f"{BAR_TOP} {title}{' ' * pad_top}\033[K{RESET}\n")

    usable_rows = rows - 2
    body_count = len(content_lines)
    for i in range(usable_rows):
        if i < body_count:
            line = content_lines[i]
            vlen = visible_len(line) + 2
            pad = max(0, cols - vlen)
            out.append(f"{BG_BLUE}  {line}{' ' * pad}\033[K{RESET}\n")
        else:
            out.append(f"{BG_BLUE}{' ' * cols}\033[K{RESET}\n")

    vis_footer_len = visible_len(footer) + 1
    pad_bottom = max(0, cols - vis_footer_len)
    out.append(f"{BAR_BOTTOM} {footer}{' ' * pad_bottom}\033[K{RESET}")

    sys.stdout.write("".join(out))
    sys.stdout.flush()

def restore_terminal():
    sys.stdout.write("\033[?25h\033[0m")
    os.system("clear")

def get_usb_disks():
    disks = []
    try:
        out = subprocess.check_output(
            ["lsblk", "-b", "-d", "-n", "-p", "-o", "NAME,SIZE,MODEL,TRAN,RM,TYPE"],
            text=True
        )
        for line in out.strip().split("\n"):
            if not line: continue
            parts = line.split()
            if len(parts) >= 4:
                dev_name = parts[0]
                bytes_sz = int(parts[1]) if parts[1].isdigit() else 0
                size_mb = bytes_sz // (1024 * 1024)
                dev_type = parts[-1]
                line_lower = line.lower()
                if dev_type == "disk" and ("usb" in line_lower or " 1 " in line):
                    model = " ".join(parts[2:-3]) if len(parts) > 3 else "USB Storage"
                    h_sz = f"{size_mb / 1024:.1f} GB" if size_mb >= 1024 else f"{size_mb} MB"
                    disks.append({
                        "path": dev_name,
                        "size_mb": size_mb,
                        "human_size": h_sz,
                        "model": model.strip() or "Generic Flash Drive"
                    })
    except Exception: pass
    return disks

def unmount_device(dev_path):
    try:
        out = subprocess.check_output(["lsblk", "-n", "-p", "-o", "NAME", dev_path], text=True)
        for part in out.strip().split("\n"):
            part = part.strip()
            if part:
                subprocess.run(["umount", part], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    except Exception: pass

# ==============================================================================
# МАСТЕР ПОШАГОВОЙ УСТАНОВКИ (INSTALLATION WIZARD)
# ==============================================================================
def run_install_wizard():
    os_ver = get_os_version()
    avail_modules = get_available_modules()
    avail_programs = get_available_programs()

    # Состояние настроек
    wiz_config = {
        "target_disk": None,
        "size_mb": 512,
        "with_doom": True,
        "with_quake": True,
        "modules": {m: True for m in avail_modules},
        "programs": {p: True for p in avail_programs}
    }

    step = 1
    while True:
        # -------------------------------------------------------------
        # ШАГ 1: Приветствие
        # -------------------------------------------------------------
        if step == 1:
            welcome_lines = [
                f"{FG_WHITE}Здравствуйте! Вас приветствует мастер установки {FG_CYAN}overOS v{os_ver}{FG_WHITE}.",
                "",
                "Данная программа в пошаговом режиме подготовит загрузочный накопитель,",
                "соберет ядро и модули, а также настроит состав программ дистрибутива.",
                "",
                "Этапы установки:",
                "  1. Выбор целевого USB-накопителя",
                "  2. Ввод точного размера накопителя (в МБ) с проверкой ограничений",
                "  3. Поэлементный выбор модулей (.SYS) и программ (.PRG)",
                "  4. Проверка итоговой сводки параметров с возможностью возврата",
                "  5. Компиляция и запись на USB-диск",
                "",
                "-------------------------------------------------------------------------------",
                "  • Чтобы начать установку, нажмите ENTER.",
                "  • Для выхода в главное меню нажмите F3."
            ]
            render_frame(f"Мастер установки overOS v{os_ver} — Приветствие (Шаг 1 из 4)", welcome_lines, "ENTER=Далее   F3=Отмена")
            k = get_key()
            if k == 'ENTER': step = 2
            elif k in ('F3', 'ESC', 'q'): return

        # -------------------------------------------------------------
        # ШАГ 2: Выбор целевой флешки
        # -------------------------------------------------------------
        elif step == 2:
            cur = 0
            while True:
                disks = get_usb_disks()
                if not disks:
                    err_lines = [
                        f"{FG_YELLOW}USB-накопители не обнаружены!{FG_WHITE}",
                        "Вставьте USB-флешку в компьютер.",
                        "",
                        "ENTER=Повторить сканирование   F3=Отмена"
                    ]
                    render_frame(f"Мастер установки overOS v{os_ver} — Поиск диска", err_lines, "ENTER=Повторить   F3=Отмена")
                    if get_key() in ('F3', 'ESC', 'q'): return
                    continue

                menu = []
                for d in disks:
                    menu.append(("disk", d))
                menu.append(("back", "[ <- Назад в приветствие ]"))

                cur = max(0, min(cur, len(menu) - 1))
                lines = [
                    f"{FG_WHITE}Выберите целевой USB-накопитель для установки:",
                    "Программа автоматически определит доступный физический объем диска.",
                    "",
                    "-------------------------------------------------------------------------------"
                ]
                for i, (mtype, d) in enumerate(menu):
                    ptr = "->" if i == cur else "  "
                    clr = FG_YELLOW if i == cur else FG_WHITE
                    if mtype == "disk":
                        lines.append(f"{clr}{ptr} [{d['path']}]  Объем: {d['human_size']:<8} ({d['size_mb']} МБ)  Модель: {d['model']}{FG_WHITE}")
                    else:
                        lines.append(f"{clr}{ptr} {d}{FG_WHITE}")

                lines.extend([
                    "-------------------------------------------------------------------------------",
                    f"{FG_RED}ВНИМАНИЕ: Все данные на выбранном диске будут безвозвратно удалены!{FG_WHITE}"
                ])

                render_frame(f"Мастер установки overOS v{os_ver} — Выбор флешки (Шаг 1 из 4)", lines, "СТРЕЛКИ=Выбор   ENTER=Далее   F3=Отмена")
                k = get_key()
                if k == 'UP': cur = (cur - 1) % len(menu)
                elif k == 'DOWN': cur = (cur + 1) % len(menu)
                elif k == 'ENTER':
                    mtype, d = menu[cur]
                    if mtype == "back":
                        step = 1
                        break
                    else:
                        wiz_config["target_disk"] = d
                        # Если емкость флешки меньше 512 МБ, адаптируем дефолт
                        if d["size_mb"] < wiz_config["size_mb"] and d["size_mb"] >= 32:
                            wiz_config["size_mb"] = d["size_mb"]
                        step = 3
                        break
                elif k in ('F3', 'ESC', 'q'): return

        # -------------------------------------------------------------
        # ШАГ 3: Ввод размера накопителя ЧИСЛОМ (с валидацией)
        # -------------------------------------------------------------
        elif step == 3:
            disk = wiz_config["target_disk"]
            input_val = str(wiz_config["size_mb"])
            error_msg = ""

            while True:
                lines = [
                    f"{FG_WHITE}Укажите размер системного раздела FAT32 для записи на флешку.",
                    f"Выбран диск: {FG_CYAN}{disk['path']}{FG_WHITE} ({disk['model']}, Емкость: {FG_YELLOW}{disk['size_mb']} МБ{FG_WHITE})",
                    "",
                    "Введите размер раздела ЧИСЛОМ (в мегабайтах) с клавиатуры:",
                    "",
                    f"   {FG_YELLOW}> [ {input_val} ] МБ{FG_WHITE}",
                    "",
                    "-------------------------------------------------------------------------------",
                    f"Параметры и ограничения:",
                    f"  • Минимальный размер:   {FG_CYAN}32 МБ{FG_WHITE} (технический минимум для FAT32)",
                    f"  • Рекомендуемый размер: {FG_CYAN}512 МБ{FG_WHITE} (оптимально для всех игр и программ)",
                    f"  • Физический лимит:     {FG_YELLOW}{disk['size_mb']} МБ{FG_WHITE} (объем выбранной флешки)",
                    "-------------------------------------------------------------------------------"
                ]

                if error_msg:
                    lines.append(f"{FG_RED}[!] {error_msg}{FG_WHITE}")
                else:
                    lines.append(f"{FG_CYAN}Наберите число и нажмите ENTER для подтверждения (ESC=Назад).{FG_WHITE}")

                render_frame(f"Мастер установки overOS v{os_ver} — Размер накопителя (Шаг 2 из 4)", lines, "ЦИФРЫ=Ввод   BACKSPACE=Стереть   ENTER=Принять   ESC=Назад")
                k = get_key()

                if k in ('0', '1', '2', '3', '4', '5', '6', '7', '8', '9'):
                    if len(input_val) < 7:
                        input_val += k
                        error_msg = ""
                elif k == 'BACKSPACE':
                    input_val = input_val[:-1]
                    error_msg = ""
                elif k in ('ESC', 'F3'):
                    step = 2
                    break
                elif k == 'ENTER':
                    if not input_val or not input_val.isdigit():
                        error_msg = "Ошибка: Введите корректное положительное число!"
                        continue
                    
                    val = int(input_val)
                    if val < 32:
                        error_msg = f"Ошибка: Слишком маленький размер ({val} МБ)! Минимум для FAT32 — 32 МБ."
                        continue
                    elif val > disk["size_mb"]:
                        error_msg = f"Ошибка: Размер {val} МБ превышает физическую емкость флешки ({disk['size_mb']} МБ)!"
                        continue
                    elif val > 2097152:
                        error_msg = "Ошибка: Размер не может превышать 2 ТБ (ограничение MBR тома)!"
                        continue

                    # Валидация пройдена!
                    wiz_config["size_mb"] = val
                    step = 4
                    break

        # -------------------------------------------------------------
        # ШАГ 4: Поэлементный выбор софта (каждый модуль и программа)
        # -------------------------------------------------------------
        elif step == 4:
            # Собираем общий список элементов
            items = []
            items.append(("game", "doom", "DOOM (DOOM.PRG + DOOM1.WAD)"))
            items.append(("game", "quake", "Quake (QUAKE.PRG + pak0.pak)"))
            
            for m in avail_modules:
                items.append(("module", m, f"{m}.sys (Системный модуль Ring 0)"))
            
            for p in avail_programs:
                items.append(("program", p, f"{p}.prg (Пользовательская программа Ring 3)"))

            items.append(("btn", "all",   "[ + ] Выбрать все компоненты"))
            items.append(("btn", "none",  "[ - ] Снять выбор со всех"))
            items.append(("btn", "next",  "[ ДАЛЕЕ: Итоговая сводка параметров -> ]"))
            items.append(("btn", "back",  "[ <- НАЗАД: Изменить размер раздела ]"))

            cur = 0
            visible_window = 13  # строк видимости для скроллинга

            while True:
                rows_disp = []
                rows_disp.append(f"{FG_WHITE}Выберите компоненты, которые будут установлены на накопитель:")
                rows_disp.append("Используйте стрелки ВВЕРХ/ВНИЗ для навигации, ПРОБЕЛ для переключения.")
                rows_disp.append("-------------------------------------------------------------------------------")

                # Расчет окна скролла
                start_idx = max(0, min(cur - visible_window // 2, len(items) - visible_window))
                end_idx = min(start_idx + visible_window, len(items))

                for i in range(start_idx, end_idx):
                    itype, ikey, ititle = items[i]
                    ptr = "->" if i == cur else "  "
                    clr = FG_YELLOW if i == cur else FG_WHITE

                    if itype == "btn":
                        rows_disp.append(f"{clr}{ptr} {ititle}{FG_WHITE}")
                    else:
                        if itype == "game":
                            is_checked = wiz_config["with_doom"] if ikey == "doom" else wiz_config["with_quake"]
                        elif itype == "module":
                            is_checked = wiz_config["modules"].get(ikey, False)
                        else:
                            is_checked = wiz_config["programs"].get(ikey, False)
                        
                        box = f"{FG_GREEN}[X]{clr}" if is_checked else f"{FG_WHITE}[ ]{clr}"
                        rows_disp.append(f"{clr}{ptr} {box} {ititle}{FG_WHITE}")

                rows_disp.append("-------------------------------------------------------------------------------")
                rows_disp.append(f"{FG_CYAN}Позиция: {cur + 1} из {len(items)} | [ПРОБЕЛ] = Вкл/Выкл | [ENTER] = Подтвердить{FG_WHITE}")

                render_frame(f"Мастер установки overOS v{os_ver} — Выбор софта (Шаг 3 из 4)", rows_disp, "СТРЕЛКИ=Скролл   ПРОБЕЛ=Вкл/Выкл   ENTER=Выбрать   F3=Отмена")
                k = get_key()

                if k == 'UP': cur = (cur - 1) % len(items)
                elif k == 'DOWN': cur = (cur + 1) % len(items)
                elif k == 'SPACE':
                    itype, ikey, _ = items[cur]
                    if itype == "game":
                        if ikey == "doom": wiz_config["with_doom"] = not wiz_config["with_doom"]
                        else: wiz_config["with_quake"] = not wiz_config["with_quake"]
                    elif itype == "module":
                        wiz_config["modules"][ikey] = not wiz_config["modules"][ikey]
                    elif itype == "program":
                        wiz_config["programs"][ikey] = not wiz_config["programs"][ikey]
                elif k == 'ENTER':
                    itype, ikey, _ = items[cur]
                    if itype == "btn":
                        if ikey == "all":
                            wiz_config["with_doom"] = True
                            wiz_config["with_quake"] = True
                            for m in wiz_config["modules"]: wiz_config["modules"][m] = True
                            for p in wiz_config["programs"]: wiz_config["programs"][p] = True
                        elif ikey == "none":
                            wiz_config["with_doom"] = False
                            wiz_config["with_quake"] = False
                            for m in wiz_config["modules"]: wiz_config["modules"][m] = False
                            for p in wiz_config["programs"]: wiz_config["programs"][p] = False
                        elif ikey == "next":
                            step = 5
                            break
                        elif ikey == "back":
                            step = 3
                            break
                    else:
                        # По нажатию Enter на элементе также переключаем его
                        if itype == "game":
                            if ikey == "doom": wiz_config["with_doom"] = not wiz_config["with_doom"]
                            else: wiz_config["with_quake"] = not wiz_config["with_quake"]
                        elif itype == "module":
                            wiz_config["modules"][ikey] = not wiz_config["modules"][ikey]
                        elif itype == "program":
                            wiz_config["programs"][ikey] = not wiz_config["programs"][ikey]
                elif k in ('F3', 'ESC', 'q'): return

        # -------------------------------------------------------------
        # ШАГ 5: Итоговая сводка параметров с возможностью возврата
        # -------------------------------------------------------------
        elif step == 5:
            disk = wiz_config["target_disk"]
            sel_mods = [f"{m}.sys" for m, v in wiz_config["modules"].items() if v]
            sel_progs = [f"{p}.prg" for p, v in wiz_config["programs"].items() if v]
            
            str_mods = ", ".join(sel_mods) if sel_mods else "Нет"
            str_progs = ", ".join(sel_progs) if sel_progs else "Нет"

            actions = [
                ("install", "[ НАЧАТЬ УСТАНОВКУ И СБОРКУ ОБРАЗА ]"),
                ("back_soft", "[ <- Вернуться: Изменить список программ и модулей ]"),
                ("back_size", "[ <- Вернуться: Изменить размер накопителя ]"),
                ("back_disk", "[ <- Вернуться: Выбрать другой USB-накопитель ]"),
                ("cancel",    "[ Отменить установку и выйти в меню ]")
            ]
            cur = 0

            while True:
                lines = [
                    f"{FG_WHITE}Мастер установки готов записать {FG_CYAN}overOS v{os_ver}{FG_WHITE} со следующими параметрами:",
                    "",
                    "========================== [ СВОДКА ПАРАМЕТРОВ ] ==========================",
                    f"  Версия системы:      {FG_CYAN}overOS v{os_ver}{FG_WHITE}",
                    f"  Целевой накопитель:  {FG_YELLOW}{disk['path']}{FG_WHITE} ({disk['model']}, {disk['human_size']})",
                    f"  Размер раздела:      {FG_YELLOW}{wiz_config['size_mb']} МБ{FG_WHITE} (FAT32 LFN)",
                    f"  Игровые пакеты:      DOOM: {FG_GREEN if wiz_config['with_doom'] else FG_RED}{'Вкл' if wiz_config['with_doom'] else 'Выкл'}{FG_WHITE} | Quake: {FG_GREEN if wiz_config['with_quake'] else FG_RED}{'Вкл' if wiz_config['with_quake'] else 'Выкл'}{FG_WHITE}",
                    f"  Модули ядра (.SYS):  {FG_CYAN}{str_mods}{FG_WHITE}",
                    f"  Программы (.PRG):    {FG_CYAN}{str_progs}{FG_WHITE}",
                    "===========================================================================",
                    f"{FG_RED}ВНИМАНИЕ: Нажатие кнопки установки сотрет все данные на {disk['path']}!{FG_WHITE}",
                    ""
                ]

                for i, (act_id, label) in enumerate(actions):
                    ptr = "->" if i == cur else "  "
                    clr = FG_YELLOW if i == cur else FG_WHITE
                    lines.append(f"{clr}{ptr} {label}{FG_WHITE}")

                render_frame(f"Мастер установки overOS v{os_ver} — Сводка (Шаг 4 из 4)", lines, "СТРЕЛКИ=Выбор   ENTER=Выбрать действие   F3=Отмена")
                k = get_key()

                if k == 'UP': cur = (cur - 1) % len(actions)
                elif k == 'DOWN': cur = (cur + 1) % len(actions)
                elif k == 'ENTER':
                    act = actions[cur][0]
                    if act == "install":
                        if execute_build_and_flash(wiz_config, os_ver, sel_mods, sel_progs):
                            return
                        else:
                            step = 5
                            break
                    elif act == "back_soft":
                        step = 4
                        break
                    elif act == "back_size":
                        step = 3
                        break
                    elif act == "back_disk":
                        step = 2
                        break
                    elif act == "cancel":
                        return
                elif k in ('F3', 'ESC', 'q'): return

# ==============================================================================
# ПРОЦЕДУРА СБОРКИ И ЗАПИСИ НА ДИСК
# ==============================================================================
def execute_build_and_flash(cfg, os_ver, sel_mods, sel_progs):
    disk = cfg["target_disk"]
    target_dev = disk["path"]

    render_frame("Сборка overOS", [
        f"{FG_WHITE}Выполняется полная компиляция overOS v{os_ver}...",
        f"  • Размер тома: {FG_CYAN}{cfg['size_mb']} МБ{FG_WHITE}",
        f"  • Модулей: {len(sel_mods)} | Программ: {len(sel_progs)}",
        "",
        "Компиляция ядра, линковка и генерация FAT32 раздела...",
        "Пожалуйста, подождите..."
    ], "Компиляция...")

    subprocess.run(["make", "clean"], capture_output=True)

    # Преобразуем списки модулей и программ в имена без расширений для Makefile
    raw_mods = " ".join([m.replace(".sys", "") for m in sel_mods])
    raw_progs = " ".join([p.replace(".prg", "") for p in sel_progs])

    cmd = [
        "make", "all",
        f"PART_SIZE_MB={cfg['size_mb']}",
        f"WITH_DOOM={1 if cfg['with_doom'] else 0}",
        f"WITH_QUAKE={1 if cfg['with_quake'] else 0}",
        f"INSTALL_MODULES={raw_mods}",
        f"INSTALL_PROGS={raw_progs}"
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)

    if res.returncode != 0 or not os.path.isfile(IMG_PATH):
        err_lines = [
            f"{FG_RED}Критическая ошибка компиляции overOS!{FG_WHITE}",
            "",
            res.stderr[-300:] if res.stderr else res.stdout[-300:],
            "",
            "Нажмите ENTER для возврата."
        ]
        while True:
            render_frame("Ошибка сборки", err_lines, "ENTER=Назад")
            if get_key() in ('ENTER', 'F3', 'ESC'): return False

    # Запись на диск
    unmount_device(target_dev)
    img_size = os.path.getsize(IMG_PATH)
    cols = os.get_terminal_size().columns
    bar_width = min(50, cols - 16)
    written_bytes = 0

    try:
        with open(IMG_PATH, "rb") as src, open(target_dev, "wb") as dst:
            while True:
                chunk = src.read(1024 * 1024)
                if not chunk: break
                dst.write(chunk)
                written_bytes += len(chunk)

                pct = int((written_bytes / img_size) * 100)
                filled = int(bar_width * (pct / 100))
                bar_str = "█" * filled + "░" * (bar_width - filled)
                mb_cur = written_bytes // (1024 * 1024)
                mb_tot = img_size // (1024 * 1024)

                lines = [
                    f"{FG_WHITE}Запись overOS v{os_ver} на {FG_CYAN}{target_dev}{FG_WHITE}...",
                    "",
                    f"Прогресс записи ({mb_cur} МБ из {mb_tot} МБ):",
                    "",
                    f"  {FG_YELLOW}[{bar_str}] {pct}%{FG_WHITE}",
                    "",
                    "Не извлекайте флешку до завершения сброса кэша!"
                ]
                render_frame("Запись на накопитель", lines, "Запись образа...")

        render_frame("Синхронизация", [
            f"{FG_WHITE}Сброс дискового буфера в память устройства (sync)...",
            "Обновление таблицы разделов MBR ядра Linux..."
        ], "Пожалуйста, подождите...")

        os.sync()
        subprocess.run(["partprobe", target_dev], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        subprocess.run(["blockdev", "--rereadpt", target_dev], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        time.sleep(1)

        success_lines = [
            f"{FG_GREEN}Установка overOS v{os_ver} успешно завершена!{FG_WHITE}",
            "",
            f"USB-диск {FG_CYAN}{target_dev}{FG_WHITE} ({disk['model']}) готов к загрузке в BIOS/CSM.",
            f"Записанный образ: {os.path.abspath(IMG_PATH)}",
            "",
            f"Параметры раздела: {cfg['size_mb']} МБ (FAT32)",
            f"Установлено: {len(sel_mods)} модулей и {len(sel_progs)} программ.",
            "",
            "Нажмите ENTER для выхода в главное меню."
        ]
        while True:
            render_frame("Установка завершена", success_lines, "ENTER=Готово")
            if get_key() in ('ENTER', 'F3', 'ESC'): break
        return True

    except Exception as e:
        err_lines = [
            f"{FG_RED}Ошибка при записи на накопитель:{FG_WHITE}",
            str(e),
            "",
            "Нажмите ENTER для возврата."
        ]
        while True:
            render_frame("Ошибка записи", err_lines, "ENTER=Назад")
            if get_key() in ('ENTER', 'F3', 'ESC'): return False

# ==============================================================================
# ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ГЛАВНОГО МЕНЮ
# ==============================================================================
def action_update_kernel():
    render_frame("Обновление ядра", [
        f"{FG_WHITE}Выполняется быстрая сборка kernel.bin...",
        "Пожалуйста, подождите..."
    ], "Компиляция ядра...")

    res = subprocess.run(["make", KERNEL_BIN], capture_output=True, text=True)
    if res.returncode != 0 or not os.path.isfile(KERNEL_BIN):
        render_frame("Ошибка сборки", [f"{FG_RED}Не удалось собрать kernel.bin!{FG_WHITE}"], "ENTER=Назад")
        while True:
            if get_key() in ('ENTER', 'F3', 'ESC'): return

    disks = get_usb_disks()
    if not disks:
        render_frame("Диск не найден", [f"{FG_YELLOW}USB-накопители не обнаружены!{FG_WHITE}"], "ENTER=Назад")
        while True:
            if get_key() in ('ENTER', 'F3', 'ESC'): return

    target_dev = disks[0]["path"]
    part_target = f"{target_dev}2" if os.path.exists(f"{target_dev}2") else f"{target_dev}p2"
    if not os.path.exists(part_target): part_target = f"{target_dev}1"

    unmount_device(target_dev)
    updated = False
    res_mcopy = subprocess.run(["mcopy", "-o", "-i", part_target, KERNEL_BIN, "::sys/KERNEL.BIN"], capture_output=True)
    if res_mcopy.returncode == 0:
        updated = True
    else:
        os.makedirs("/tmp/overos_mount", exist_ok=True)
        if subprocess.run(["mount", part_target, "/tmp/overos_mount"], stderr=subprocess.DEVNULL).returncode == 0:
            try:
                dest = "/tmp/overos_mount/sys/KERNEL.BIN"
                subprocess.run(["cp", KERNEL_BIN, dest], check=True)
                os.sync()
                updated = True
            except Exception: pass
            finally:
                subprocess.run(["umount", "/tmp/overos_mount"], stderr=subprocess.DEVNULL)

    lines = [
        f"{FG_GREEN}Ядро KERNEL.BIN успешно обновлено на {part_target}!{FG_WHITE}",
        "Все остальные файлы, игры и настройки сохранены."
    ] if updated else [f"{FG_RED}Не удалось перезаписать KERNEL.BIN на {part_target}!{FG_WHITE}"]

    while True:
        render_frame("Обновление ядра", lines, "ENTER=Готово")
        if get_key() in ('ENTER', 'F3', 'ESC'): break

def action_test_qemu():
    if not os.path.isfile(IMG_PATH):
        render_frame("Тестирование", [f"{FG_RED}Образ build/os.img не найден! Сначала запустите установщик.{FG_WHITE}"], "ENTER=Назад")
        while True:
            if get_key() in ('ENTER', 'F3', 'ESC'): return
    restore_terminal()
    print(">>> Запуск overOS в QEMU...")
    subprocess.run(["make", "run"])
    sys.stdout.write("\033[2J\033[H")

def main():
    if os.geteuid() != 0:
        print("Ошибка: Запустите через 'sudo make install' или 'sudo python3 tools/installer_gui.py'")
        sys.exit(1)

    sys.stdout.write("\033[2J\033[H")
    sys.stdout.flush()

    os_ver = get_os_version()
    menu_options = [
        "Установить overOS (Мастер пошаговой установки)",
        "Обновить ядро на флешке (быстро, без удаления файлов)",
        "Протестировать образ в QEMU",
        "Очистить сборку (make clean)",
        "Выход"
    ]
    cur_idx = 0

    while True:
        lines = [
            f"{FG_WHITE}Добро пожаловать в центр развертывания {FG_CYAN}overOS v{os_ver}{FG_WHITE}.",
            "Используйте стрелки ВВЕРХ и ВНИЗ для навигации, ENTER для выбора.",
            "",
            "-------------------------------------------------------------------------------"
        ]

        for i, opt in enumerate(menu_options):
            if i == cur_idx:
                lines.append(f"{FG_YELLOW}  -> [ {opt} ]{FG_WHITE}")
            else:
                lines.append(f"     [ {opt} ]")

        lines.extend([
            "-------------------------------------------------------------------------------",
            f"Версия ядра:       {FG_CYAN}{os_ver}{FG_WHITE}",
            f"Текущий образ:     " + (
                f"{FG_GREEN}ГОТОВ ({os.path.abspath(IMG_PATH)}){FG_WHITE}"
                if os.path.isfile(IMG_PATH) else f"{FG_RED}НЕ СОБРАН{FG_WHITE}"
            )
        ])

        render_frame(f"Главное меню overOS v{os_ver}", lines, "СТРЕЛКИ=Выбор   ENTER=Запуск   F3=Выход")
        k = get_key()

        if k == 'UP': cur_idx = (cur_idx - 1) % len(menu_options)
        elif k == 'DOWN': cur_idx = (cur_idx + 1) % len(menu_options)
        elif k in ('F3', 'ESC', 'q'): break
        elif k == 'ENTER':
            choice = menu_options[cur_idx]
            if choice.startswith("Установить overOS"): run_install_wizard()
            elif choice.startswith("Обновить ядро"): action_update_kernel()
            elif choice.startswith("Протестировать"): action_test_qemu()
            elif choice.startswith("Очистить"):
                subprocess.run(["make", "clean"], capture_output=True)
                render_frame("Очистка", [f"{FG_GREEN}Сборка очищена!{FG_WHITE}"], "ENTER=Готово")
                while True:
                    if get_key() in ('ENTER', 'F3', 'ESC'): break
            elif choice == "Выход": break

    restore_terminal()

if __name__ == "__main__":
    main()