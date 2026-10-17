#ifndef AV_H
#define AV_H
// av.h — звук и видео одной библиотекой (/lib/ffmpeg.ll).
//
// Это ffmpeg overOS: программе не нужно знать ни про контейнер MP4, ни про
// H.264, ни про AAC, ни про буфер опорных кадров. Она открывает файл, получает
// параметры, забирает готовый PCM и готовые кадры картинки — всё остальное
// (разбор контейнера, декодирование, переупорядочение кадров по времени показа,
// растяжение частоты и лимитер) делает библиотека.
//
//     av_info_t info;
//     if (av_open("/video.mp4", &info) == 0) {
//         ... av_audio_read() -> в звуковой выход ...
//         av_video_decode_until(now_ms);
//         av_video_blit(px, w, h, x, y, out_w, out_h);
//     }
//
// За этим интерфейсом стоит настоящий FFmpeg, собранный под overOS (см.
// src/libs/ffmpeg и tools/build_ffmpeg.sh): MP4/MOV, MKV, AVI, MP3, WAV, видео
// H.264, звук AAC (включая HE-AAC с SBR) и PCM. Открыт может быть один файл за
// раз — этого хватает плееру, а состояние декодеров живёт в самой библиотеке.

#include <stdint.h>

#define AV_FMT_NONE 0
#define AV_FMT_WAV  1
#define AV_FMT_MP3  2
#define AV_FMT_MP4  3

typedef struct {
    int      fmt;                 // AV_FMT_*
    int      has_video;
    int      has_audio;
    int      audio_unsupported;   // звук есть, но кодек не наш (видео всё равно играет)
    int      width, height;       // размер картинки
    uint32_t duration_ms;
    int      rate, channels, bits;   // параметры готового PCM
    int      bitrate;             // кбит/с, если известно (MP3)
    char     codec[24];           // «H.264 / MP4», «MP3», «WAV PCM»
} av_info_t;

// Открыть файл. 0 — получилось, иначе отрицательный код (av_error_text).
int av_open(const char* path, av_info_t* info);
void av_close(void);
const char* av_error_text(int code);

// --- звук --------------------------------------------------------------------
// Выдать кусок готового PCM: *out указывает внутрь библиотеки, возвращается
// число байт (0 — данные кончились). После записи в звуковой выход вызывающий
// сообщает, сколько байт ушло, через av_audio_advance.
int  av_audio_read(const uint8_t** out, uint32_t max_bytes);
void av_audio_advance(uint32_t bytes);
int  av_at_end(void);

// --- видео -------------------------------------------------------------------
// Декодировать всё, что нужно к моменту ms, и выбрать кадр для показа.
// Возвращает 1, если кадр для показа есть.
int  av_video_decode_until(uint32_t ms);
// Вывести выбранный кадр в буфер ARGB: масштабирование ближайшим соседом,
// YUV -> RGB по BT.601. Возвращает 0, если показывать нечего.
int  av_video_blit(uint32_t* dst, int dst_w, int dst_h, int x, int y, int out_w, int out_h);
// Время показа следующего несжатого образца (для оценки конца файла)
int  av_video_finished(void);

// --- перемотка ---------------------------------------------------------------
// К ближайшему опорному кадру не позже ms. Возвращает время, с которого реально
// пойдёт воспроизведение.
uint32_t av_seek_ms(uint32_t ms);

#endif
