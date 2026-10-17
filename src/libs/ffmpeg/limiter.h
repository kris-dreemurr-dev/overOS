#ifndef LIMITER_H
#define LIMITER_H
// limiter.h — пиковый лимитер с заглядыванием вперёд, для вывода звука в int16.
//
// Зачем он нужен: материал из сети часто громче полной шкалы. У видео из
// Telegram декодер ffmpeg выдаёт пик 1.30 (на 2,3 дБ выше 0 dBFS), и любой
// проигрыватель, который просто пишет int16, режет такие отсчёты «в полку» —
// это слышно как короткий скрежет на громких местах (в нашем файле около 300
// участков по 2–5 мс). Лимитер вместо обрезания плавно убирает усиление.
//
// Как устроен: поток идёт блоками по LIM_BLOCK отсчётов, выход задержан ровно
// на один блок. Пока блок лежит в задержке, мы уже знаем пик следующего, так
// что усиление успевает съехать линейно по блоку и «полки» не возникает.
// Назад усиление возвращается медленно (LIM_RELEASE за блок), поэтому громкий
// участок не дёргает уровень соседних.
//
// Применять нужно к последнему вещественному звену тракта (после растяжения
// частоты): интерполяция сама может подкинуть промежуточный отсчёт выше пиков.

#include <stdint.h>

#define LIM_BLOCK    128       // ~5,8 мс при 22 кГц, ~2,9 мс при 44,1 кГц
#define LIM_MAX_CH   2
#define LIM_CEILING  0.98f     // куда целимся по пику (в долях полной шкалы)
#define LIM_RELEASE  0.25f     // доля возврата усиления за блок (~25 мс)

typedef struct {
    int   ch;
    float gain;                       // усиление, применённое к прошлому блоку
    float prev_target;                // предел прошлого блока (чтобы не превысить его при возврате)
    float delay[LIM_MAX_CH][LIM_BLOCK];
    int   filled;                     // в задержке уже есть блок
} limiter_t;

static inline void lim_init(limiter_t* l, int channels) {
    l->ch = (channels > LIM_MAX_CH) ? LIM_MAX_CH : (channels < 1 ? 1 : channels);
    l->gain = 1.0f;
    l->prev_target = 1.0f;
    l->filled = 0;
    for (int c = 0; c < LIM_MAX_CH; c++)
        for (int i = 0; i < LIM_BLOCK; i++) l->delay[c][i] = 0.0f;
}

static inline int16_t lim_clip(float v) {
    int s = (int)(v >= 0 ? v + 0.5f : v - 0.5f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

// Обработать n отсчётов (n кратно LIM_BLOCK). in — чередование каналов в шкале
// PCM (+-32768), out — int16 чередованием. Возвращает, сколько отсчётов готово:
// первый вызов отдаёт на один блок меньше (он ушёл в задержку).
static inline int lim_process(limiter_t* l, const float* in, int n, int16_t* out) {
    const float ceil_pcm = LIM_CEILING * 32768.0f;
    int done = 0;
    for (int b = 0; b + LIM_BLOCK <= n; b += LIM_BLOCK) {
        // 1. Пик следующего блока -- по нему решаем, какое усиление допустимо
        float peak = 0.0f;
        for (int i = 0; i < LIM_BLOCK; i++) {
            for (int c = 0; c < l->ch; c++) {
                float v = in[(b + i) * l->ch + c];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
        }
        float target = (peak > ceil_pcm) ? (ceil_pcm / peak) : 1.0f;
        if (target > l->gain) {                                  // возврат -- плавный
            target = l->gain + (target - l->gain) * LIM_RELEASE;
            if (target > l->prev_target) target = l->prev_target;  // не громче предела этого блока
        }

        // 2. Выдаём задержанный блок, линейно переводя усиление к цели
        if (l->filled) {
            for (int i = 0; i < LIM_BLOCK; i++) {
                float g = l->gain + (target - l->gain) * ((float)(i + 1) / (float)LIM_BLOCK);
                for (int c = 0; c < l->ch; c++)
                    out[done * l->ch + c] = lim_clip(l->delay[c][i] * g);
                done++;
            }
        }

        // 3. Новый блок -- в задержку
        for (int i = 0; i < LIM_BLOCK; i++)
            for (int c = 0; c < l->ch; c++) l->delay[c][i] = in[(b + i) * l->ch + c];
        l->filled = 1;
        l->gain = target;
        l->prev_target = (peak > ceil_pcm) ? (ceil_pcm / peak) : 1.0f;
    }
    return done;
}

// Дослать хвост (последний блок) в конце потока
static inline int lim_flush(limiter_t* l, int16_t* out) {
    if (!l->filled) return 0;
    for (int i = 0; i < LIM_BLOCK; i++)
        for (int c = 0; c < l->ch; c++) out[i * l->ch + c] = lim_clip(l->delay[c][i] * l->gain);
    l->filled = 0;
    return LIM_BLOCK;
}

#endif
