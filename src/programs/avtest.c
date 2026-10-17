// avtest — проверка загрузки /lib/ffmpeg.ll из консоли: печатает код ошибки.
#include "ovos.h"

// Своё преобразование числа в строку: libc в программах нет
static void itoa10(int v, char* out) {
    char tmp[16];
    int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (!u) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    int p = 0;
    if (neg) out[p++] = '-';
    while (n) out[p++] = tmp[--n];
    out[p] = '\0';
}
#include "ffmpeg/av.h"
#include "ffmpeg/ffmpeg_ll.h"

int main(void) {
    int r = av_bind();
    ov_print("av_bind = ");
    char b[16];
    itoa10(r, b);
    ov_print(b);
    ov_print("\n");
    if (r < 0) ov_exit(1);

    char arg[128];
    av_info_t info;
    if (ov_get_arg(arg, sizeof(arg)) > 0 && arg[0]) {
        int e = av_open(arg, &info);
        ov_print("av_open = ");
        itoa10(e, b);
        ov_print(b);
        ov_print(" (");
        ov_print(av_error_text(e));
        ov_print(")\n");
        if (e == 0) {
            ov_print("формат: ");
            ov_print(info.codec);
            ov_print(", видео ");
            itoa10(info.width, b); ov_print(b); ov_print("x");
            itoa10(info.height, b); ov_print(b);
            ov_print(", звук ");
            itoa10(info.rate, b); ov_print(b); ov_print(" Гц\n");
            // Заодно это и замер скорости: декодируем подряд и засекаем время.
            // Соотношение «мс декодирования на кадр» — то число, по которому
            // видно, тянет ли машина видео в реальном времени (для 25 кадров
            // в секунду нужно укладываться в 40 мс на кадр вместе с выводом).
            int frames = 0;
            uint64_t t0 = ov_ms();
            for (uint32_t t = 0; t < 8000 && frames < 200; t += 40)
                if (av_video_decode_until(t)) frames++;
            uint64_t dt = ov_ms() - t0;
            ov_print("кадров декодировано: ");
            itoa10(frames, b); ov_print(b);
            ov_print(" за ");
            itoa10((int)dt, b); ov_print(b);
            ov_print(" мс (");
            itoa10(frames ? (int)(dt / (uint64_t)frames) : 0, b); ov_print(b);
            ov_print(" мс на кадр)\n");

            // Картинка: сколько пикселей не чёрные (проверяем и сам вывод кадра)
            static uint32_t px[320 * 240];
            int lit = 0;
            if (av_video_blit(px, 320, 240, 0, 0, 320, 240)) {
                for (int i = 0; i < 320 * 240; i++) if ((px[i] & 0xFFFFFF) != 0) lit++;
            }
            ov_print("непустых пикселей кадра: ");
            itoa10(lit, b); ov_print(b); ov_print("\n");

            // Звук: сколько байт отдаёт библиотека и есть ли там не тишина
            uint32_t total = 0; int peak = 0;
            for (int i = 0; i < 200; i++) {
                const uint8_t* pcm = 0;
                int n = av_audio_read(&pcm, 8192);
                if (n <= 0) break;
                const int16_t* sm = (const int16_t*)pcm;
                for (int k = 0; k < n / 2; k++) {
                    int v = sm[k] < 0 ? -sm[k] : sm[k];
                    if (v > peak) peak = v;
                }
                total += (uint32_t)n;
                av_audio_advance((uint32_t)n);
            }
            ov_print("звук: байт ");
            itoa10((int)total, b); ov_print(b);
            ov_print(", пик ");
            itoa10(peak, b); ov_print(b); ov_print("\n");
        }
    }
    ov_exit(0);
    return 0;
}
