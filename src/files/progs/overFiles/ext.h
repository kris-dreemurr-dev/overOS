#ifndef EXT_H
#define EXT_H
// ext.h — определение типа файла по расширению: вид (для значка), название
// (для подсказки) и действие при открытии.
//
// Действие назначается только тем форматам, которые наши программы реально
// умеют открыть. Для остальных вид и название есть, а открытие сообщает, что
// файл нечем открыть, — иначе пользователь получал бы пустое окно.

#include <stdint.h>

// Вспомогательные функции строк для freestanding окружения
static inline int ext_strlen(const char* s) {
    int l = 0; while (s && s[l]) l++; return l;
}

static inline int ext_streq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char c1 = *a, c2 = *b;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

static inline int ext_ends_with_ci(const char* name, const char* ext) {
    int nl = ext_strlen(name);
    int el = ext_strlen(ext);
    if (nl < el) return 0;
    const char* p = name + (nl - el);
    while (*p && *ext) {
        char c1 = *p, c2 = *ext;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        p++; ext++;
    }
    return 1;
}

// Проверка расширения по списку, оканчивающемуся нулевым указателем
static inline int ext_in(const char* name, const char* const* list) {
    for (int i = 0; list[i]; i++) if (ext_ends_with_ci(name, list[i])) return 1;
    return 0;
}

// Форматирование размера файла: "540 Б", "5.9 КБ", "1.4 МБ", "2.1 ГБ"
static inline void ext_format_size(uint32_t bytes, char* out, int max_len) {
    const char* unit = " Б";
    uint32_t val = bytes, frac = 0, div = 1;

    if (bytes >= 1024u * 1024 * 1024)      { div = 1024u * 1024 * 1024; unit = " ГБ"; }
    else if (bytes >= 1024u * 1024)        { div = 1024u * 1024;        unit = " МБ"; }
    else if (bytes >= 1024u)               { div = 1024u;               unit = " КБ"; }

    if (div > 1) {
        val  = bytes / div;
        frac = ((bytes % div) * 10) / div;
    }

    char tmp[16];
    int i = 0;
    uint32_t n = val;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }

    int p = 0;
    while (i > 0 && p < max_len - 8) out[p++] = tmp[--i];
    if (div > 1 && p < max_len - 6) {          // дробную часть показываем только с приставкой
        out[p++] = '.';
        out[p++] = (char)('0' + (frac % 10));
    }
    while (*unit && p < max_len - 1) out[p++] = *unit++;
    out[p] = '\0';
}

// Проверка DPRG заголовка на флаг PRG_FLAG_DESKTOP (0x01)
static inline int ext_check_prg_desktop(const char* full_path) {
    extern int sys_open(const char* fn, int flags);
    extern int sys_read(int fd, void* buf, int sz);
    extern void sys_close(int fd);

    int fd = sys_open(full_path, 0);
    if (fd < 0) return 0;
    uint8_t hdr[64];
    int rd = sys_read(fd, hdr, 64);
    sys_close(fd);

    if (rd >= 64 && hdr[0] == 'D' && hdr[1] == 'P' && hdr[2] == 'R' && hdr[3] == 'G') {
        uint64_t flags = *(uint64_t*)&hdr[48];
        return (flags & 0x01) ? 1 : 0;
    }
    return 0;
}

// ============================================================================
// Вид файла — он же выбор значка
// ============================================================================
#define FILE_KIND_DIR      0
#define FILE_KIND_IMAGE    1
#define FILE_KIND_AUDIO    2
#define FILE_KIND_VIDEO    3
#define FILE_KIND_TEXT     4
#define FILE_KIND_CODE     5
#define FILE_KIND_CONFIG   6
#define FILE_KIND_ARCHIVE  7
#define FILE_KIND_EXEC     8    // .prg с флагом рабочего стола — запускается двойным щелчком
#define FILE_KIND_MODULE   9    // .sys — модуль ядра, Ring 0
#define FILE_KIND_BINARY  10    // .bin, .prg консольной программы, образы
#define FILE_KIND_FONT    11
#define FILE_KIND_LIBRARY 12   // .ll — разделяемая библиотека overOS (аналог .so)
#define FILE_KIND_OTHER   13
#define FILE_KIND_COUNT   14

static const char* const ext_list_image[]   = { ".bmp", ".png", ".jpg", ".jpeg", ".gif", ".ico", ".tga", ".ppm",
                                                ".webp", ".apng", 0 };
static const char* const ext_list_audio[]   = { ".wav", ".mp3", ".ogg", ".flac", ".mid", ".mod", ".aac", ".m4a",
                                                ".opus", ".oga", ".alac", ".ac3", ".eac3", ".dts", ".wma",
                                                ".ape", ".tta", ".wv", ".aiff", ".aif", ".au", ".caf", ".dsf",
                                                ".mp2", ".mp1", ".amr", ".ra", ".w64", ".voc", 0 };
static const char* const ext_list_video[]   = { ".mp4", ".avi", ".mkv", ".mpg", ".mpeg", ".mov", ".webm", ".h264",
                                                ".hevc", ".h265", ".m4v", ".flv", ".wmv", ".asf", ".ts", ".m2ts",
                                                ".mts", ".vob", ".ogv", ".3gp", ".rm", ".rmvb", ".dv", ".swf",
                                                ".y4m", ".mjpeg", 0 };
static const char* const ext_list_text[]    = { ".txt", ".md", ".log", ".readme", ".nfo", 0 };
static const char* const ext_list_code[]    = { ".c", ".h", ".cpp", ".hpp", ".asm", ".s", ".py", ".sh", ".ld", ".mk", 0 };
static const char* const ext_list_config[]  = { ".json", ".ini", ".conf", ".cfg", ".rc", ".toml", ".yml", 0 };
static const char* const ext_list_archive[] = { ".zip", ".tar", ".gz", ".wad", ".pak", ".iso", ".pk3", 0 };
static const char* const ext_list_font[]    = { ".ttf", ".otf", ".fnt", ".psf", 0 };
static const char* const ext_list_binary[]  = { ".bin", ".img", ".elf", ".o", ".a", ".dat", ".dll", ".exe", 0 };

static inline int ext_get_kind(const char* full_path, const char* name, uint8_t attr) {
    if (attr & 0x10)                      return FILE_KIND_DIR;
    if (ext_ends_with_ci(name, ".sys"))   return FILE_KIND_MODULE;
    if (ext_ends_with_ci(name, ".ll"))    return FILE_KIND_LIBRARY;
    if (ext_ends_with_ci(name, ".prg"))
        return ext_check_prg_desktop(full_path) ? FILE_KIND_EXEC : FILE_KIND_BINARY;
    if (ext_in(name, ext_list_image))     return FILE_KIND_IMAGE;
    if (ext_in(name, ext_list_audio))     return FILE_KIND_AUDIO;
    if (ext_in(name, ext_list_video))     return FILE_KIND_VIDEO;
    if (ext_in(name, ext_list_code))      return FILE_KIND_CODE;
    if (ext_in(name, ext_list_config))    return FILE_KIND_CONFIG;
    if (ext_in(name, ext_list_archive))   return FILE_KIND_ARCHIVE;
    if (ext_in(name, ext_list_font))      return FILE_KIND_FONT;
    if (ext_in(name, ext_list_binary))    return FILE_KIND_BINARY;
    if (ext_in(name, ext_list_text))      return FILE_KIND_TEXT;
    return FILE_KIND_OTHER;
}

// Название типа для подсказки: сначала точные форматы, потом общий вид
static inline const char* ext_get_type_label(const char* full_path, const char* name, uint8_t attr) {
    int kind = ext_get_kind(full_path, name, attr);

    if (ext_ends_with_ci(name, ".bmp"))  return "Изображение BMP";
    if (ext_ends_with_ci(name, ".png"))  return "Изображение PNG";
    if (ext_ends_with_ci(name, ".jpg") || ext_ends_with_ci(name, ".jpeg")) return "Изображение JPEG";
    if (ext_ends_with_ci(name, ".wav"))  return "Звук WAV";
    if (ext_ends_with_ci(name, ".mp3"))  return "Звук MP3";
    if (ext_ends_with_ci(name, ".mp4"))  return "Видео MP4";
    if (ext_ends_with_ci(name, ".mkv"))  return "Видео MKV";
    if (ext_ends_with_ci(name, ".avi"))  return "Видео AVI";
    if (ext_ends_with_ci(name, ".mov"))  return "Видео QuickTime";
    if (ext_ends_with_ci(name, ".m4a"))  return "Звук AAC (M4A)";
    if (ext_ends_with_ci(name, ".aac"))  return "Звук AAC";
    if (ext_ends_with_ci(name, ".c"))    return "Исходник на Си";
    if (ext_ends_with_ci(name, ".h"))    return "Заголовок Си";
    if (ext_ends_with_ci(name, ".asm") || ext_ends_with_ci(name, ".s")) return "Исходник на ассемблере";
    if (ext_ends_with_ci(name, ".json")) return "Настройки JSON";
    if (ext_ends_with_ci(name, ".txt"))  return "Текстовый документ";
    if (ext_ends_with_ci(name, ".wad") || ext_ends_with_ci(name, ".pak")) return "Архив игры";

    switch (kind) {
        case FILE_KIND_DIR:     return "Папка";
        case FILE_KIND_IMAGE:   return "Изображение";
        case FILE_KIND_AUDIO:   return "Звуковой файл";
        case FILE_KIND_VIDEO:   return "Видеофайл";
        case FILE_KIND_TEXT:    return "Текстовый документ";
        case FILE_KIND_CODE:    return "Исходный код";
        case FILE_KIND_CONFIG:  return "Файл настроек";
        case FILE_KIND_ARCHIVE: return "Архив";
        case FILE_KIND_EXEC:    return "Программа";
        case FILE_KIND_MODULE:  return "Модуль ядра";
        case FILE_KIND_BINARY:  return "Двоичный файл";
        case FILE_KIND_FONT:    return "Шрифт";
        case FILE_KIND_LIBRARY: return "Библиотека overOS";
        default:                return "Файл";
    }
}

// Строка подсказки: "prog_loader.c (Исходник на Си, 5.9 КБ)"
static inline void ext_get_info_string(const char* full_path, const char* name, uint8_t attr, uint32_t size, char* out, int max_len) {
    int p = 0;
    while (name[p] && p < max_len - 40) {
        out[p] = name[p];
        p++;
    }
    out[p++] = ' ';
    out[p++] = '(';

    const char* type_str = ext_get_type_label(full_path, name, attr);
    while (*type_str && p < max_len - 20) {
        out[p++] = *type_str++;
    }

    if (!(attr & 0x10)) {                      // у папок размер не показываем
        out[p++] = ',';
        out[p++] = ' ';
        char size_buf[16];
        ext_format_size(size, size_buf, sizeof(size_buf));
        int sp = 0;
        while (size_buf[sp] && p < max_len - 2) {
            out[p++] = size_buf[sp++];
        }
    }

    out[p++] = ')';
    out[p] = '\0';
}

// ============================================================================
// Действие при открытии
// ============================================================================
#define FILE_ACT_DIR       1
#define FILE_ACT_EXEC_PRG  2
#define FILE_ACT_IMAGE     3
#define FILE_ACT_TEXT      4
#define FILE_ACT_BLOCKED   5
#define FILE_ACT_AUDIO     6

// Форматы, которые наши программы действительно открывают.
// Звук и видео умеет /lib/ffmpeg.ll (настоящий FFmpeg); список повторяет то,
// что включено в его сборке (см. configure в tools/build_ffmpeg.sh).
static const char* const ext_open_image[] = { ".bmp", ".png", 0 };
static const char* const ext_open_audio[] = { ".wav", ".mp3", ".mp2", ".mp1", ".mp4", ".m4a", ".m4v", ".mov",
                                              ".mkv", ".webm", ".avi", ".aac", ".h264", ".hevc", ".h265",
                                              ".flac", ".ogg", ".oga", ".ogv", ".opus", ".ac3", ".eac3",
                                              ".dts", ".wma", ".wmv", ".asf", ".flv", ".ts", ".m2ts", ".mts",
                                              ".mpg", ".mpeg", ".vob", ".ape", ".tta", ".wv", ".aiff", ".aif",
                                              ".au", ".caf", ".dsf", ".w64", ".voc", ".amr", ".rm", ".rmvb",
                                              ".dv", ".swf", ".3gp", ".y4m", 0 };

static inline int ext_get_action(const char* full_path, const char* name, uint8_t attr) {
    if (attr & 0x10) return FILE_ACT_DIR;

    // Защита от дурака: desktop.prg нельзя открывать/запускать
    if (ext_streq_ci(name, "desktop.prg")) return FILE_ACT_BLOCKED;

    if (ext_ends_with_ci(name, ".prg"))
        return ext_check_prg_desktop(full_path) ? FILE_ACT_EXEC_PRG : FILE_ACT_BLOCKED;

    if (ext_in(name, ext_open_image)) return FILE_ACT_IMAGE;
    if (ext_in(name, ext_open_audio)) return FILE_ACT_AUDIO;

    // Форматы, для которых декодера пока нет, и двоичные данные блокируем:
    // открывать их блокнотом бессмысленно
    int kind = ext_get_kind(full_path, name, attr);
    if (kind == FILE_KIND_IMAGE || kind == FILE_KIND_AUDIO || kind == FILE_KIND_VIDEO ||
        kind == FILE_KIND_ARCHIVE || kind == FILE_KIND_MODULE || kind == FILE_KIND_BINARY ||
        kind == FILE_KIND_FONT  || kind == FILE_KIND_LIBRARY)
        return FILE_ACT_BLOCKED;

    // Остальное (текст, код, настройки, файлы без расширения) — блокнотом
    return FILE_ACT_TEXT;
}

#endif // EXT_H
