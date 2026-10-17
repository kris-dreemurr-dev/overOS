#ifndef APPINFO_H
#define APPINFO_H
// appinfo.h — что известно о программе по её исполняемому файлу: имя для
// меню и уведомлений, класс (для дока) и есть ли значок. Берётся из ресурсов
// .prg (src/libs/common/prgres.h) и кэшируется: док и меню спрашивают на каждом
// кадре, а файл читать каждый раз -- это обращения к флешке.

typedef struct {
    char path[96];
    char name[64];      // "Файлы"; нет ресурса NAME -- имя файла без .prg
    char app[24];       // "ovfiles"; нет ресурса APPI -- имя файла в нижнем регистре
    int  has_icon;      // в .prg есть ресурс ICON
} appinfo_t;

// Никогда не NULL (для пустого пути -- запись-заглушка)
const appinfo_t* appinfo(const char* path);

#endif
