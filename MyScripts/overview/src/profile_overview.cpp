/*
 * profile_overview.cpp
 *
 * Без взаимного pkill.
 * Без system() из signal handler.
 * Безопасное завершение LayerWindow.
 */

#include "../common/layer_window.hpp"
#include "../common/draw_helpers.hpp"
#include "../common/proc_helpers.hpp"
#include "../../config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <climits>
#include <signal.h>
#include <cerrno>

#define POV_COLS    3
#define POV_ROWS    3
#define POV_HDR_H   30
#define POV_HINT_H  22

// ─────────────────────────── signal state ────────────────────────

static volatile sig_atomic_t g_stop_requested = 0;

static void signal_handler(int)
{
    /*
     * НИКАКИХ system(), malloc(), Wayland API и т.п.
     * внутри signal handler.
     */
    g_stop_requested = 1;
}

// ─────────────────────────── submap ──────────────────────────────

static void reset_submap()
{
    /*
     * Выполняется только из обычного event-loop/thread контекста.
     * Раньше здесь был блокирующий system() — на время его выполнения
     * (fork шелла + коннект к Hyprland IPC, обычно 10-50+ мс)
     * LayerWindow::run() был полностью заморожен и не мог обработать
     * ни один Wayland fd, ни один таймер, включая g_stop_requested.
     * Если хоткей нажимался именно в это окно, ответ откладывался до
     * разморозки, что ощущалось как "хоткей не сработал с первого
     * раза". spawn_detached() не ждёт завершения — event loop не
     * замирает.
     */
    spawn_detached(
        "hyprctl dispatch "
        "'hl.dsp.submap(\"reset\")' "
        ">/dev/null 2>&1");
}

// ─────────────────────────── single instance ─────────────────────

static int g_lock_fd = -1;

static bool acquire_single_instance(const char *app_name)
{
    char lock_path[128];

    snprintf(
        lock_path,
        sizeof(lock_path),
        "/tmp/%s.lock",
        app_name);

    g_lock_fd =
        open(
            lock_path,
            O_RDWR | O_CREAT | O_CLOEXEC,
            0666);

    if (g_lock_fd < 0)
        return false;

    if (flock(
            g_lock_fd,
            LOCK_EX | LOCK_NB) < 0) {

        if (errno == EWOULDBLOCK ||
            errno == EAGAIN) {

            /*
             * Экземпляр уже существует.
             *
             * toggle-profile-overview.sh спавнит новый процесс только
             * в ветке missing) — когда hyprctl clients -j не находит
             * окна с таким title. Раз мы вообще дошли до сюда (flock
             * провалился), значит окно физически есть, а скрипт его
             * почему-то не увидел — в норме такого не бывает. Ничего
             * не делаем: правильная реакция на "уже запущен" — тихо
             * выйти, уже работающий процесс никак не трогаем. Но хотя
             * бы не молча — иначе ручной запуск из терминала выглядит
             * как "просто ничего не произошло".
             */
            char pid_buf[32] = {0};

            lseek(g_lock_fd, 0, SEEK_SET);

            ssize_t n = read(
                g_lock_fd,
                pid_buf,
                sizeof(pid_buf) - 1);

            fprintf(
                stderr,
                "%s: уже запущен (pid %s) — выхожу, "
                "работающий процесс не трогаю\n",
                app_name,
                (n > 0) ? pid_buf : "?");

            close(g_lock_fd);
            g_lock_fd = -1;

            return false;
        }

        close(g_lock_fd);
        g_lock_fd = -1;

        return false;
    }

    /*
     * Lock успешно захвачен.
     * Записываем PID текущего экземпляра.
     */
    if (ftruncate(g_lock_fd, 0) == 0) {
        char pid_buf[32];

        snprintf(
            pid_buf,
            sizeof(pid_buf),
            "%d\n",
            static_cast<int>(getpid()));

        write(
            g_lock_fd,
            pid_buf,
            strlen(pid_buf));

        lseek(
            g_lock_fd,
            0,
            SEEK_SET);
    }

    return true;
}


static void release_single_instance()
{
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

// ─────────────────────────── state ───────────────────────────────

struct PovWin {
    char cls[22];
    char title[30];
};

struct PovSlot {
    PovWin wins[24];
    int count = 0;
    cairo_surface_t *thumb = nullptr;
};

struct PovState {
    int base_ws    = 1;
    int cur_prof   = 1;
    int altab_prof = 2;
    int prev_prof  = 0;

    PovSlot slots[PROFILE_COUNT + 1];
};

static PovState g;
static int sel = 1;

static LayerWindow *win = nullptr;

// ─────────────────────────── storage ─────────────────────────────

static int read_altab_val(int bws)
{
    char pat[16];
    char line[32];

    snprintf(
        pat,
        sizeof(pat),
        "ws_%d=",
        bws);

    FILE *f =
        fopen(ALTAB_STORAGE, "r");

    if (!f)
        return 2;

    const size_t plen = strlen(pat);

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, pat, plen) == 0) {
            fclose(f);

            int v =
                atoi(line + plen);

            return
                (v >= 1 &&
                 v <= PROFILE_COUNT)
                ? v
                : 2;
        }
    }

    fclose(f);
    return 2;
}

static void save_altab_val(
    int bws,
    int prof)
{
    char tmp[128];
    char pat[32];
    char line[128];

    snprintf(
        tmp,
        sizeof(tmp),
        "%s.tmp",
        ALTAB_STORAGE);

    snprintf(
        pat,
        sizeof(pat),
        "ws_%d=",
        bws);

    FILE *in =
        fopen(ALTAB_STORAGE, "r");

    FILE *out =
        fopen(tmp, "w");

    if (!out) {
        if (in)
            fclose(in);

        return;
    }

    const size_t plen = strlen(pat);

    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (strncmp(line, pat, plen) != 0)
                fputs(line, out);
        }

        fclose(in);
    }

    fprintf(
        out,
        "ws_%d=%d\n",
        bws,
        prof);

    fclose(out);

    rename(tmp, ALTAB_STORAGE);
}

static void save_profile_val(
    int bws,
    int prof)
{
    char tmp[128];
    char pat[32];
    char line[128];

    snprintf(
        tmp,
        sizeof(tmp),
        "%s.tmp",
        PROFILE_STORAGE);

    snprintf(
        pat,
        sizeof(pat),
        "ws_%d=",
        bws);

    FILE *in =
        fopen(PROFILE_STORAGE, "r");

    FILE *out =
        fopen(tmp, "w");

    if (!out) {
        if (in)
            fclose(in);

        return;
    }

    const size_t plen = strlen(pat);

    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (strncmp(line, pat, plen) != 0)
                fputs(line, out);
        }

        fclose(in);
    }

    fprintf(
        out,
        "ws_%d=%d\n",
        bws,
        prof);

    fclose(out);

    rename(tmp, PROFILE_STORAGE);
}

static int read_prev_val(int bws)
{
    char path[64];
    char buf[16] = {0};

    snprintf(
        path,
        sizeof(path),
        "/tmp/hypr-altab-prev_ws_%d",
        bws);

    FILE *f =
        fopen(path, "r");

    if (!f)
        return 0;

    fgets(buf, sizeof(buf), f);
    fclose(f);

    const int v = atoi(buf);

    return
        (v >= 1 &&
         v <= PROFILE_COUNT)
        ? v
        : 0;
}

// ─────────────────────────── load state ──────────────────────────

static void load_state()
{
    const int raw =
        active_workspace_id();

    if (raw > 0) {
        const int bws =
            BASE_WS(raw);

        if (bws >= 1 &&
            bws <= WS_COUNT) {

            g.base_ws = bws;
        }
    }

    for (int p = 1;
         p <= PROFILE_COUNT;
         ++p) {

        if (g.slots[p].thumb) {
            cairo_surface_destroy(
                g.slots[p].thumb);

            g.slots[p].thumb = nullptr;
        }

        g.slots[p].count = 0;
    }

    g.cur_prof =
        read_ws_profile(g.base_ws);

    if (g.cur_prof < 1 ||
        g.cur_prof > PROFILE_COUNT) {

        g.cur_prof = 1;
    }

    g.altab_prof =
        read_altab_val(g.base_ws);

    g.prev_prof =
        read_prev_val(g.base_ws);

    for (int p = 1;
         p <= PROFILE_COUNT;
         ++p) {

        char path[128];

        snprintf(
            path,
            sizeof(path),
            "%s/ws_%d_p%d.png",
            WS_CACHE_DIR,
            g.base_ws,
            p);

        cairo_surface_t *s =
            cairo_image_surface_create_from_png(path);

        if (cairo_surface_status(s) ==
            CAIRO_STATUS_SUCCESS) {

            g.slots[p].thumb = s;
        }
        else {
            cairo_surface_destroy(s);
        }
    }

    FILE *fp =
        popen(
            "hyprctl clients",
            "r");

    if (!fp)
        return;

    struct TempClient {
        int ws = 0;

        char cls[64] = {0};
        char title[128] = {0};
        char ws_name[64] = {0};
    } cur;

    auto add_client = [&]() {
        if (cur.ws <= 0)
            return;

        if (strncmp(
                cur.ws_name,
                "special",
                7) == 0) {

            return;
        }

        if (strcmp(cur.cls, "ws-overview") == 0 ||
            strcmp(cur.cls, "dashboard") == 0 ||
            strcmp(cur.cls, "profile-overview") == 0) {

            return;
        }

        const int bws =
            BASE_WS(cur.ws);

        const int prof =
            PROFILE_N(cur.ws);

        if (bws != g.base_ws)
            return;

        if (prof < 1 ||
            prof > PROFILE_COUNT) {

            return;
        }

        PovSlot *sl =
            &g.slots[prof];

        if (sl->count >= 24)
            return;

        PovWin *e =
            &sl->wins[sl->count++];

        memset(e, 0, sizeof(*e));

        strncpy(
            e->cls,
            cur.cls,
            sizeof(e->cls) - 1);

        strncpy(
            e->title,
            cur.title,
            sizeof(e->title) - 1);

        if (e->cls[0] &&
            strlen(e->cls) < strlen(e->title)) {

            strncpy(
                e->title,
                e->cls,
                sizeof(e->title) - 1);
        }
    };

    char line[512];

    while (fgets(line, sizeof(line), fp)) {

        if (strncmp(
                line,
                "Window ",
                7) == 0) {

            add_client();

            cur = TempClient();

            char *arrow =
                strstr(line, "-> ");

            if (arrow) {
                strncpy(
                    cur.title,
                    arrow + 3,
                    sizeof(cur.title) - 1);

                char *colon =
                    strrchr(cur.title, ':');

                if (colon)
                    *colon = '\0';

                cur.title[
                    strcspn(
                        cur.title,
                        "\n\r")] = '\0';
            }

            continue;
        }

        char *key = line;

        while (*key == ' ' ||
               *key == '\t') {

            ++key;
        }

        if (strncmp(
                key,
                "class: ",
                7) == 0) {

            strncpy(
                cur.cls,
                key + 7,
                sizeof(cur.cls) - 1);

            cur.cls[
                strcspn(
                    cur.cls,
                    "\n\r")] = '\0';
        }
        else if (strncmp(
                     key,
                     "workspace: ",
                     11) == 0) {

            sscanf(
                key + 11,
                "%d (%63[^)])",
                &cur.ws,
                cur.ws_name);
        }
        else if (strncmp(
                     key,
                     "title: ",
                     7) == 0) {

            if (cur.title[0] == '\0') {
                strncpy(
                    cur.title,
                    key + 7,
                    sizeof(cur.title) - 1);

                cur.title[
                    strcspn(
                        cur.title,
                        "\n\r")] = '\0';
            }
        }
    }

    add_client();

    pclose(fp);
}

// ─────────────────────────── actions ─────────────────────────────

static void close_overview()
{
    reset_submap();

    if (win)
        win->stop();
}

/*
 * Прячет оверлей по-настоящему: теперь это обычное toplevel-окно
 * (title="profile-overview", см. layer_window.cpp/xdg_toplevel), так
 * что hl.dsp.window.move на special-workspace действительно находит
 * его и убирает с экрана — ровно так же, как toggle-dashboard.sh уже
 * делает для dashboard. (Раньше это было layer-shell окно без
 * workspace вообще, и этот же вызов был no-op — отсюда и был баг
 * "никак не закрывается".)
 *
 * Вызывается только из on_key() (Escape/q — см. ниже). Показ теперь
 * целиком на стороне toggle-profile-overview.sh, как и submap —
 * симметрично dashboard, никакого своего IPC/fade не нужно.
 */
static void park_hidden()
{
    spawn_detached(
        "hyprctl dispatch "
        "'hl.dsp.window.move({ workspace = \"special:profiles\", "
        "window = \"title:profile-overview\", follow = false })' "
        ">/dev/null 2>&1");

    reset_submap();
}

/*
 * Открывает панель hidden-apps-view (список скрытых по Win+Alt+C окон)
 * для ВЫБРАННОЙ (sel) ячейки — не обязательно активного сейчас
 * профиля. Сначала прячем себя тем же путём, что и по Escape/q
 * (park_hidden), затем спавним toggle-скрипт с явными base/prof — см.
 * toggle-hidden-apps.sh.
 *
 * Небольшая гонка (park_hidden асинхронно дёргает hyprctl, а
 * toggle-hidden-apps.sh запускается сразу следом) — тот же паттерн,
 * что уже есть у кросс-биндингов CTRL+ALT+I/E внутри сабмапов (см.
 * hyprland.lua), в рамках текущей архитектуры это приемлемо.
 */
static void open_hidden_for_selected()
{
    if (sel < 1 ||
        sel > PROFILE_COUNT) {

        return;
    }

    char cmd[256];

    snprintf(
        cmd,
        sizeof(cmd),
        "~/.config/hypr/MyScripts/toggle-hidden-apps.sh %d %d "
        ">/dev/null 2>&1",
        g.base_ws,
        sel);

    park_hidden();
    spawn_detached(cmd);
}

static void reload_and_redraw()
{
    load_state();

    sel = g.cur_prof;

    if (sel < 1 ||
        sel > PROFILE_COUNT) {

        sel = 1;
    }

    win->request_redraw();
}

// Фоновое обновление: курсор остаётся на месте, но на другом рабочем столе
// встаёт на его текущий профиль. Вызывается и пока окно скрыто, поэтому смену
// стола часто замечает именно секундный таймер, а не флаг при открытии.
static void refresh_keeping_selection()
{
    const int old_base =
        g.base_ws;

    const int sv =
        sel;

    load_state();

    sel =
        (g.base_ws != old_base)
            ? g.cur_prof
            : sv;

    if (sel < 1)
        sel = 1;

    if (sel > PROFILE_COUNT)
        sel = PROFILE_COUNT;

    win->request_redraw();
}

static void goto_prof_action(int prof)
{
    if (prof < 1 ||
        prof > PROFILE_COUNT) {

        return;
    }

    const int target =
        HYPR_ID(g.base_ws, prof);

    write_ws_profile(
        g.base_ws,
        prof);

    save_profile_val(
        g.base_ws,
        prof);

    /*
     * Тот же свап, что и в profile-switch.sh (goto): если уходим С
     * TAB — TAB едет за нами на новую цель (иначе Alt+Tab вёл бы
     * обратно туда, где мы уже не стоим). Если приходим НА TAB — не
     * свапнуть значило бы, что ACT (=prof) и TAB совпадут; меняем
     * местами, TAB теперь там, откуда мы ушли.
     */
    if (g.cur_prof == g.altab_prof &&
        prof != g.altab_prof) {

        save_altab_val(
            g.base_ws,
            prof);

        g.altab_prof = prof;

    } else if (prof == g.altab_prof &&
               g.cur_prof != g.altab_prof) {

        save_altab_val(
            g.base_ws,
            g.cur_prof);

        g.altab_prof = g.cur_prof;
    }

    reset_submap();

    char cmd[512];

    snprintf(
        cmd,
        sizeof(cmd),
        "hyprctl dispatch "
        "'hl.dsp.window.move({ workspace = \"special:profiles\", "
        "window = \"title:profile-overview\", follow = false })' "
        ">/dev/null 2>&1; "
        "sleep 0.05; "
        "hyprctl dispatch "
        "'hl.dsp.focus({ workspace = %d })' "
        ">/dev/null 2>&1; "
        "touch " OV_RELOAD_FLAG
        " 2>/dev/null",
        target);

    // Не блокируем event loop (см. комментарий в reset_submap()).
    spawn_detached(cmd);
}

// ─────────────────────────── geometry ────────────────────────────

static void cell_geom(
    int width,
    int height,
    int *out_ox,
    int *out_oy,
    int *out_cw,
    int *out_ch)
{
    int avail_h =
        height -
        POV_HDR_H -
        POV_HINT_H;

    int cw =
        (width -
         OV_GAP * (POV_COLS + 1))
        / POV_COLS;

    int ch =
        cw * MON_H / MON_W;

    int mh =
        (avail_h -
         OV_GAP * (POV_ROWS + 1) -
         OV_LABEL_H * POV_ROWS)
        / POV_ROWS;

    if (ch > mh) {
        ch = mh;
        cw = ch * MON_W / MON_H;
    }

    *out_ox =
        (width -
         (cw * POV_COLS +
          OV_GAP * (POV_COLS + 1))) / 2;

    *out_oy =
        POV_HDR_H +
        (avail_h -
         ((ch + OV_LABEL_H) * POV_ROWS +
          OV_GAP * (POV_ROWS + 1))) / 2;

    *out_cw = cw;
    *out_ch = ch;
}

// ─────────────────────────── draw ────────────────────────────────

static void draw_func(
    cairo_t *cr,
    int width,
    int height)
{
    set_color(cr, C_BG, 1.0);
    cairo_paint(cr);

    cairo_select_font_face(
        cr,
        "monospace",
        CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL);

    cairo_rectangle(
        cr,
        0,
        0,
        width,
        POV_HDR_H);

    set_color(cr, C_DIM, 1.0);
    cairo_fill(cr);

    char hdr[256];

    if (g.cur_prof == g.altab_prof &&
        g.prev_prof > 0) {

        snprintf(
            hdr,
            sizeof(hdr),
            " ПРОФИЛИ  ws_%d   "
            "[ACT/TAB]:P%d   [PRV]:P%d",
            g.base_ws,
            g.cur_prof,
            g.prev_prof);
    }
    else {
        snprintf(
            hdr,
            sizeof(hdr),
            " ПРОФИЛИ  ws_%d   "
            "[ACT]:P%d  →tab→  [TAB]:P%d",
            g.base_ws,
            g.cur_prof,
            g.altab_prof);
    }

    cairo_set_font_size(cr, 13.0);

    set_color(cr, C_TEXT, 0.9);

    cairo_move_to(
        cr,
        8,
        POV_HDR_H - 9);

    cairo_show_text(cr, hdr);

    int ox, oy, cw, ch;

    cell_geom(
        width,
        height,
        &ox,
        &oy,
        &cw,
        &ch);

    for (int p = 1;
         p <= PROFILE_COUNT;
         ++p) {

        const int col =
            (p - 1) % POV_COLS;

        const int row =
            (p - 1) / POV_COLS;

        const int cx =
            ox +
            OV_GAP +
            col * (cw + OV_GAP);

        const int cy =
            oy +
            OV_GAP +
            row * (ch + OV_LABEL_H + OV_GAP);

        PovSlot *sl =
            &g.slots[p];

        const bool is_sel =
            p == sel;

        const bool is_cur =
            p == g.cur_prof;

        const bool is_tab =
            p == g.altab_prof;

        const bool is_prv =
            g.prev_prof > 0 &&
            p == g.prev_prof &&
            g.cur_prof == g.altab_prof;

        const unsigned int pcol =
            PROFILE_COLORS[p];

        rounded_rect(
            cr,
            cx,
            cy,
            cw,
            ch,
            OV_CORNER_R);

        set_color(
            cr,
            is_cur
                ? 0x141a21
                : C_CELL_BG,
            1.0);

        cairo_fill(cr);

        cairo_save(cr);

        rounded_rect(
            cr,
            cx,
            cy,
            cw,
            ch,
            OV_CORNER_R);

        cairo_clip(cr);

        if (sl->thumb) {
            const int tw =
                cairo_image_surface_get_width(
                    sl->thumb);

            const int th =
                cairo_image_surface_get_height(
                    sl->thumb);

            cairo_save(cr);

            cairo_translate(
                cr,
                cx,
                cy);

            cairo_scale(
                cr,
                static_cast<double>(cw) / tw,
                static_cast<double>(ch) / th);

            cairo_set_source_surface(
                cr,
                sl->thumb,
                0,
                0);

            cairo_paint(cr);

            cairo_restore(cr);
        }
        else if (sl->count > 0) {

            int max_show =
                (ch - 16) / 15;

            if (max_show < 1)
                max_show = 1;

            const int show =
                sl->count < max_show
                    ? sl->count
                    : max_show;

            const int top =
                cy + (ch - show * 15) / 2;

            cairo_set_font_size(
                cr,
                10.5);

            for (int wi = 0;
                 wi < show;
                 ++wi) {

                set_color(
                    cr,
                    (is_sel || is_cur)
                        ? C_TEXT
                        : C_TEXT_DIM,
                    (is_sel || is_cur)
                        ? 0.85
                        : 0.55);

                cairo_text_extents_t te;

                cairo_text_extents(
                    cr,
                    sl->wins[wi].title,
                    &te);

                double tx =
                    cx +
                    (cw - te.width) / 2.0;

                if (tx < cx + 2)
                    tx = cx + 2;

                cairo_move_to(
                    cr,
                    tx,
                    top + wi * 15 + 12);

                cairo_show_text(
                    cr,
                    sl->wins[wi].title);
            }

            if (sl->count > max_show) {
                char more[16];

                snprintf(
                    more,
                    sizeof(more),
                    "+%d",
                    sl->count - max_show);

                cairo_set_font_size(
                    cr,
                    9.0);

                set_color(
                    cr,
                    C_TEXT_DIM,
                    0.5);

                cairo_move_to(
                    cr,
                    cx + 4,
                    cy + ch - 5);

                cairo_show_text(
                    cr,
                    more);
            }
        }
        else {
            const char *empty =
                "пусто";

            cairo_set_font_size(
                cr,
                10.5);

            cairo_text_extents_t te;

            cairo_text_extents(
                cr,
                empty,
                &te);

            set_color(
                cr,
                C_TEXT_DIM,
                is_sel ? 0.45 : 0.25);

            cairo_move_to(
                cr,
                cx + (cw - te.width) / 2.0,
                cy + ch / 2.0 + 5);

            cairo_show_text(
                cr,
                empty);
        }

        if (!is_sel && !is_cur) {
            set_color(
                cr,
                0x000000,
                0.45);

            cairo_paint(cr);
        }

        cairo_restore(cr);

        rounded_rect(
            cr,
            cx - 1.5,
            cy - 1.5,
            cw + 3,
            ch + 3,
            OV_CORNER_R + 1);

        if (is_sel) {
            set_color(
                cr,
                C_BORDER_SEL,
                1.0);

            cairo_set_line_width(
                cr,
                2.5);
        }
        else if (is_cur) {
            set_color(
                cr,
                C_BORDER_CUR,
                1.0);

            cairo_set_line_width(
                cr,
                1.5);
        }
        else {
            set_color(
                cr,
                C_BORDER_DIM,
                0.45);

            cairo_set_line_width(
                cr,
                1.0);
        }

        cairo_stroke(cr);

        if (is_cur)
            status_frame(
                cr,
                cx,
                cy,
                cw,
                ch,
                OV_CORNER_R,
                C_SEL,
                4.0,
                2.0,
                0.95);

        if (is_tab)
            status_frame(
                cr,
                cx,
                cy,
                cw,
                ch,
                OV_CORNER_R,
                0xe5c07b,
                is_cur ? 7.5 : 4.0,
                2.0,
                0.85);

        int lx = cx;
        int ly = cy + ch + 2;

        cairo_set_font_size(
            cr,
            is_sel ? 12.0 : 10.5);

        set_color(
            cr,
            pcol,
            (is_sel || is_cur)
                ? 1.0
                : 0.6);

        char plbl[8];

        snprintf(
            plbl,
            sizeof(plbl),
            "P%d",
            p);

        cairo_move_to(
            cr,
            lx + 4,
            ly + OV_LABEL_H - 6);

        cairo_show_text(
            cr,
            plbl);

        cairo_set_font_size(
            cr,
            9.0);

        int mx =
            lx + 26;

        if (is_cur) {
            set_color(
                cr,
                C_SEL,
                0.9);

            cairo_move_to(
                cr,
                mx,
                ly + OV_LABEL_H - 6);

            cairo_show_text(
                cr,
                "ACT");

            mx += 28;
        }

        if (is_tab) {
            set_color(
                cr,
                0xe5c07b,
                0.85);

            cairo_move_to(
                cr,
                mx,
                ly + OV_LABEL_H - 6);

            cairo_show_text(
                cr,
                "TAB");

            mx += 28;
        }

        if (is_prv) {
            set_color(
                cr,
                0x98c379,
                0.85);

            cairo_move_to(
                cr,
                mx,
                ly + OV_LABEL_H - 6);

            cairo_show_text(
                cr,
                "PRV");
        }

        if (sl->count > 0) {
            char cnt[16];

            snprintf(
                cnt,
                sizeof(cnt),
                "%d",
                sl->count);

            cairo_text_extents_t te;

            cairo_text_extents(
                cr,
                cnt,
                &te);

            set_color(
                cr,
                C_TEXT_DIM,
                0.55);

            cairo_move_to(
                cr,
                lx + cw -
                    te.width -
                    te.x_bearing -
                    4,
                ly + OV_LABEL_H - 6);

            cairo_show_text(
                cr,
                cnt);
        }
    }

    cairo_set_font_size(
        cr,
        10.0);

    set_color(
        cr,
        C_TEXT_DIM,
        0.55);

    cairo_move_to(
        cr,
        OV_GAP,
        height - 7);

    cairo_show_text(
        cr,
        "hjkl/1-9 : навигация    "
        "Enter/a : перейти    "
        "t : set TAB    "
        "s : скрытые окна    "
        "r : обновить    "
        "q/Esc : закрыть");
}

// ─────────────────────────── keyboard ────────────────────────────

static void on_key(xkb_keysym_t sym)
{
    if (g_stop_requested) {
        close_overview();
        return;
    }

    switch (sym) {

        case XKB_KEY_h:
        case XKB_KEY_Left:
            if ((sel - 1) % 3 > 0) {
                --sel;
                win->request_redraw();
            }
            break;

        case XKB_KEY_l:
        case XKB_KEY_Right:
            if ((sel - 1) % 3 < 2) {
                ++sel;
                win->request_redraw();
            }
            break;

        case XKB_KEY_k:
        case XKB_KEY_Up:
            if (sel > 3) {
                sel -= 3;
                win->request_redraw();
            }
            break;

        case XKB_KEY_j:
        case XKB_KEY_Down:
            if (sel <= 6) {
                sel += 3;
                win->request_redraw();
            }
            break;

        case XKB_KEY_1:
        case XKB_KEY_2:
        case XKB_KEY_3:
        case XKB_KEY_4:
        case XKB_KEY_5:
        case XKB_KEY_6:
        case XKB_KEY_7:
        case XKB_KEY_8:
        case XKB_KEY_9:
            sel =
                sym - XKB_KEY_0;

            win->request_redraw();
            break;

        case XKB_KEY_a:
        case XKB_KEY_A:
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            goto_prof_action(sel);
            break;

        case XKB_KEY_t:
        case XKB_KEY_T:
            if (sel >= 1 &&
                sel != g.cur_prof) {

                save_altab_val(
                    g.base_ws,
                    sel);

                g.altab_prof = sel;

                win->request_redraw();
            }
            break;

        case XKB_KEY_r:
        case XKB_KEY_R: {
            const int sv = sel;

            load_state();

            sel = sv;

            if (sel < 1)
                sel = 1;

            if (sel > PROFILE_COUNT)
                sel = PROFILE_COUNT;

            win->request_redraw();
            break;
        }

        case XKB_KEY_s:
        case XKB_KEY_S:
            open_hidden_for_selected();
            break;

        case XKB_KEY_q:
        case XKB_KEY_Q:
        case XKB_KEY_Escape:
            park_hidden();
            break;

        default:
            break;
    }
}

// ─────────────────────────── mouse ───────────────────────────────

static void on_click(
    double x,
    double y,
    int n_press,
    int width,
    int height)
{
    int ox, oy, cw, ch;

    cell_geom(
        width,
        height,
        &ox,
        &oy,
        &cw,
        &ch);

    for (int p = 1;
         p <= PROFILE_COUNT;
         ++p) {

        const int col =
            (p - 1) % POV_COLS;

        const int row =
            (p - 1) / POV_COLS;

        const int cx =
            ox +
            OV_GAP +
            col * (cw + OV_GAP);

        const int cy =
            oy +
            OV_GAP +
            row * (ch + OV_LABEL_H + OV_GAP);

        if (x >= cx &&
            x <= cx + cw &&
            y >= cy &&
            y <= cy + ch + OV_LABEL_H) {

            sel = p;

            if (n_press == 2)
                goto_prof_action(p);
            else
                win->request_redraw();

            return;
        }
    }
}

// ─────────────────────────── main ────────────────────────────────

int main(
    int argc,
    char *argv[])
{
    signal(
        SIGTERM,
        signal_handler);

    signal(
        SIGINT,
        signal_handler);

    /*
     * spawn_detached() не дожидается дочерних процессов — без этого
     * они оставались бы зомби до следующего waitpid(). SIG_IGN для
     * SIGCHLD заставляет ядро пожинать их автоматически.
     */
    signal(
        SIGCHLD,
        SIG_IGN);

    /*
     * Никакого pkill.
     *
     * ws-overview и profile-overview теперь
     * не уничтожают друг друга.
     */
    if (!acquire_single_instance(
            "profile-overview")) {

        return 0;
    }

    int raw =
        (argc > 1)
            ? atoi(argv[1])
            : 1;

    g.base_ws =
        BASE_WS(raw);

    if (g.base_ws < 1 ||
        g.base_ws > WS_COUNT) {

        g.base_ws = 1;
    }

    sel = 1;

    LayerWindow window(
        "profile-overview");

    win = &window;

    int last_width  = 0;
    int last_height = 0;

    window.set_draw_fn(
        [&](cairo_t *cr, int w, int h) {

            last_width  = w;
            last_height = h;

            draw_func(
                cr,
                w,
                h);
        });

    window.set_key_fn(on_key);

    window.set_click_fn(
        [&](double x,
            double y,
            int n_press) {

            on_click(
                x,
                y,
                n_press,
                last_width,
                last_height);
        });

    if (!window.init()) {
        fprintf(
            stderr,
            "profile-overview: "
            "не удалось инициализировать "
            "окно\n");

        release_single_instance();

        return 1;
    }

    window.add_timer(
        10,
        [] {
            static bool initial = true;

            if (!initial)
                return;

            if (g_stop_requested) {
                win->stop();
                return;
            }

            reload_and_redraw();

            initial = false;
        });

    window.add_timer(
        100,
        [] {
            if (g_stop_requested) {
                close_overview();
                return;
            }

            /*
             * Не завязываемся на видимость: подхватываем свежий
             * скриншот из capture_ws_async() (grim готовит его в фоне
             * и может закончить чуть позже самого показа) независимо
             * от того, запаркованы мы сейчас на special-workspace или
             * нет — дешевле, чем гонять отдельную логику "не обновлять,
             * пока не видно".
             */
            if (access(
                    POV_RELOAD_FLAG,
                    F_OK) == 0) {

                remove(POV_RELOAD_FLAG);

                refresh_keeping_selection();
            }
        });

    window.add_timer(
        1000,
        [] {
            if (g_stop_requested) {
                close_overview();
                return;
            }

            refresh_keeping_selection();
        });

    const int result =
        window.run();

    /*
     * Последний reset выполняется уже
     * в нормальном контексте процесса.
     */
    reset_submap();

    release_single_instance();

    return result;
}

