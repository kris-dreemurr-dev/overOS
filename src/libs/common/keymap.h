#ifndef KEYMAP_H
#define KEYMAP_H

// Перевод скан-кодов PS/2 (set 1) в символы UTF-8: US QWERTY и русская ЙЦУКЕН, с Shift.
// Используется композитором: приложения получают уже готовый текст.

#include <stdint.h>

static const char* const km_us_lo[0x3A] = {
    0, 0, "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", 0,
    0, "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", 0,
    0, "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "`", 0,
    "\\", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", 0, "*", 0, " "
};
static const char* const km_us_hi[0x3A] = {
    0, 0, "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", 0,
    0, "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", 0,
    0, "A", "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", "~", 0,
    "|", "Z", "X", "C", "V", "B", "N", "M", "<", ">", "?", 0, "*", 0, " "
};
static const char* const km_ru_lo[0x3A] = {
    0, 0, "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", 0,
    0, "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ъ", 0,
    0, "ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "ё", 0,
    "\\", "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", ".", 0, "*", 0, " "
};
static const char* const km_ru_hi[0x3A] = {
    0, 0, "!", "\"", "№", ";", "%", ":", "?", "*", "(", ")", "_", "+", 0,
    0, "Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З", "Х", "Ъ", 0,
    0, "Ф", "Ы", "В", "А", "П", "Р", "О", "Л", "Д", "Ж", "Э", "Ё", 0,
    "/", "Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", ",", 0, "*", 0, " "
};

// Caps Lock меняет регистр только у букв: на цифрах и знаках он ничего не делает,
// поэтому просто подменить shift нельзя (иначе с включённым Caps «1» давала бы «!»).
// Буква определяется по нижнему регистру: латиница a..z либо кириллица в UTF-8
// (ведущий байт 0xD0/0xD1).
static inline int km_is_letter(const char* lo) {
    if (!lo) return 0;
    unsigned char c = (unsigned char)lo[0];
    return (c >= 'a' && c <= 'z') || c == 0xD0 || c == 0xD1;
}

// out — не меньше 8 байт; пустая строка, если клавиша не печатает символ
static inline void km_translate(int scancode, int shift, int caps, int ru, char* out) {
    out[0] = '\0';
    if (scancode < 0 || scancode >= 0x3A) return;
    const char* lo = ru ? km_ru_lo[scancode] : km_us_lo[scancode];
    if (caps && km_is_letter(lo)) shift = !shift;
    const char* s = ru ? (shift ? km_ru_hi[scancode] : km_ru_lo[scancode])
                       : (shift ? km_us_hi[scancode] : km_us_lo[scancode]);
    if (!s) return;
    int i = 0;
    while (s[i] && i < 7) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

#endif
