#!/usr/bin/env python3
# Генерация /etc для образа overOS: passwd, group, shadow, perms
# Формат хеша shadow: $ov$<salt>$<sha256(salt + password) hex> — проверяется ядром (src/kernel/users.c)
import hashlib
import os
import secrets
import sys

if len(sys.argv) != 5:
    print(f"Usage: {sys.argv[0]} <out_dir> <root_password> <user_name|''> <user_password>")
    print("  Пустое имя пользователя: только root, при первом запуске — первоначальная настройка (OOBE)")
    sys.exit(1)

out_dir, root_pass, user, user_pass = sys.argv[1:5]
UID = 1000

if user and not all(c.isdigit() or ('a' <= c <= 'z') or c in "_-" for c in user):
    print(f"mkusers: bad user name '{user}' (allowed: a-z 0-9 _ -)")
    sys.exit(1)


def shadow_hash(password):
    if password == "":
        return "!"                     # заблокирован: вход по паролю запрещён (root — через sudo)
    salt = secrets.token_hex(8)
    return f"$ov${salt}${hashlib.sha256((salt + password).encode()).hexdigest()}"


os.makedirs(out_dir, exist_ok=True)


def write(name, text):
    with open(os.path.join(out_dir, name), "w", newline="\n") as f:
        f.write(text)


write("passwd",
      "root:x:0:0:root:/root:tty\n" +
      (f"{user}:x:{UID}:{UID}:{user}:/home/{user}:tty\n" if user else ""))

write("group",
      "root:x:0:\n"
      f"wheel:x:10:{user}\n"            # администраторы: могут sudo
      "users:x:100:\n" +
      (f"{user}:x:{UID}:\n" if user else ""))

write("shadow",
      f"root:{shadow_hash(root_pass)}:\n" +
      (f"{user}:{shadow_hash(user_pass)}:\n" if user else ""))

# Системные язык, часовой пояс и раскладки (OOBE перезапишет своим выбором)
# Модули ядра, которые поднимает загрузчик (как /etc/modules-load.d в systemd).
# Имя без пути ищется в /utils. По умолчанию ничего не грузим: все наши .sys —
# необязательные, а падение модуля на старте хуже, чем его отсутствие.
write("modules",
      "# Модули ядра overOS: по одному имени в строке, ищутся в /utils\n"
      "# Строки с # -- комментарии. Пример:\n"
      "#stress.sys\n")

write("locale.conf", "LANG=ru_RU.UTF-8\n")
write("timezone", "Europe/Moscow\n")
write("vconsole.conf", "KEYMAP=us\nXKBLAYOUT=us,ru\n")

# Флаг первоначальной настройки: пользователь задан при сборке — OOBE не нужна
write("oobe", "1\n" if user else "0\n")

write("perms",
      "# path uid gid mode - права файлов (в FAT32 их хранить негде)\n"
      "# Файл без своей строки наследует права ближайшего каталога-родителя\n"
      "/ 0 0 0755\n"
      "/sys/kernel.bin 0 0 0600\n"
      "/utils 0 0 0700\n"
      "/etc 0 0 0755\n"
      "/etc/shadow 0 0 0600\n"
      "/root 0 0 0700\n"
      "/home 0 0 0755\n" +
      (f"/home/{user} {UID} {UID} 0700\n" if user else ""))
