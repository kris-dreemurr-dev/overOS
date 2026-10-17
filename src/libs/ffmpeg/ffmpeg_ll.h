#ifndef FFMPEG_LL_H
#define FFMPEG_LL_H
// ffmpeg_ll.h — подключение /lib/ffmpeg.ll (звук и видео) к программе.
//
// Программа включает av.h ради типов, затем этот файл и зовёт av_bind() в
// начале main. Дальше вызовы av_* идут через таблицу указателей — имена
// подменяются макросами, поэтому код программы остаётся обычным:
//
//     #include "av.h"
//     #include "ffmpeg_ll.h"
//     int main(void) { if (av_bind() < 0) ov_exit(1); av_open("/x.mp4", &info); ... }
//
// Внутри библиотеки лежат MP4, H.264, AAC, MP3 и WAV: программе не нужно ни
// одного декодера своей сборкой.

#include "ovos.h"
#include "av.h"

#define FFMPEG_PATH "/lib/ffmpeg.ll"

typedef struct {
    int lib;
    int (*open)(const char* path, av_info_t* info);
    void (*close)(void);
    const char* (*error_text)(int code);
    int (*audio_read)(const uint8_t** out, uint32_t max_bytes);
    void (*audio_advance)(uint32_t bytes);
    int (*at_end)(void);
    int (*video_decode_until)(uint32_t ms);
    int (*video_blit)(uint32_t* dst, int dst_w, int dst_h, int x, int y, int out_w, int out_h);
    int (*video_finished)(void);
    uint32_t (*seek_ms)(uint32_t ms);
} av_api_t;

static av_api_t g_av;

// 0 — библиотека загружена, -1 — нет файла, -2 — не нашлось символа
static inline int av_bind(void) {
    for (unsigned i = 0; i < sizeof(g_av); i++) ((char*)&g_av)[i] = 0;
    int h = ov_lib_open(FFMPEG_PATH);
    if (h < 0) return -1;
    g_av.lib = h;
    OV_BIND(h, g_av.open, "av_open");
    OV_BIND(h, g_av.close, "av_close");
    OV_BIND(h, g_av.error_text, "av_error_text");
    OV_BIND(h, g_av.audio_read, "av_audio_read");
    OV_BIND(h, g_av.audio_advance, "av_audio_advance");
    OV_BIND(h, g_av.at_end, "av_at_end");
    OV_BIND(h, g_av.video_decode_until, "av_video_decode_until");
    OV_BIND(h, g_av.video_blit, "av_video_blit");
    OV_BIND(h, g_av.video_finished, "av_video_finished");
    OV_BIND(h, g_av.seek_ms, "av_seek_ms");
    const void* const* p = (const void* const*)&g_av;
    for (unsigned i = 1; i < sizeof(g_av) / sizeof(void*); i++) if (!p[i]) return -2;
    return 0;
}

#define av_open               g_av.open
#define av_close              g_av.close
#define av_error_text         g_av.error_text
#define av_audio_read         g_av.audio_read
#define av_audio_advance      g_av.audio_advance
#define av_at_end             g_av.at_end
#define av_video_decode_until g_av.video_decode_until
#define av_video_blit         g_av.video_blit
#define av_video_finished     g_av.video_finished
#define av_seek_ms            g_av.seek_ms

#endif
