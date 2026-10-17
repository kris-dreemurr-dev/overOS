// =============================================================================
// avshim.c — /lib/ffmpeg.ll поверх настоящего FFmpeg.
//
// Снаружи библиотека осталась той же: программа зовёт av_open/av_audio_read/
// av_video_decode_until/av_video_blit из src/lib/av.h и ничего не знает ни про
// контейнеры, ни про кодеки. Внутри теперь не наши декодеры, а libavformat,
// libavcodec, libswscale и libswresample, собранные под overOS (см.
// tools/build_ffmpeg.sh и сисрут в src/libs/ffmpeg).
//
// Что из этого следует:
//   * форматов столько, сколько собрано в сборке FFmpeg, а не сколько мы
//     написали руками (MP4/MOV, MKV, AVI, MP3, WAV, чистый AAC и H.264);
//   * HE-AAC (SBR + parametric stereo) наконец играет полностью, а не одним
//     ядром AAC-LC в 22 кГц;
//   * масштабированием картинки и YUV->RGB занимается libswscale, то есть
//     сглаженно, а не ближайшим соседом.
//
// Файл читается через свой AVIOContext прямо сисколлами ядра: целиком в память
// он больше не загружается (раньше под это был буфер на 32 МБ).
//
// Потоков в overOS у библиотеки нет: FFmpeg собран без pthreads, декодирование
// идёт в том же процессе, который рисует кадр.
// =============================================================================

#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "av.h"
#include "ovos.h"
#include "limiter.h"

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"

#define IO_BUF      (64 * 1024)
#define PCM_CAP     (2 * 1024 * 1024)      // ~11 с стерео 44,1 кГц
#define STAGE_FR    8192                   // отсчётов на кадр звука с запасом

// --- состояние ---------------------------------------------------------------
static int               s_fd = -1;
static AVFormatContext*  s_fmt_ctx = 0;
static AVIOContext*      s_io = 0;
static unsigned char*    s_io_buf = 0;
static AVPacket*         s_pkt = 0;

static int               s_vstream = -1, s_astream = -1;
static AVCodecContext*   s_vdec = 0;
static AVCodecContext*   s_adec = 0;
static AVFrame*          s_vframe = 0;     // кадр, который показываем
static AVFrame*          s_atmp = 0;
static uint32_t          s_vcur_ms = 0;

// Очередь декодированных кадров. Раньше здесь был ровно один «следующий» кадр,
// и пока он ждал своего времени, decode() у видеодекодера не звали вовсе —
// очередь внутри libavcodec переполнялась, avcodec_send_packet отвечал EAGAIN,
// и ПАКЕТ выбрасывался. Для H.264 это дыра в потоке: дальше не декодируется
// ничего до следующего опорного кадра, и на экране застывает первый кадр,
// хотя звук идёт идеально. Кадр потерять можно (это просто пропуск), пакет —
// нельзя, поэтому теперь переполняется очередь кадров, а не декодера.
#define VQ_MAX 8
static AVFrame*          s_vq[VQ_MAX];
static uint32_t          s_vq_ms[VQ_MAX];
static int               s_vq_head = 0, s_vq_n = 0, s_vq_cap = VQ_MAX;

static struct SwsContext* s_sws = 0;
static int               s_sws_w = 0, s_sws_h = 0, s_sws_fmt = -1;
static int               s_sws_ow = 0, s_sws_oh = 0;

static SwrContext*       s_swr = 0;
static limiter_t         s_lim;
static float             s_stage[STAGE_FR * 2];
static int               s_stage_fr = 0;
static int16_t           s_pcm[PCM_CAP / 2];
static uint32_t          s_pcm_len = 0, s_pcm_pos = 0;   // в байтах

static int               s_eof = 0;
static int               s_fmt = AV_FMT_NONE;
static int               s_rate = 0, s_channels = 2, s_bits = 16;
static uint32_t          s_total_ms = 0;
static int               s_bitrate = 0;
static int               s_vw = 0, s_vh = 0;
static int               s_audio_unsupported = 0;
static char              s_codec[24];
static int               s_opened = 0;

// --- чтение файла: AVIOContext прямо на дескриптор ядра -----------------------
static int io_read(void* opaque, uint8_t* buf, int size) {
    (void)opaque;
    int64_t n = ov_read(s_fd, buf, (uint64_t)size);
    if (n < 0) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    return (int)n;
}

static int64_t io_seek(void* opaque, int64_t off, int whence) {
    (void)opaque;
    if (whence == AVSEEK_SIZE) return ov_fsize(s_fd);
    return ov_lseek(s_fd, off, whence & 3);
}

// --- вспомогательное ---------------------------------------------------------
static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void str_cat(char* dst, const char* src, int cap) {
    int i = 0;
    while (dst[i] && i < cap - 1) i++;
    while (src && *src && i < cap - 1) dst[i++] = *src++;
    dst[i] = '\0';
}

static uint32_t pts_to_ms(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE) return 0;
    int64_t ms = av_rescale_q(pts, tb, (AVRational){ 1, 1000 });
    return ms < 0 ? 0 : (uint32_t)ms;
}

static void pcm_compact(void) {
    if (!s_pcm_pos) return;
    uint32_t left = s_pcm_len - s_pcm_pos;
    if (left) memmove(s_pcm, (uint8_t*)s_pcm + s_pcm_pos, left);
    s_pcm_len = left;
    s_pcm_pos = 0;
}

// Звук из кадра декодера: swresample -> лимитер -> int16 в очередь.
// Весь тракт до последнего шага вещественный: материал из сети бывает громче
// полной шкалы, и округление в int16 до лимитера слышно как скрежет.
static void audio_frame(AVFrame* f) {
    if (!s_swr) return;
    const uint8_t** in = (const uint8_t**)f->extended_data;
    int in_fr = f->nb_samples;
    for (;;) {
        int room = STAGE_FR - s_stage_fr;
        if (room <= 0) break;
        uint8_t* out = (uint8_t*)&s_stage[s_stage_fr * 2];
        int got = swr_convert(s_swr, &out, room, in, in_fr);
        in = 0; in_fr = 0;                       // остаток добираем из самого swr
        if (got <= 0) break;
        s_stage_fr += got;
    }

    // Лимитер работает блоками по LIM_BLOCK и задерживает выход на один блок
    int done = s_stage_fr - (s_stage_fr % LIM_BLOCK);
    if (!done) return;
    for (int i = 0; i < done * 2; i++) s_stage[i] *= 32768.0f;

    if (s_pcm_len + (uint32_t)done * 4 > PCM_CAP) pcm_compact();
    if (s_pcm_len + (uint32_t)done * 4 > PCM_CAP) { s_stage_fr = 0; return; }

    int n = lim_process(&s_lim, s_stage, done, (int16_t*)((uint8_t*)s_pcm + s_pcm_len));
    s_pcm_len += (uint32_t)n * 4;

    int rest = s_stage_fr - done;
    for (int i = 0; i < rest * 2; i++) s_stage[i] = s_stage[done * 2 + i];
    s_stage_fr = rest;
}

// Выбросить самый старый кадр очереди (мы отстаём от звука)
static void vq_drop_oldest(void) {
    if (!s_vq_n) return;
    av_frame_unref(s_vq[s_vq_head]);
    s_vq_head = (s_vq_head + 1) % VQ_MAX;
    s_vq_n--;
}

static void vq_clear(void) {
    while (s_vq_n) vq_drop_oldest();
    s_vq_head = 0;
}

// Забрать всё готовое из декодера
static void drain(AVCodecContext* dec, int video) {
    for (;;) {
        if (video) {
            if (s_vq_n >= s_vq_cap) vq_drop_oldest();      // очередь полна — теряем старый кадр
            int slot = (s_vq_head + s_vq_n) % VQ_MAX;
            AVFrame* f = s_vq[slot];
            if (!f) return;
            av_frame_unref(f);
            if (avcodec_receive_frame(dec, f) < 0) return;
            s_vq_ms[slot] = pts_to_ms(f->best_effort_timestamp != AV_NOPTS_VALUE
                                          ? f->best_effort_timestamp : f->pts,
                                      s_fmt_ctx->streams[s_vstream]->time_base);
            s_vq_n++;
            continue;
        }
        AVFrame* f = s_atmp;
        if (!f) return;
        if (avcodec_receive_frame(dec, f) < 0) return;
        audio_frame(f);
        av_frame_unref(f);
    }
}

// Один шаг разбора: прочитать пакет и отдать его декодеру.
// 0 — файл кончился.
static int demux_step(void) {
    if (s_eof) {
        if (s_vdec) { avcodec_send_packet(s_vdec, 0); drain(s_vdec, 1); }
        if (s_adec) { avcodec_send_packet(s_adec, 0); drain(s_adec, 0); }
        return 0;
    }
    int r = av_read_frame(s_fmt_ctx, s_pkt);
    if (r < 0) { s_eof = 1; return 0; }
    if (s_pkt->stream_index == s_vstream && s_vdec) {
        // EAGAIN значит «сначала забери готовые кадры»: пакет при этом не
        // принят, и выбрасывать его нельзя — освобождаем место и повторяем.
        int r2 = avcodec_send_packet(s_vdec, s_pkt);
        if (r2 == AVERROR(EAGAIN)) { drain(s_vdec, 1); r2 = avcodec_send_packet(s_vdec, s_pkt); }
        if (r2 >= 0) drain(s_vdec, 1);
    } else if (s_pkt->stream_index == s_astream && s_adec) {
        int r2 = avcodec_send_packet(s_adec, s_pkt);
        if (r2 == AVERROR(EAGAIN)) { drain(s_adec, 0); r2 = avcodec_send_packet(s_adec, s_pkt); }
        if (r2 >= 0) drain(s_adec, 0);
    }
    av_packet_unref(s_pkt);
    return 1;
}

// --- открытие ----------------------------------------------------------------
static void shut_down(void) {
    if (s_sws) { sws_freeContext(s_sws); s_sws = 0; }
    if (s_swr) { swr_free(&s_swr); }
    if (s_vframe) av_frame_free(&s_vframe);
    for (int i = 0; i < VQ_MAX; i++) if (s_vq[i]) av_frame_free(&s_vq[i]);
    s_vq_head = s_vq_n = 0;
    s_vq_cap = VQ_MAX;
    if (s_atmp) av_frame_free(&s_atmp);
    if (s_pkt) av_packet_free(&s_pkt);
    if (s_vdec) avcodec_free_context(&s_vdec);
    if (s_adec) avcodec_free_context(&s_adec);
    if (s_fmt_ctx) avformat_close_input(&s_fmt_ctx);
    if (s_io) { av_freep(&s_io->buffer); avio_context_free(&s_io); }
    s_io_buf = 0;
    if (s_fd >= 0) { ov_close(s_fd); s_fd = -1; }
    s_vstream = s_astream = -1;
    s_pcm_len = s_pcm_pos = 0;
    s_stage_fr = 0;
    s_eof = 0;
    s_vw = s_vh = 0;
    s_sws_w = s_sws_h = s_sws_ow = s_sws_oh = 0;
    s_sws_fmt = -1;
    s_audio_unsupported = 0;
    s_opened = 0;
    s_fmt = AV_FMT_NONE;
}

static int open_decoder(int idx, AVCodecContext** out) {
    AVStream* st = s_fmt_ctx->streams[idx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return -1;
    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (!c) return -1;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0) { avcodec_free_context(&c); return -1; }
    c->thread_count = 1;                       // потоков в overOS у библиотеки нет
    c->pkt_timebase = st->time_base;
    if (avcodec_open2(c, dec, 0) < 0) { avcodec_free_context(&c); return -1; }
    *out = c;
    return 0;
}

// Имя кодека для окна: FFmpeg зовёт их «h264», «aac», «pcm_s16le» — человеку
// привычнее «H.264», «AAC», «PCM»
static const char* nice_codec(const char* n) {
    if (!n) return "";
    if (!strcmp(n, "h264")) return "H.264";
    if (!strcmp(n, "hevc")) return "H.265";
    if (!strcmp(n, "mpeg4")) return "MPEG-4";
    if (!strcmp(n, "aac") || !strcmp(n, "aac_latm")) return "AAC";
    if (!strcmp(n, "mp3") || !strcmp(n, "mp3float")) return "MP3";
    if (!strncmp(n, "pcm_", 4)) return "PCM";
    return n;
}

static int fmt_kind(const char* name, int has_video) {
    if (!name) return has_video ? AV_FMT_MP4 : AV_FMT_MP3;
    if (strstr(name, "wav")) return AV_FMT_WAV;
    if (strstr(name, "mp3")) return AV_FMT_MP3;
    return has_video ? AV_FMT_MP4 : AV_FMT_MP3;
}

int av_open(const char* path, av_info_t* info) {
    shut_down();
    av_log_set_level(AV_LOG_QUIET);             // журнал FFmpeg не нужен в окне плеера

    s_fd = ov_open(path, OV_O_RDONLY);
    if (s_fd < 0) return -1;
    if (ov_fsize(s_fd) <= 0) { shut_down(); return -2; }

    s_io_buf = (unsigned char*)av_malloc(IO_BUF);
    if (!s_io_buf) { shut_down(); return -10; }
    s_io = avio_alloc_context(s_io_buf, IO_BUF, 0, 0, io_read, 0, io_seek);
    if (!s_io) { shut_down(); return -10; }

    s_fmt_ctx = avformat_alloc_context();
    if (!s_fmt_ctx) { shut_down(); return -10; }
    s_fmt_ctx->pb = s_io;
    s_fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&s_fmt_ctx, "", 0, 0) < 0) { shut_down(); return -4; }
    if (avformat_find_stream_info(s_fmt_ctx, 0) < 0) { shut_down(); return -4; }

    s_vstream = av_find_best_stream(s_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    s_astream = av_find_best_stream(s_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    if (s_vstream < 0 && s_astream < 0) { shut_down(); return -4; }

    s_pkt = av_packet_alloc();
    if (!s_pkt) { shut_down(); return -10; }

    s_codec[0] = '\0';

    if (s_vstream >= 0) {
        if (open_decoder(s_vstream, &s_vdec) < 0) {
            s_vstream = -1;                                  // видео не наш кодек
        } else {
            s_vw = s_vdec->width;
            s_vh = s_vdec->height;
            s_vframe = av_frame_alloc();
            if (!s_vframe) { shut_down(); return -10; }
            // Глубина очереди — по размеру кадра: у библиотеки свой пул на 40 МБ,
            // и восемь кадров FullHD в него просто не влезут.
            int px = s_vw * s_vh;
            s_vq_cap = px > 0 ? (8 * 1024 * 1024) / (px * 3 / 2) : VQ_MAX;
            if (s_vq_cap > VQ_MAX) s_vq_cap = VQ_MAX;
            if (s_vq_cap < 2) s_vq_cap = 2;
            for (int i = 0; i < VQ_MAX; i++) {
                s_vq[i] = av_frame_alloc();
                if (!s_vq[i]) { shut_down(); return -10; }
            }
            s_vq_head = s_vq_n = 0;
            str_copy(s_codec, nice_codec(avcodec_get_name(s_vdec->codec_id)), sizeof(s_codec));
        }
    }

    if (s_astream >= 0) {
        if (open_decoder(s_astream, &s_adec) < 0) {
            s_astream = -1;
            s_audio_unsupported = 1;
        } else {
            s_rate = s_adec->sample_rate;
            s_channels = 2;
            s_bits = 16;
            s_atmp = av_frame_alloc();
            if (!s_atmp) { shut_down(); return -10; }
            AVChannelLayout out_l;
            av_channel_layout_default(&out_l, 2);
            if (swr_alloc_set_opts2(&s_swr, &out_l, AV_SAMPLE_FMT_FLT, s_rate,
                                    &s_adec->ch_layout, s_adec->sample_fmt,
                                    s_adec->sample_rate, 0, 0) < 0 ||
                swr_init(s_swr) < 0) { shut_down(); return -10; }
            lim_init(&s_lim, 2);
            if (s_codec[0]) str_cat(s_codec, " / ", sizeof(s_codec));
            str_cat(s_codec, nice_codec(avcodec_get_name(s_adec->codec_id)), sizeof(s_codec));
        }
    }

    s_fmt = fmt_kind(s_fmt_ctx->iformat ? s_fmt_ctx->iformat->name : 0, s_vstream >= 0);
    s_total_ms = (s_fmt_ctx->duration != AV_NOPTS_VALUE && s_fmt_ctx->duration > 0)
                     ? (uint32_t)(s_fmt_ctx->duration / 1000) : 0;
    s_bitrate = (int)(s_fmt_ctx->bit_rate / 1000);
    s_opened = 1;

    if (info) {
        for (unsigned i = 0; i < sizeof(*info); i++) ((uint8_t*)info)[i] = 0;
        info->fmt = s_fmt;
        info->has_video = (s_vstream >= 0);
        info->has_audio = (s_astream >= 0);
        info->audio_unsupported = s_audio_unsupported;
        info->width = s_vw;
        info->height = s_vh;
        info->duration_ms = s_total_ms;
        info->rate = s_rate;
        info->channels = s_channels;
        info->bits = s_bits;
        info->bitrate = s_bitrate;
        str_copy(info->codec, s_codec, (int)sizeof(info->codec));
    }
    return 0;
}

void av_close(void) { shut_down(); }

const char* av_error_text(int code) {
    switch (code) {
        case 0:   return "";
        case -1:  return "Файл не открывается";
        case -2:  return "Файл слишком короткий";
        case -4:  return "Не распознан формат файла";
        case -10: return "Не хватило памяти";
        default:  return "Не удалось открыть файл";
    }
}

// --- звук --------------------------------------------------------------------
int av_audio_read(const uint8_t** out, uint32_t max_bytes) {
    if (!s_opened || s_astream < 0) return 0;
    int guard = 0;
    while (s_pcm_pos >= s_pcm_len && guard++ < 64) {
        if (!demux_step()) break;
    }
    if (s_pcm_pos >= s_pcm_len) return 0;
    uint32_t avail = s_pcm_len - s_pcm_pos;
    uint32_t n = (avail < max_bytes) ? avail : max_bytes;
    n -= n % 4;                                   // стерео 16 бит
    if (!n) return 0;
    *out = (const uint8_t*)s_pcm + s_pcm_pos;
    return (int)n;
}

void av_audio_advance(uint32_t bytes) {
    s_pcm_pos += bytes;
    if (s_pcm_pos >= s_pcm_len) { s_pcm_pos = s_pcm_len = 0; }
}

int av_at_end(void) {
    if (!s_opened) return 1;
    if (s_astream < 0) return s_eof;
    return s_eof && s_pcm_pos >= s_pcm_len;
}

// --- видео -------------------------------------------------------------------
int av_video_finished(void) {
    if (!s_opened || s_vstream < 0) return 1;
    return s_eof && !s_vq_n;
}

int av_video_decode_until(uint32_t ms) {
    if (!s_opened || s_vstream < 0) return 0;
    int guard = 0;
    for (;;) {
        if (!s_vq_n) {
            if (guard++ > 256 || !demux_step()) break;
            continue;
        }
        if (s_vq_ms[s_vq_head] > ms && s_vframe->data[0]) break;   // следующему кадру ещё не время
        // Показываемый кадр меняем местами со слотом очереди: все AVFrame
        // остаются выделенными, лишних av_frame_alloc нет.
        AVFrame* t = s_vframe;
        s_vframe = s_vq[s_vq_head];
        s_vq[s_vq_head] = t;
        av_frame_unref(t);
        s_vcur_ms = s_vq_ms[s_vq_head];
        s_vq_head = (s_vq_head + 1) % VQ_MAX;
        s_vq_n--;
        if (s_vcur_ms > ms) break;                            // первый кадр показываем сразу
    }
    return s_vframe && s_vframe->data[0];
}

int av_video_blit(uint32_t* dst, int dst_w, int dst_h, int x, int y, int out_w, int out_h) {
    if (!s_opened || !s_vframe || !s_vframe->data[0]) return 0;
    // Прямоугольник ужимаем под буфер: лучше показать картинку на пару
    // пикселей меньше, чем не показать ничего (и тем более чем писать за край)
    if (x < 0) { out_w += x; x = 0; }
    if (y < 0) { out_h += y; y = 0; }
    if (x + out_w > dst_w) out_w = dst_w - x;
    if (y + out_h > dst_h) out_h = dst_h - y;
    if (out_w <= 0 || out_h <= 0) return 0;

    if (!s_sws || s_sws_w != s_vframe->width || s_sws_h != s_vframe->height ||
        s_sws_fmt != s_vframe->format || s_sws_ow != out_w || s_sws_oh != out_h) {
        if (s_sws) sws_freeContext(s_sws);
        s_sws = sws_getContext(s_vframe->width, s_vframe->height, s_vframe->format,
                               out_w, out_h, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, 0, 0, 0);
        if (!s_sws) return 0;
        s_sws_w = s_vframe->width;
        s_sws_h = s_vframe->height;
        s_sws_fmt = s_vframe->format;
        s_sws_ow = out_w;
        s_sws_oh = out_h;
    }

    uint8_t* planes[4] = { (uint8_t*)(dst + (int64_t)y * dst_w + x), 0, 0, 0 };
    int strides[4] = { dst_w * 4, 0, 0, 0 };
    sws_scale(s_sws, (const uint8_t* const*)s_vframe->data, s_vframe->linesize,
              0, s_vframe->height, planes, strides);

    // Окно клиента непрозрачное: альфа должна быть 255, иначе композитор
    // выведет кадр полупрозрачным
    for (int row = 0; row < out_h; row++) {
        uint32_t* p = dst + (int64_t)(y + row) * dst_w + x;
        for (int i = 0; i < out_w; i++) p[i] |= 0xFF000000u;
    }
    return 1;
}

// --- перемотка ---------------------------------------------------------------
uint32_t av_seek_ms(uint32_t ms) {
    if (!s_opened) return 0;
    if (s_total_ms && ms >= s_total_ms) ms = 0;
    int64_t ts = av_rescale_q((int64_t)ms, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
    if (av_seek_frame(s_fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) return s_vcur_ms;

    if (s_vdec) avcodec_flush_buffers(s_vdec);
    if (s_adec) avcodec_flush_buffers(s_adec);
    if (s_vframe) av_frame_unref(s_vframe);
    vq_clear();
    s_pcm_len = s_pcm_pos = 0;
    s_stage_fr = 0;
    s_eof = 0;
    lim_init(&s_lim, 2);

    // Доехать до первого кадра после перемотки, чтобы вернуть настоящее время
    int guard = 0;
    while (!s_vq_n && guard++ < 512 && demux_step()) { }
    if (s_vq_n) {
        AVFrame* t = s_vframe;
        s_vframe = s_vq[s_vq_head];
        s_vq[s_vq_head] = t;
        av_frame_unref(t);
        s_vcur_ms = s_vq_ms[s_vq_head];
        s_vq_head = (s_vq_head + 1) % VQ_MAX;
        s_vq_n--;
        return s_vcur_ms;
    }
    return ms;
}
