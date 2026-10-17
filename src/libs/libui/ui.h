#ifndef UI_H
#define UI_H

// =============================================================================
// libui — тулкит overOS для оконных приложений.
//
// Приложение не общается с композитором и не рисует рамки/кнопки попиксельно: оно
// описывает интерфейс виджетами в каждом кадре (immediate mode, как microui/Dear ImGui):
//
//     ui_win_t* w = ui_open("Плеер", 480, 200, icon, 0);
//     while (ui_begin(w, playing ? 33 : UI_WAIT)) {      // ждём ввод или таймер
//         ui_clear(w);
//         ui_label(w, 16, 16, "Трек: speech.wav");
//         if (ui_button(w, 16, 120, 90, 28, "Play")) start();
//         if (ui_slider(w, 16, 80, 440, &pos, 0, len) == UI_RELEASED) seek(pos);
//         ui_end(w);                                       // кадр — композитору
//     }
//
// ui_begin возвращает 0, когда пользователь закрыл окно, — программа завершается.
// Буфер окна лежит в разделяемой памяти: композитор берёт кадр прямо из него.
// Цвета и метрики — общие (тема Breeze), чтобы все приложения выглядели одинаково.
// =============================================================================

#include <stdint.h>
#include "ovos.h"
#include "wproto.h"

#define UI_WAIT OV_WAIT_FOREVER

// Тема (как у Desktop)
#define UI_BG           0x232629
#define UI_BG_DARK      0x1B1E20
#define UI_PANEL        0x2A2E32
#define UI_HOVER        0x353B42
#define UI_BORDER       0x3B4148
#define UI_TEXT         0xFCFCFC
#define UI_TEXT_MUTED   0x8F98A0
#define UI_ACCENT       0x3DAEE9
#define UI_ACCENT_HOVER 0x5BBEF0
#define UI_DANGER       0xDA4453
#define UI_SUCCESS      0x27AE60

// Шрифт интерфейса теперь переменной ширины (src/lib/font_ui.h, см. uifont.h):
// UI_FONT_W — только средний шаг для грубых прикидок, точную ширину строки
// даёт ui_text_width(). UI_FONT_H — высота строки для вертикального центра.
#define UI_FONT_W 8
#define UI_FONT_H 16
#define UI_MONO_W 8      // ширина символа моноширинного шрифта (ui_text_mono)

typedef struct ui_win ui_win_t;

// Окно. NULL — композитор не запущен или нет памяти.
ui_win_t* ui_open(const char* title, int w, int h, const uint16_t* icon16, uint32_t flags);
void      ui_close(ui_win_t* w);
void      ui_set_title(ui_win_t* w, const char* title);
int       ui_width(ui_win_t* w);
int       ui_height(ui_win_t* w);
uint32_t* ui_pixels(ui_win_t* w);

// Кадр: ждёт событие или timeout_ms. 0 — окно закрыто (выйти из программы).
int  ui_begin(ui_win_t* w, uint32_t timeout_ms);
void ui_end(ui_win_t* w);
// Попросить следующий кадр сразу (анимация), даже если событий нет
void ui_request_frame(ui_win_t* w);
// Окно каждый кадр перерисовывает себя целиком (игра, видео): ui_end не
// переносит показанный кадр во вторую половину буфера. Для окна во весь
// экран это несколько мегабайт копирования на кадр.
void ui_redraw_all(ui_win_t* w);

// --- Рисование (координаты клиентской области) ---
void ui_clear(ui_win_t* w);
void ui_fill(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color);
void ui_frame(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color);   // контур 1px
void ui_hline(ui_win_t* w, int x, int y, int len, uint32_t color);
void ui_text(ui_win_t* w, int x, int y, const char* utf8, uint32_t color);
// Моноширинный вывод (растровый 8x16): текстовый редактор, таблицы, код
void ui_text_mono(ui_win_t* w, int x, int y, const char* utf8, uint32_t color);
int  ui_text_width(const char* utf8);
void ui_text_center(ui_win_t* w, int x, int y, int ww, const char* utf8, uint32_t color);
void ui_icon16(ui_win_t* w, int x, int y, const uint16_t* mask, uint32_t color);
void ui_blit(ui_win_t* w, int x, int y, int ww, int hh, const uint32_t* px, int src_stride);

// --- Виджеты (возвращают 1, если пользователь их активировал) ---
void ui_label(ui_win_t* w, int x, int y, const char* utf8);
void ui_label_muted(ui_win_t* w, int x, int y, const char* utf8);
int  ui_button(ui_win_t* w, int x, int y, int ww, int hh, const char* label);
int  ui_button_accent(ui_win_t* w, int x, int y, int ww, int hh, const char* label);
int  ui_checkbox(ui_win_t* w, int x, int y, const char* label, int* value);

#define UI_SLIDER_IDLE     0
#define UI_SLIDER_DRAGGING 1     // значение меняется прямо сейчас
#define UI_RELEASED        2     // отпустили — значение окончательное
int  ui_slider(ui_win_t* w, int x, int y, int ww, int* value, int min, int max);
void ui_progress(ui_win_t* w, int x, int y, int ww, int hh, int value, int max);

// Однострочное поле ввода. 1 — нажали Enter.
int  ui_textbox(ui_win_t* w, int x, int y, int ww, char* buf, int cap);
// То же для пароля: символы не показываются
int  ui_password(ui_win_t* w, int x, int y, int ww, char* buf, int cap);
// Отдать фокус ввода следующему вызванному полю (например, при открытии диалога)
void ui_focus_next_textbox(ui_win_t* w);
// Список строк с прокруткой (колесо, стрелки). 1 — элемент активирован (двойной клик / Enter).
int  ui_list(ui_win_t* w, int x, int y, int ww, int hh, const char* const* items, int count,
             int* selected, int* scroll);

// --- Ввод текущего кадра (для своих виджетов/игр) ---
int         ui_mouse_x(ui_win_t* w);
int         ui_mouse_y(ui_win_t* w);
int         ui_mouse_down(ui_win_t* w);                 // ЛКМ зажата
int         ui_mouse_clicked(ui_win_t* w);              // ЛКМ нажата в этом кадре
int         ui_double_clicked(ui_win_t* w);
int         ui_scroll(ui_win_t* w);                     // щелчки колеса за кадр (+ вниз)
// Захват мыши (игры): курсор прячется, движение приходит смещениями без упора в
// край экрана. Композитор может снять захват сам (Meta, другое окно) —
// ui_grabbed это покажет.
void        ui_grab(ui_win_t* w, int on);
int         ui_grabbed(ui_win_t* w);
void        ui_motion(ui_win_t* w, int* dx, int* dy);   // смещение захваченной мыши за кадр
int         ui_buttons(ui_win_t* w);                    // кнопки: бит 0 — левая, 1 — правая, 2 — средняя
int         ui_key_pressed(ui_win_t* w, int scancode);  // WP_KEY_* / скан-код нажат в этом кадре
// Все скан-коды кадра как они пришли от композитора, вместе с отпусканиями
// (бит 0x80). Нужно эмулятору терминала: он пересылает их в псевдотерминал,
// а раскладку, Shift и эхо делает, как в Linux, сам драйвер TTY.
int         ui_raw_keys(ui_win_t* w, int* out, int max);
int         ui_mods(ui_win_t* w);                       // WP_MOD_*
const char* ui_typed(ui_win_t* w);                      // напечатанный за кадр текст UTF-8
int         ui_hit(ui_win_t* w, int x, int y, int ww, int hh);
int         ui_focused(ui_win_t* w);
int         ui_command(ui_win_t* w);                    // WP_CMD_* из меню композитора, 0 — нет

// --- Единственный экземпляр программы (как activation в XDG) ---
// Класс программы задаётся до ui_open; композитор запоминает его для окна.
void        ui_app_id(const char* id);
// Путь из WP_EV_OPEN за этот кадр ("" — ничего не просили открыть)
const char* ui_open_request(ui_win_t* w);
// Отдать путь уже работающей программе класса app_id. 1 — отдали, 0 — её нет.
int         ui_hand_over(const char* app_id, const char* path);

// --- Курсор --------------------------------------------------------------
// Какой курсор показывать над окном (WP_CUR_* из wproto.h): текстовое поле
// просит WP_CUR_TEXT, долгая операция — WP_CUR_BUSY. Края окна композитор
// подставляет сам. Тулкит сбрасывает форму в стрелку в начале каждого кадра,
// поэтому «пока наведено» достаточно звать это каждый кадр.
void ui_cursor(ui_win_t* w, int shape);

// --- Значки -------------------------------------------------------------
// SVG-значок размером size: name -- имя системного значка (/sys/icons/<name>.svg)
// или путь к программе ("/desprogs/ovfiles.prg" -- её значок из ресурсов .prg).
// Монохромные значки окрашиваются в tint, цветные рисуются как есть (tint =
// 0xFFFFFF). Растеризуется один раз и кэшируется. 1 -- нарисован, 0 -- нет такого.
int ui_icon(ui_win_t* w, int x, int y, int size, const char* name, uint32_t tint);

// --- Уведомления ----------------------------------------------------------
// Послать -- ov_notify() из common/ovnotify.h (окно для этого не нужно).
// Нажатую кнопку уведомления узнаём здесь: номер кнопки (0/1), -1 -- щелчок
// по самому уведомлению, -2 -- ничего не нажимали. *id -- id уведомления.
int ui_notify_action(ui_win_t* w, uint32_t* id);

// Небольшие утилиты форматирования без libc
void ui_itoa(int v, char* out);
void ui_fmt_time(uint32_t ms, char* out);   // "мм:сс"

#endif
