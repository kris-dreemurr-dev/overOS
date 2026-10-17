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

// Куда идёт звук (hda_get_target)
#define HDA_TARGET_NONE   0
#define HDA_TARGET_ANALOG 1
#define HDA_TARGET_HDMI   2

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
int  hda_ensure_init(void);          // hda_init() один раз, безопасно звать откуда угодно (0 ok, -1/-3 как у hda_init)
void hda_play_test_tone(void);       // тестовый тон 1 кГц (громкость учитывается)
void hda_stop(void);
int  hda_get_status(void);

int  hda_set_output(int mode);       // 0 ок, -1 такого выхода нет, -2 неверный режим
int  hda_get_output(void);
int  hda_get_target(void);           // куда реально идёт звук: HDA_TARGET_*
int  hda_jack_poll(void);            // вызывать периодически (штекер/дисплей); 1 = наушники воткнуты
int  hda_headphones_plugged(void);   // последнее известное состояние

int  check_audio_outputs(hda_outputs_t* out);   // 0 ok
void hda_print_outputs(void);                   // то же текстом через kputs

// ---- Громкость (программная: работает на динамиках, наушниках и HDMI) ----
void hda_set_volume(int percent);    // 0..100, действует сразу (с задержкой до ~170 мс на уже поставленные в очередь данные)
int  hda_get_volume(void);
void hda_set_mute(int mute);
int  hda_get_mute(void);

// ---- Потоковое воспроизведение PCM (для плееров) ----
// Порядок: hda_pcm_open() -> в цикле hda_pcm_write() -> hda_pcm_drained() -> hda_pcm_close().
// owner — pid программы: у каждой свой голос, драйвер их микширует, поэтому два
// плеера больше не выключают звук друг другу.
// 44.1 и 48 кГц играются как есть, остальные частоты ресемплируются. Вход: 8/16/24/32 бит.
int      hda_pcm_open(uint32_t rate, int channels, int bits, uint64_t owner);   // 0 ок, <0 ошибка (-1 не готов, -2 нет выхода, -3 формат, -5 выход)
int      hda_pcm_write(const void* data, uint32_t bytes, uint64_t owner);       // не блокируется; вернёт, сколько байт принято (0 = очередь полна)
uint32_t hda_pcm_position_ms(uint64_t owner);   // сколько мс проиграно с момента open/flush
uint32_t hda_pcm_queued_ms(uint64_t owner);     // сколько мс стоит в очереди
int      hda_pcm_drained(uint64_t owner);       // 1: всё записанное уже проиграно
uint32_t hda_pcm_underruns(uint64_t owner);     // сколько раз данных не хватило
void     hda_pcm_flush(uint64_t owner);         // сбросить очередь и позицию (перемотка, пауза)
uint32_t hda_pcm_frame_bytes(uint64_t owner);    // размер кадра открытого потока, байт
void     hda_pcm_close(uint64_t owner);

// ---- Диагностика ----
void hda_set_verbose(int on);        // 1 = печатать, что нашёл драйвер (вызывай ДО hda_init()); по умолчанию 0
void hda_dump_state(void);       // полный дамп состояния драйвера, стрима и кодека (всегда в консоль)

// Прокачка аппаратной очереди звука из таймера ядра (hda.c): без неё DMA
// уходит на второй круг, когда программа перестала писать (пауза, фон).
void     hda_tick(void);

// Погасить тестовый тон, не трогая звук программ
void     hda_stop_test_tone(void);

#endif