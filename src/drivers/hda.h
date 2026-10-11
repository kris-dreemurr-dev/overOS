#ifndef HDA_H
#define HDA_H

#include <stdint.h>

// Статусы драйвера HDA
#define HDA_STATUS_NOT_FOUND   0
#define HDA_STATUS_INITIALIZED 1
#define HDA_STATUS_PLAYING     2

// Режимы вывода звука
#define HDA_OUT_AUTO       0   // ноутбук: динамики/наушники; ПК: воткнутый разъём, иначе HDMI с дисплеем
#define HDA_OUT_SPEAKERS   1   // всегда динамики
#define HDA_OUT_HEADPHONES 2   // всегда наушники (разъём)
#define HDA_OUT_HDMI       3   // всегда HDMI/DisplayPort (звук в монитор/телевизор)

// Что доступно для вывода звука (заполняется check_audio_outputs)
typedef struct {
    int controllers;          // сколько HDA-контроллеров найдено
    int codecs;               // сколько кодеков ответило
    int has_speakers;         // встроенные динамики
    int has_headphones;       // разъём наушников
    int has_lineout;          // линейный выход
    int has_hdmi;             // число цифровых портов HDMI/DisplayPort (по данным BIOS)
    int has_spdif;            // SPDIF
    int headphones_plugged;   // штекер наушников воткнут
    int lineout_plugged;      // штекер в линейном выходе
    int hdmi_connected;       // сколько HDMI/DP портов с подключённым дисплеем
    int analog_playback;      // 1: драйвер умеет играть через аналоговый выход
    int hdmi_playback;        // 1: есть HDMI-пин, через который драйвер может играть
} hda_outputs_t;

int  hda_init(void);                 // 0 ok, -1 нет контроллера, -3 играть некуда (нет ни аналога, ни HDMI)
void hda_play_test_tone(void);
void hda_stop(void);
int  hda_get_status(void);

int  hda_set_output(int mode);       // 0 ок, -1 такого выхода нет, -2 неверный режим
int  hda_get_output(void);
int  hda_jack_poll(void);            // вызывать периодически (штекер/дисплей); 1 = наушники воткнуты
int  hda_headphones_plugged(void);   // последнее известное состояние

int  check_audio_outputs(hda_outputs_t* out);   // 0 ok
void hda_print_outputs(void);                   // то же текстом через kputs

#endif