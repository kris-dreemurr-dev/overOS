int is_playpal(char* name) {
    if (name[0] == 'P' && name[1] == 'L' && name[2] == 'A' && name[3] == 'Y' &&
        name[4] == 'P' && name[5] == 'A' && name[6] == 'L') {
        return 1;
        }
        return 0;
}

void main() {
    clear(0x000000);
    print_color("=== devOS Doom Port: Step 3 (PLAYPAL) ===\n\n", 0x55FFFF);

    print_color("[FS] Opening DOOM1.WAD... ", 0xAAAAAA);
    int fd = open("DOOM1.WAD");
    if (fd < 0) {
        print_color("FAILED!\n", 0xFF5555);
        exit(1);
    }
    print_color("OK\n", 0x55FF55);

    // Загружаем весь WAD целиком (до 16 МБ)
    uint32_t wad_base = 0x02000000;
    print_color("[FS] Loading full WAD to RAM (16 MB)... ", 0xAAAAAA);

    int bytes = read(fd, wad_base, 16000000);
    if (bytes <= 0) {
        print_color("READ ERROR!\n", 0xFF5555);
        close(fd);
        exit(1);
    }
    print_color("OK (", 0x55FF55);
    print_num(bytes);
    print_color(" bytes)\n", 0x55FF55);
    close(fd);

    uint32_t numlumps = *(uint32_t*)(wad_base + 4);
    uint32_t dir_ofs  = *(uint32_t*)(wad_base + 8);

    print_color("[WAD] Total Lumps: ", 0xAAAAAA);
    print_num(numlumps);
    print("\n");

    print_color("[WAD] Dir Offset:  ", 0xAAAAAA);
    print_num(dir_ofs);
    print("\n");

    // Ищем PLAYPAL в считанном каталоге
    print_color("[WAD] Searching for PLAYPAL... ", 0xAAAAAA);
    uint32_t playpal_pos = 0;
    uint32_t playpal_size = 0;

    for (int i = 0; i < numlumps; i++) {
        uint32_t entry = wad_base + dir_ofs + (i * 16);
        char* name = (char*)(entry + 8);

        if (is_playpal(name)) {
            playpal_pos  = *(uint32_t*)(entry);
            playpal_size = *(uint32_t*)(entry + 4);
            break;
        }
    }

    if (playpal_pos == 0) {
        print_color("NOT FOUND!\n", 0xFF5555);
        exit(1);
    }
    print_color("FOUND!\n", 0x55FF55);

    print_color("      File Offset: ", 0xAAAAAA);
    print_num(playpal_pos);
    print(" | Size: ");
    print_num(playpal_size);
    print(" bytes\n\n");

    print_color("Press ANY key to blit Doom 256-color palette grid...", 0xFFFF55);
    while (!get_key()) {
        sleep(20);
    }

    // Конвертируем палитру PLAYPAL (256 RGB триплетов = 768 байт) в ARGB32
    uint32_t pal_addr = 0x01060000;
    uint32_t* pal = (uint32_t*)pal_addr;
    uint8_t* raw_rgb = (uint8_t*)(wad_base + playpal_pos);

    for (int i = 0; i < 256; i++) {
        uint32_t r = raw_rgb[i * 3 + 0];
        uint32_t g = raw_rgb[i * 3 + 1];
        uint32_t b = raw_rgb[i * 3 + 2];
        pal[i] = (r << 16) | (g << 8) | b;
    }

    // Загружаем настоящие цвета Дума в ядро
    set_palette((uint32_t*)pal_addr);

    // Формируем сетку 16x16 всех 256 цветов
    uint32_t fb_addr = 0x01070000;
    uint8_t* fb = (uint8_t*)fb_addr;

    clear(0x0A0E1A);

    for (int y = 0; y < 200; y++) {
        int row = y * 320;
        for (int x = 0; x < 320; x++) {
            if (x < 2 || x >= 318 || y < 2 || y >= 198) {
                fb[row + x] = 255;
            } else if (x >= 32 && x < 288 && y >= 20 && y < 180) {
                int col = (x - 32) / 16;
                int line = (y - 20) / 10;
                int color_idx = line * 16 + col;
                fb[row + x] = (uint8_t)color_idx;
            } else {
                fb[row + x] = 0;
            }
        }
    }

    blit_frame((uint8_t*)fb_addr);

    while (!get_key()) {
        sleep(20);
    }

    clear(0x000000);
    exit(0);
}
