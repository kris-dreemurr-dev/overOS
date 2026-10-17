// overPlayer — проигрыватель overOS: WAV, MP3 и видео H.264 в MP4 со звуком.
//
// Сама программа ничего не декодирует: контейнер, видео, звук и перемотку
// делает /lib/ffmpeg.ll (src/lib/av.h), а тулкит окон берётся из /lib/libui.ll.
// Здесь остаются окно, управление воспроизведением, часы и вывод кадра.

#include "libui/ui.h"
#include "libui/libui_ll.h"
#include "ffmpeg/av.h"
#include "ffmpeg/ffmpeg_ll.h"

#define WIN_W 480
#define WIN_H 196

// Звуковой выход (syscall 70): команды драйвера HDA
#define ACTL_PCM_OPEN     7
#define ACTL_PCM_WRITE    8
#define ACTL_PCM_CLOSE    9
#define ACTL_FLUSH       10
#define ACTL_POSITION_MS 11
#define ACTL_DRAINED     13

// Ядро ждёт частоту отдельным аргументом, а каналы и разрядность — упакованными
// во втором (user_mode.c, syscall 70 подкоманда 7). Плеер раньше складывал всё в
// один аргумент, и открытие звука возвращало -3 «неверные параметры».
static int sys_audio_open(uint32_t rate, int channels, int bits) {
    return (int)ov_audio(ACTL_PCM_OPEN, (uint64_t)rate, (uint64_t)channels | ((uint64_t)bits << 8));
}
static int  sys_audio_write(const void* data, uint32_t bytes) { return (int)ov_audio(ACTL_PCM_WRITE, (uint64_t)data, bytes); }
static void sys_audio_close(void)                             { ov_audio(ACTL_PCM_CLOSE, 0, 0); }
static int  sys_audio_ctl(int cmd, int arg)                   { return (int)ov_audio((uint64_t)cmd, (uint64_t)(int64_t)arg, 0); }

static const uint16_t icon_sound_player[16] = {
    0b0000000000000000, 0b0000001100000000, 0b0000011100001000, 0b0000111100010100,
    0b0011111100100010, 0b0111111100100010, 0b0111111100010100, 0b0111111100001000,
    0b0111111100000000, 0b0111111100001000, 0b0111111100010100, 0b0011111100100010,
    0b0000111100010100, 0b0000011100001000, 0b0000001100000000, 0b0000000000000000
};

#define VID_AREA_W 760                     // размер картинки по умолчанию
#define VID_AREA_H 430

static char       s_filepath[128] = "/speech.wav";
static av_info_t  s_info;
static int        s_err = 0;
static int        s_disp_w = 0, s_disp_h = 0;    // размер картинки на экране

static int        s_is_playing = 0, s_is_paused = 0;
static uint32_t   s_curr_ms = 0, s_base_ms = 0;
static uint64_t   s_play_start = 0;
static int        s_audio_open = 0, s_audio_err = 0;

static void append(char* dst, int* p, const char* s) { while (*s) dst[(*p)++] = *s++; dst[*p] = '\0'; }

static int have_audio(void)  { return s_info.has_audio && !s_info.audio_unsupported; }
static int is_video(void)    { return s_info.has_video; }

// Вписать картинку в отведённую область с сохранением пропорций
static void fit_video(int area_w, int area_h) {
    s_disp_w = s_info.width;
    s_disp_h = s_info.height;
    if (s_disp_w <= 0 || s_disp_h <= 0) { s_disp_w = s_disp_h = 0; return; }
    if (s_disp_w > area_w) { s_disp_h = s_disp_h * area_w / s_disp_w; s_disp_w = area_w; }
    if (s_disp_h > area_h) { s_disp_w = s_disp_w * area_h / s_disp_h; s_disp_h = area_h; }
}

static int open_file(const char* path) {
    av_close();
    s_err = av_open(path, &s_info);
    if (s_err != 0) {
        s_info.fmt = AV_FMT_NONE;
        s_disp_w = s_disp_h = 0;
        return s_err;
    }
    fit_video(VID_AREA_W, VID_AREA_H);
    return 0;
}

// ============================================================================
// Воспроизведение
// ============================================================================
static void audio_close(void) {
    if (s_audio_open) { sys_audio_close(); s_audio_open = 0; }
}

static int audio_start_at(uint32_t ms) {
    if (s_info.fmt == AV_FMT_NONE) return -1;
    uint32_t real = av_seek_ms(ms);
    s_base_ms = real;
    s_curr_ms = real;
    s_play_start = ov_ms();
    if (!have_audio()) return 0;                  // видео без звука идёт по часам
    if (!s_audio_open) {
        int r = sys_audio_open((uint32_t)s_info.rate, s_info.channels, s_info.bits);
        if (r < 0) { s_audio_err = r; return is_video() ? 0 : r; }
        s_audio_open = 1;
        s_audio_err = 0;
    } else {
        sys_audio_ctl(ACTL_FLUSH, 0);
    }
    return 0;
}

static void audio_pump(void) {
    if (!s_audio_open) return;
    for (int guard = 0; guard < 64; guard++) {
        const uint8_t* p = 0;
        int n = av_audio_read(&p, 8192);
        if (n <= 0) break;
        int wrote = sys_audio_write(p, (uint32_t)n);
        if (wrote <= 0) break;
        av_audio_advance((uint32_t)wrote);
        if (wrote < n) break;                      // очередь драйвера заполнена
    }
}

static void stop_play(void) {
    audio_close();
    s_is_playing = 0;
    s_is_paused = 0;
    s_curr_ms = 0;
}

static void start_play(void) {
    if (s_info.fmt == AV_FMT_NONE) return;
    if (s_curr_ms + 10 >= s_info.duration_ms) s_curr_ms = 0;
    if (audio_start_at(s_curr_ms) == 0) { s_is_playing = 1; s_is_paused = 0; }
}

static void pause_play(void) {
    if (!s_is_playing) return;
    if (is_video()) {
        s_curr_ms = s_base_ms + (uint32_t)(ov_ms() - s_play_start);
        s_is_playing = 0;
        s_is_paused = 1;
        return;
    }
    s_curr_ms = s_base_ms + (uint32_t)sys_audio_ctl(ACTL_POSITION_MS, 0);
    audio_close();
    s_is_playing = 0;
    s_is_paused = 1;
}

static void toggle_play_stop(void) { if (s_is_playing) stop_play(); else start_play(); }
static void toggle_pause(void)     { if (s_is_playing) pause_play(); else if (s_is_paused) start_play(); }

// Геометрия окна под текущий файл: под звук компактная, под видео — по картинке
static void layout(int* win_w, int* win_h, int* video_y, int* ctl_y) {
    *win_w = WIN_W; *win_h = WIN_H; *video_y = 0;
    if (is_video() && s_disp_w > 0) {
        *win_w = s_disp_w + 24;
        if (*win_w < WIN_W) *win_w = WIN_W;
        *video_y = 41;
        *win_h = *video_y + s_disp_h + 100;
    }
    *ctl_y = (is_video() && s_disp_w > 0) ? (*video_y + s_disp_h + 10) : 84;
}

int main(void) {
    // Декодеры и тулкит — разделяемые библиотеки, а не часть программы
    if (av_bind() < 0 || libui_bind() < 0) ov_exit(1);
    char arg[128];
    if (ov_get_arg(arg, sizeof(arg)) > 0 && arg[0]) ov_strlcpy(s_filepath, arg, sizeof(s_filepath));
    open_file(s_filepath);

    // Единственный экземпляр: следующий файл из overFiles придёт сюда (WP_EV_OPEN),
    // а не откроет второе окно плеера.
    ui_app_id("ovplayer");

    int alive = 1;
    while (alive) {
    int win_w, win_h, video_y, ctl_y;
    layout(&win_w, &win_h, &video_y, &ctl_y);

    ui_win_t* w = ui_open(is_video() ? "ovPlayer — Видео" : "ovPlayer — Аудиоплеер",
                          win_w, win_h, icon_sound_player, WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    int seek_ms = 0, seeking = 0, reopen = 0;

    while (ui_begin(w, s_is_playing ? 16 : UI_WAIT)) {
        // Окно могли развернуть или потянуть за край — картинка подстраивается
        win_w = ui_width(w);
        win_h = ui_height(w);
        if (is_video()) {
            fit_video(win_w - 24, win_h - 141);
            video_y = 41;
            ctl_y = video_y + s_disp_h + 10;
        } else {
            ctl_y = 84;
        }

        // Нам передали другой файл (композитор нашёл уже открытый плеер)
        const char* req = ui_open_request(w);
        if (req[0]) {
            if (s_is_playing || s_is_paused) stop_play();
            audio_close();
            ov_strlcpy(s_filepath, req, sizeof(s_filepath));
            open_file(s_filepath);
            seeking = 0;
            int nw, nh, nvy, ncy;
            layout(&nw, &nh, &nvy, &ncy);
            if (nw != win_w || nh != win_h) { reopen = 1; break; }   // размер окна поменялся
            video_y = nvy; ctl_y = ncy;
            ui_set_title(w, is_video() ? "ovPlayer — Видео" : "ovPlayer — Аудиоплеер");
        }

        uint32_t dur = s_info.duration_ms;

        if (s_is_playing && is_video()) {
            // Со звуком ведущие часы — позиция в звуковом потоке, иначе системные
            if (have_audio() && s_audio_open) audio_pump();
            uint32_t elapsed = s_base_ms + (uint32_t)((have_audio() && s_audio_open)
                                 ? (uint32_t)sys_audio_ctl(ACTL_POSITION_MS, 0)
                                 : (uint32_t)(ov_ms() - s_play_start));
            av_video_decode_until(elapsed);   // кадр выбирается по времени показа (pts)
            s_curr_ms = (elapsed > dur) ? dur : elapsed;
            if (av_video_finished() && elapsed >= dur) stop_play();
        } else if (s_is_playing) {
            audio_pump();
            uint32_t pos = s_base_ms + (uint32_t)sys_audio_ctl(ACTL_POSITION_MS, 0);
            s_curr_ms = pos > dur ? dur : pos;
            if (av_at_end() && sys_audio_ctl(ACTL_DRAINED, 0)) stop_play();
        }

        if (ui_key_pressed(w, WP_KEY_ESC)) break;
        if (ui_key_pressed(w, WP_KEY_SPACE)) {
            if (s_is_playing || s_is_paused) toggle_pause(); else toggle_play_stop();
        }

        ui_clear(w);
        ui_fill(w, 0, 0, win_w, 40, UI_PANEL);
        ui_hline(w, 0, 40, win_w, UI_BORDER);
        ui_icon16(w, 14, 12, icon_sound_player, UI_ACCENT);
        ui_label(w, 40, 12, s_filepath);

        if (is_video()) {
            // Чёрный фон под картинкой, чтобы поля не мигали
            ui_fill(w, 0, video_y, win_w, s_disp_h, 0x000000);
            av_video_blit(ui_pixels(w), win_w, win_h, (win_w - s_disp_w) / 2, video_y, s_disp_w, s_disp_h);
        } else if (s_info.fmt != AV_FMT_NONE) {
            char info[80], num[12];
            int p = 0;
            info[0] = '\0';
            ui_itoa(s_info.rate, num);     append(info, &p, num); append(info, &p, " Гц · ");
            ui_itoa(s_info.bits, num);     append(info, &p, num); append(info, &p, " бит · ");
            append(info, &p, s_info.channels == 2 ? "стерео · " : "моно · ");
            append(info, &p, s_info.codec);
            if (s_info.bitrate > 0) {
                append(info, &p, " ");
                ui_itoa(s_info.bitrate, num); append(info, &p, num);
                append(info, &p, " кбит/с");
            }
            ui_label_muted(w, 20, 54, info);
        } else {
            ui_text(w, 20, 54, av_error_text(s_err), UI_DANGER);
        }

        int shown = seeking ? seek_ms : (int)s_curr_ms;
        int st = ui_slider(w, 20, ctl_y, win_w - 40, &shown, 0, dur ? (int)dur : 1);
        if (st == UI_SLIDER_DRAGGING) { seeking = 1; seek_ms = shown; }
        if (st == UI_RELEASED) {
            seeking = 0;
            s_curr_ms = (uint32_t)shown;
            if (s_is_playing) audio_start_at(s_curr_ms);
            else av_seek_ms(s_curr_ms);
        }

        char t_cur[8], t_dur[8];
        ui_fmt_time((uint32_t)shown, t_cur);
        ui_fmt_time(dur, t_dur);
        ui_label(w, 20, ctl_y + 24, t_cur);
        ui_label_muted(w, win_w - 20 - ui_text_width(t_dur), ctl_y + 24, t_dur);

        int by = ctl_y + 52;
        if (ui_button_accent(w, 20, by, 110, 30, s_is_playing ? "Стоп" : "Играть")) toggle_play_stop();
        if (ui_button(w, 140, by, 130, 30, s_is_paused ? "Продолжить" : "Пауза") && (s_is_playing || s_is_paused))
            toggle_pause();

        const char* status;
        uint32_t scol;
        if (s_info.audio_unsupported) { status = "Видео без звука (кодек не разобран)"; scol = UI_TEXT_MUTED; }
        else if (s_audio_err < 0) { status = "Ошибка звука"; scol = UI_DANGER; }
        else if (s_is_playing)    { status = "Играет"; scol = UI_SUCCESS; }
        else if (s_is_paused)     { status = "Пауза"; scol = UI_TEXT_MUTED; }
        else                      { status = "Остановлено"; scol = UI_TEXT_MUTED; }
        ui_text(w, win_w - 20 - ui_text_width(status), by + 7, status, scol);

        ui_end(w);
    }

    audio_close();
    av_close();
    ui_close(w);
    if (!reopen) alive = 0;      // окно закрыли, а не пересоздаём под новый файл
    }
    ov_exit(0);
    return 0;
}
