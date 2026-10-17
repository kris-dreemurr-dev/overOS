// =============================================================================
// overTerm — терминал рабочего стола overOS.
//
// Это настоящий эмулятор терминала, а не отдельная оболочка со своим разбором
// команд. Окно открывает ПСЕВДОТЕРМИНАЛ ядра (сисколл 123): обычный TTY
// системы, такой же как tty1..tty6, только его картинка никуда не выводится.
// На нём работает та же /sys/sh.prg, что и в текстовой консоли, поэтому здесь
// есть всё — ls, ps, su, sudo, useradd, fastfetch, запуск программ.
//
// Задача окна проста и ровно та же, что у xterm:
//   1. открыть псевдотерминал размером с текстовую область окна;
//   2. каждый кадр забирать его картинку (ov_pty_read) и выводить её;
//   3. пересылать туда скан-коды клавиш (ov_pty_key) — раскладку, Shift,
//      Backspace и эхо делает драйвер TTY в ядре, как и положено.
//
// Размер консоли задаётся в знакоместах 8x16 шрифта ядра, поэтому при
// изменении размера окна псевдотерминал открывается заново.
// =============================================================================

#include "common/ovos.h"
#include "libui/ui.h"
#include "libui/libui_ll.h"

static const uint16_t icon_term[16] = {
    0x0000, 0x7FFE, 0x4002, 0x4002, 0x4882, 0x4482, 0x4282, 0x4482,
    0x4882, 0x4002, 0x47E2, 0x4002, 0x4002, 0x7FFE, 0x0000, 0x0000
};

// Отступ вокруг текста, как в Konsole
#define PAD_X 6
#define PAD_Y 4

// Потолок картинки консоли: 1600x1000 точек хватает на любое наше окно
#define PTY_MAX_PX (1600 * 1000)
static uint32_t s_screen[PTY_MAX_PX];

static int s_pty = -1;
static int s_pty_w, s_pty_h;

static void pty_drop(void) {
    if (s_pty >= 0) { ov_pty_close(s_pty); s_pty = -1; }
}

// Открыть псевдотерминал под текущий размер окна (в целых знакоместах)
static int pty_fit(int win_w, int win_h) {
    int w = (win_w - PAD_X * 2) & ~7;
    int h = (win_h - PAD_Y * 2) & ~15;
    if (w < 160) w = 160;
    if (h < 64)  h = 64;
    if ((long)w * h > PTY_MAX_PX) {                 // окно больше, чем мы готовы рисовать
        h = PTY_MAX_PX / w;
        h &= ~15;
    }
    if (s_pty >= 0 && w == s_pty_w && h == s_pty_h) return 1;

    pty_drop();
    s_pty = ov_pty_open(w, h);
    if (s_pty < 0) return 0;
    s_pty_w = w;
    s_pty_h = h;
    return 1;
}

int main(void) {
    if (libui_bind() < 0) ov_exit(1);
    ui_app_id("ovterm");

    ui_win_t* w = ui_open("Терминал", 820, 520, icon_term, WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    int keys[32];
    int warned = 0;

    // Кадры идут по таймеру: оболочка печатает сама по себе (вывод программы,
    // приглашение), и ждать события от окна было бы неправильно.
    while (ui_begin(w, 40)) {
        int win_w = ui_width(w), win_h = ui_height(w);

        if (!pty_fit(win_w, win_h)) {
            ui_clear(w);
            if (!warned) warned = 1;
            ui_text(w, 16, 16, "Не удалось открыть псевдотерминал.", UI_DANGER);
            ui_label_muted(w, 16, 40, "Свободных слотов нет — закройте другое окно терминала.");
            ui_end(w);
            continue;
        }

        // Клавиши — прямо в очередь TTY, вместе с отпусканиями: Shift, Ctrl и
        // раскладку разбирает драйвер ядра, ровно как в текстовой консоли.
        int n = ui_raw_keys(w, keys, 32);
        for (int i = 0; i < n; i++) {
            int sc = keys[i];
            // Расширенные клавиши (стрелки, Delete) композитор отдаёт как
            // 0x100 | код -- в TTY они идут парой 0xE0 + код, иначе история
            // по стрелкам работала бы только в текстовой консоли.
            if (sc & 0x100) ov_pty_key(s_pty, 0xE0);
            ov_pty_key(s_pty, (unsigned char)(sc & 0xFF));
        }

        // Оболочка закончилась (пользователь набрал exit) — закрываем окно
        if (!ov_pty_alive(s_pty)) break;

        ui_clear(w);
        int got = ov_pty_read(s_pty, s_screen, sizeof(s_screen));
        if (got > 0) {
            ui_blit(w, PAD_X, PAD_Y, s_pty_w, s_pty_h, s_screen, s_pty_w);
        }
        ui_end(w);
    }

    pty_drop();
    ui_close(w);
    ov_exit(0);
    return 0;
}
