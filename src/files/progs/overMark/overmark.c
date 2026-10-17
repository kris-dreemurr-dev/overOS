// =============================================================================
// overMark — нагрузочный тест процессора в духе FurMark.
//
// «Пушистый бублик» (burn.c) трассируется лучами на ВСЕХ ядрах сразу: основной
// поток и рабочие ядра (ov_worker_start) берут полосы кадра по одной, пока
// они не кончатся, — ядро не простаивает ни такта. Рядом — то, ради чего тест
// запускают: температура процессора (датчики ядра, сисколл 143), частота
// (сбрасывается ли она от перегрева), загрузка каждого ядра и график
// температуры за полторы минуты. При заданном пороге тест останавливается
// сам — процессор защищает себя и без нас, но жарить его до упора незачем.
// =============================================================================

#include "ovos.h"
#include "libui/ui.h"
#include "libui/libui_ll.h"
#include "burn.h"

#define MAX_W 2560
#define MAX_H 1600
#define BAND  8                    // строк в полосе: столько поток берёт за раз
#define HIST  180                  // замеров температуры (раз в 0.5 с — полторы минуты)
#define PANEL_H 170

static uint32_t s_img[MAX_W * MAX_H];

// --- потоки ---------------------------------------------------------------
static volatile uint32_t s_go = 0;        // номер кадра: рабочие ждут, пока он станет их следующим
static volatile uint32_t s_next = 0;      // следующая свободная полоса
static volatile uint32_t s_done = 0;      // сколько рабочих закончили кадр
static volatile uint32_t s_bands_by[OV_SMP_MAX + 1];
static burn_frame_t s_frame;
static int s_bands = 0;
static int s_workers = 0;
static uint8_t s_stacks[OV_SMP_MAX][64 * 1024] __attribute__((aligned(16)));

static void do_bands(int who) {
    for (;;) {
        uint32_t b = __atomic_fetch_add(&s_next, 1, __ATOMIC_RELAXED);
        if ((int)b >= s_bands) break;
        int y0 = (int)b * BAND, y1 = y0 + BAND;
        if (y1 > s_frame.h) y1 = s_frame.h;
        burn_rows(&s_frame, y0, y1);
        s_bands_by[who]++;
    }
}

static void worker(void* arg) {
    int who = (int)(intptr_t)arg;
    uint32_t seen = 0;
    for (;;) {
        // ov_wait_u32 ждёт, ПОКА слово равно seen: спим в hlt до нового кадра
        while (__atomic_load_n(&s_go, __ATOMIC_ACQUIRE) == seen) ov_wait_u32(&s_go, seen, 0);
        seen = __atomic_load_n(&s_go, __ATOMIC_ACQUIRE);
        do_bands(who);
        __atomic_add_fetch(&s_done, 1, __ATOMIC_RELEASE);
    }
}

static void render_frame(void) {
    s_bands = (s_frame.h + BAND - 1) / BAND;
    __atomic_store_n(&s_next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_done, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_go, 1, __ATOMIC_RELEASE);
    ov_wake(&s_go);
    do_bands(0);
    uint32_t d;
    while ((d = __atomic_load_n(&s_done, __ATOMIC_ACQUIRE)) != (uint32_t)s_workers)
        ov_wait_u32(&s_done, d, 1);                // уступаем процессор, пока число не изменилось
}

// --- вывод ----------------------------------------------------------------
static void str_add(char* d, int* n, int max, const char* s) { while (*s && *n < max - 1) d[(*n)++] = *s++; d[*n] = 0; }
static void num_add(char* d, int* n, int max, int v) { char t[16]; ui_itoa(v, t); str_add(d, n, max, t); }

static uint32_t temp_color(int t) {
    if (t >= 90) return 0xDA4453;
    if (t >= 80) return 0xF67400;
    if (t >= 65) return 0xFDBC4B;
    return 0x27AE60;
}

typedef struct { const char* name; int steps; int ssaa; } mode_t;
static const mode_t MODES[3] = { { "Лёгкий", 24, 0 }, { "Средний", 48, 0 }, { "Экстрим", 64, 1 } };

int main(void) {
    libui_bind();
    ui_app_id("overmark");
    ui_win_t* w = ui_open("overMark", 1000, 680, 0, WP_FLAG_RESIZABLE);
    if (!w) return 1;
    ui_redraw_all(w);

    // Рабочие — на всех свободных ядрах
    ov_smp_info_t si;
    ov_smp_info(&si);
    int free_cores = si.free > OV_SMP_MAX ? OV_SMP_MAX : si.free;
    for (int i = 0; i < free_cores; i++)
        if (ov_worker_start(worker, (void*)(intptr_t)(i + 1), s_stacks[i] + sizeof(s_stacks[i])) >= 0) s_workers++;
    const int threads = s_workers + 1;

    ov_cpuinfo_t ci;
    int have_ci = ov_cpuinfo(&ci) == 0 || ci.brand[0];

    int running = 0, mode = 1, autostop = 1, stop_at = 95;
    uint64_t t_start = 0, t_last = ov_ms(), t_sample = 0, run_ms = 0;
    float time_s = 0.0f;
    int fps = 0, frames = 0;
    uint64_t fps_t = ov_ms();
    int temp = -1, temp_max = -1, mhz = 0, tjmax = 100;
    int hist[HIST];
    int nhist = 0;
    const char* note = 0;
    ov_cpu_sensors_t cs;
    uint64_t busy_prev[OV_SMP_MAX] = { 0 }, total_prev[OV_SMP_MAX] = { 0 };
    int load[OV_SMP_MAX + 1] = { 0 };

    while (ui_begin(w, running ? 0 : 200)) {
        uint64_t now = ov_ms();
        float dt = (float)(now - t_last) / 1000.0f;
        t_last = now;
        const int W = ui_width(w), H = ui_height(w);
        int vw = W, vh = H - PANEL_H;
        if (vw > MAX_W) vw = MAX_W;
        if (vh > MAX_H) vh = MAX_H;
        if (vh < 16) vh = 16;

        // Датчики — дважды в секунду
        if (now - t_sample >= 500) {
            t_sample = now;
            if (ov_cpu_sensors(&cs) && cs.valid) {
                temp = cs.pkg_c;
                tjmax = cs.tjmax_c;
                if (running && temp > temp_max) temp_max = temp;
            } else temp = -1;
            mhz = cs.cur_mhz;
            if (nhist == HIST) { for (int i = 1; i < HIST; i++) hist[i - 1] = hist[i]; nhist--; }
            hist[nhist++] = temp;
            // Загрузка рабочих ядер этой программы (доля тактов вне hlt)
            ov_smp_info(&si);
            int k = 1;
            for (int i = 0; i < OV_SMP_MAX && k < threads; i++) {
                if (si.cpu[i].state == 0 || si.cpu[i].owner_pid != ov_getpid()) continue;
                uint64_t db = si.cpu[i].busy_tsc - busy_prev[i], dtt = si.cpu[i].total_tsc - total_prev[i];
                busy_prev[i] = si.cpu[i].busy_tsc;
                total_prev[i] = si.cpu[i].total_tsc;
                load[k++] = dtt ? (int)(db * 100 / dtt) : 0;
            }
            load[0] = running ? 100 : 0;              // основной поток считает вместе со всеми
            if (running && autostop && temp >= stop_at) { running = 0; note = "Остановлено: процессор дошёл до порога"; }
        }

        if (running) {
            time_s += dt;
            run_ms = now - t_start;
            s_frame.time = time_s;
            s_frame.steps = MODES[mode].steps;
            s_frame.ssaa = MODES[mode].ssaa;
            s_frame.w = vw & ~3;                       // по четыре луча в ряд
            s_frame.h = vh;
            s_frame.out = s_img;
            render_frame();
            frames++;
        }
        if (now - fps_t >= 1000) { fps = (int)(frames * 1000 / (now - fps_t)); frames = 0; fps_t = now; }

        // --- картинка
        ui_clear(w);
        if (s_frame.w > 0) ui_blit(w, 0, 0, s_frame.w, s_frame.h, s_img, s_frame.w);
        if (!running) {
            const char* m = note ? note : "Нажми «Старт» — все ядра начнут греть процессор";
            ui_text_center(w, 0, vh / 2 - 8, W, m, note ? 0xFDBC4B : UI_TEXT);
        }
        char line[160];
        int n = 0;
        str_add(line, &n, sizeof line, "overMark   ");
        num_add(line, &n, sizeof line, fps);
        str_add(line, &n, sizeof line, " FPS   потоков ");
        num_add(line, &n, sizeof line, threads);
        str_add(line, &n, sizeof line, "   режим: ");
        str_add(line, &n, sizeof line, MODES[mode].name);
        ui_text(w, 11, 9, line, 0x101010);
        ui_text(w, 10, 8, line, 0xFFFFFF);

        // --- панель снизу
        int py = H - PANEL_H;
        ui_fill(w, 0, py, W, PANEL_H, UI_PANEL);
        ui_hline(w, 0, py, W, UI_BORDER);
        int x = 16, y = py + 12;
        if (running) { if (ui_button_accent(w, x, y, 110, 30, "Стоп")) running = 0; }
        else if (ui_button_accent(w, x, y, 110, 30, "Старт")) {
            running = 1; note = 0; t_start = now; temp_max = temp;
        }
        for (int i = 0; i < 3; i++) {
            int bx = x + 124 + i * 100;
            if (ui_button(w, bx, y, 92, 30, MODES[i].name)) mode = i;
            if (i == mode) ui_frame(w, bx - 2, y - 2, 96, 34, UI_ACCENT);
        }
        n = 0;
        str_add(line, &n, sizeof line, "Остановить при ");
        num_add(line, &n, sizeof line, stop_at);
        str_add(line, &n, sizeof line, " °C");
        ui_checkbox(w, x + 440, y + 6, line, &autostop);
        if (ui_button(w, x + 640, y, 30, 30, "-") && stop_at > 60) stop_at -= 5;
        if (ui_button(w, x + 676, y, 30, 30, "+") && stop_at < 100) stop_at += 5;

        // Показатели
        y += 46;
        n = 0;
        str_add(line, &n, sizeof line, "Температура: ");
        if (temp >= 0) { num_add(line, &n, sizeof line, temp); str_add(line, &n, sizeof line, " °C"); }
        else str_add(line, &n, sizeof line, "нет датчика");
        ui_text(w, x, y, line, temp >= 0 ? temp_color(temp) : UI_TEXT_MUTED);
        n = 0;
        str_add(line, &n, sizeof line, "максимум ");
        if (temp_max >= 0) { num_add(line, &n, sizeof line, temp_max); str_add(line, &n, sizeof line, " °C"); }
        else str_add(line, &n, sizeof line, "—");
        str_add(line, &n, sizeof line, ", порог процессора ");
        num_add(line, &n, sizeof line, tjmax);
        str_add(line, &n, sizeof line, " °C");
        ui_label_muted(w, x + 190, y, line);
        y += 22;
        n = 0;
        str_add(line, &n, sizeof line, "Частота: ");
        if (mhz > 0) { num_add(line, &n, sizeof line, mhz); str_add(line, &n, sizeof line, " МГц"); }
        else str_add(line, &n, sizeof line, "нет данных");
        if (have_ci && ci.base_mhz) { str_add(line, &n, sizeof line, " (база "); num_add(line, &n, sizeof line, ci.base_mhz); str_add(line, &n, sizeof line, ")"); }
        str_add(line, &n, sizeof line, "    Время теста: ");
        char tb[16];
        ui_fmt_time((uint32_t)run_ms, tb);
        str_add(line, &n, sizeof line, tb);
        ui_label(w, x, y, line);
        y += 22;
        if (have_ci) ui_label_muted(w, x, y, ci.brand);
        // Загрузка потоков
        y += 22;
        for (int i = 0; i < threads && i < 9; i++) {
            int bx = x + i * 74;
            ui_fill(w, bx, y, 66, 10, UI_BG_DARK);
            ui_fill(w, bx, y, 66 * (load[i] > 100 ? 100 : load[i]) / 100, 10, i ? 0x58C060 : UI_ACCENT);
        }

        // График температуры справа: 40..100 °C
        int gw = W - 760 > 180 ? W - 760 : 180, gx = W - gw - 16, gh = PANEL_H - 58, gy = py + 50;
        if (gx > 720) {
            ui_fill(w, gx, gy, gw, gh, UI_BG_DARK);
            for (int c = 40; c <= 100; c += 20) {
                int ly = gy + gh - (c - 40) * gh / 60;
                ui_hline(w, gx, ly, gw, UI_BORDER);
                char cb[8]; ui_itoa(c, cb);
                ui_text(w, gx - 26, ly - 8, cb, UI_TEXT_MUTED);
            }
            for (int i = 1; i < nhist; i++) {
                if (hist[i] < 0 || hist[i - 1] < 0) continue;
                int x0 = gx + (i - 1) * gw / HIST, x1 = gx + i * gw / HIST;
                int t0 = hist[i - 1] < 40 ? 40 : (hist[i - 1] > 100 ? 100 : hist[i - 1]);
                int t1 = hist[i] < 40 ? 40 : (hist[i] > 100 ? 100 : hist[i]);
                int y0 = gy + gh - (t0 - 40) * gh / 60, y1 = gy + gh - (t1 - 40) * gh / 60;
                // Отрезок: ступенькой (по горизонтали, потом по вертикали)
                ui_fill(w, x0, y0, x1 - x0 + 1, 2, temp_color(t1));
                int ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
                ui_fill(w, x1, ya, 2, yb - ya + 2, temp_color(t1));
            }
            ui_label_muted(w, gx, py + 28, "Температура за 90 с");
        }
        ui_end(w);
    }
    ui_close(w);
    return 0;
}
