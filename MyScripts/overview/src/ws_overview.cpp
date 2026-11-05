/*
 * ws_overview.cpp
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
#include <signal.h>
#include <cerrno>
#include <algorithm>
#include <cmath>

static volatile sig_atomic_t g_stop_requested = 0;

static int g_lock_fd = -1;

static LayerWindow *win = nullptr;

// ─────────────────────────── signal ──────────────────────────────

static void signal_handler(int)
{
    /*
     * Signal handler должен быть async-signal-safe.
     *
     * Никакого system(), Wayland API или malloc.
     */
    g_stop_requested = 1;
}

// ─────────────────────────── submap ──────────────────────────────

static void reset_submap()
{
    /*
     * Раньше здесь был блокирующий system() — см. подробный комментарий
     * у reset_submap() в profile_overview.cpp: на время его выполнения
     * event loop полностью замирал, и ответ на toggle-хоткей, нажатый
     * в этот момент, откладывался до разморозки — ощущалось как
     * "хоткей срабатывает через раз/нужно два нажатия".
     */
    spawn_detached(
        "hyprctl dispatch "
        "'hl.dsp.submap(\"reset\")' "
        ">/dev/null 2>&1");
}

// ─────────────────────────── lock ───────────────────────────────

static bool acquire_single_instance(
    const char *app_name)
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

        /*
         * Экземпляр уже существует — см. подробности в main() у
         * вызова acquire_single_instance(). Хотя бы не молча: иначе
         * ручной запуск из терминала выглядит как "просто ничего не
         * произошло".
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

        lseek(g_lock_fd, 0, SEEK_SET);
    }

    return true;
}

static void release_single_instance()
{
    if (g_lock_fd >= 0) {
        flock(
            g_lock_fd,
            LOCK_UN);

        close(g_lock_fd);

        g_lock_fd = -1;
    }
}

// ─────────────────────────── colors ─────────────────────────────

static unsigned int app_color(
    const char *cls)
{
    unsigned int h = 5381;

    for (int i = 0;
         cls[i];
         ++i) {

        h =
            ((h << 5) + h) +
            static_cast<unsigned char>(cls[i]);
    }

    const unsigned int r =
        120 + (h & 0x7f);

    const unsigned int g =
        120 + ((h >> 8) & 0x7f);

    const unsigned int b =
        120 + ((h >> 16) & 0x7f);

    return
        (r << 16) |
        (g << 8) |
        b;
}

// ─────────────────────────── state ───────────────────────────────

struct WinInfo {
    char cls[32];
    char title[48];

    int x;
    int y;
    int w;
    int h;

    int hypr_ws;
};

struct OvState {
    WinInfo wins[96];

    int count = 0;

    int cur_base_ws = 1;
    int cur_profile = 1;

    int ws_profiles[WS_COUNT + 1] = {};
    int altab[WS_COUNT + 1] = {};

    cairo_surface_t *
        thumbs[WS_COUNT + 1] = {};
};

static OvState st;

static int sel_ws = 0;

// ─────────────────────────── load ────────────────────────────────

static void load_state()
{
    const int raw =
        active_workspace_id();

    if (raw > 0) {
        st.cur_base_ws =
            BASE_WS(raw);

        st.cur_profile =
            PROFILE_N(raw);
    }

    for (int i = 1;
         i <= WS_COUNT;
         ++i) {

        st.ws_profiles[i] =
            read_ws_profile(i);

        char pat[32];
        char line2[64];

        snprintf(
            pat,
            sizeof(pat),
            "ws_%d=",
            i);

        FILE *af =
            fopen(
                ALTAB_STORAGE,
                "r");

        st.altab[i] = 2;

        if (af) {
            const size_t plen =
                strlen(pat);

            while (fgets(
                line2,
                sizeof(line2),
                af)) {

                if (strncmp(
                        line2,
                        pat,
                        plen) == 0) {

                    const int v =
                        atoi(line2 + plen);

                    st.altab[i] =
                        (v >= 1 &&
                         v <= PROFILE_COUNT)
                            ? v
                            : 2;

                    break;
                }
            }

            fclose(af);
        }
    }

    if (st.cur_base_ws >= 1 &&
        st.cur_base_ws <= WS_COUNT) {

        st.ws_profiles[
            st.cur_base_ws] =
            st.cur_profile;
    }

    for (int i = 1;
         i <= WS_COUNT;
         ++i) {

        if (st.thumbs[i]) {
            cairo_surface_destroy(
                st.thumbs[i]);

            st.thumbs[i] = nullptr;
        }

        char tpath[128];

        snprintf(
            tpath,
            sizeof(tpath),
            "%s/ws_%d_latest.png",
            WS_CACHE_DIR,
            i);

        cairo_surface_t *s =
            cairo_image_surface_create_from_png(
                tpath);

        if (cairo_surface_status(s) ==
            CAIRO_STATUS_SUCCESS) {

            st.thumbs[i] = s;
        }
        else {
            cairo_surface_destroy(s);
        }
    }

    if (st.cur_base_ws >= 1 &&
            st.cur_base_ws <= WS_COUNT) {

        if (st.thumbs[st.cur_base_ws]) {
            cairo_surface_destroy(
                    st.thumbs[st.cur_base_ws]);

            st.thumbs[st.cur_base_ws] = nullptr;
        }
    }


    st.count = 0;

    FILE *fp =
        popen(
            "hyprctl clients",
            "r");

    if (!fp)
        return;

    struct TempClient {
        int ws = 0;

        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        char cls[64] = {0};
        char title[128] = {0};

        bool mapped = false;

        char ws_name[64] = {0};
    } cur;

    auto add_client = [&]() {

        if (!cur.mapped)
            return;

        if (cur.ws <= 0)
            return;

        if (st.count >= 96)
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

        if (bws < 1 ||
            bws > WS_COUNT) {

            return;
        }

        WinInfo *w =
            &st.wins[st.count++];

        memset(
            w,
            0,
            sizeof(*w));

        w->x = cur.x;
        w->y = cur.y;
        w->w = cur.w;
        w->h = cur.h;
        w->hypr_ws = cur.ws;

        strncpy(
            w->cls,
            cur.cls,
            sizeof(w->cls) - 1);

        strncpy(
            w->title,
            cur.title,
            sizeof(w->title) - 1);
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
                     "at: ",
                     4) == 0) {

            sscanf(
                key + 4,
                "%d,%d",
                &cur.x,
                &cur.y);
        }
        else if (strncmp(
                     key,
                     "size: ",
                     6) == 0) {

            sscanf(
                key + 6,
                "%d,%d",
                &cur.w,
                &cur.h);
        }
        else if (strncmp(
                     key,
                     "mapped: ",
                     8) == 0) {

            int m = 0;

            sscanf(
                key + 8,
                "%d",
                &m);

            cur.mapped =
                (m == 1);
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

// ─────────────────────────── close / navigation ──────────────────

static void close_overview()
{
    reset_submap();

    if (win)
        win->stop();
}

/*
 * Прячет оверлей по-настоящему: теперь это обычное toplevel-окно
 * (title="ws-overview", см. layer_window.cpp/xdg_toplevel), так что
 * hl.dsp.window.move на special-workspace действительно находит его
 * и убирает с экрана — ровно так же, как toggle-dashboard.sh уже
 * делает для dashboard. (Раньше это было layer-shell окно без
 * workspace вообще, и этот же вызов был no-op — отсюда и был баг
 * "никак не закрывается".)
 *
 * Вызывается только из on_key() (Escape/q). Показ и submap теперь
 * целиком на стороне toggle-overview.sh — симметрично dashboard,
 * никакого своего IPC/fade не нужно.
 */
static void park_hidden()
{
    spawn_detached(
        "hyprctl dispatch "
        "'hl.dsp.window.move({ workspace = \"special:overview\", "
        "window = \"title:ws-overview\", follow = false })' "
        ">/dev/null 2>&1");

    reset_submap();
}


static void reload_and_redraw()
{
    load_state();

    sel_ws =
        st.cur_base_ws - 1;

    if (sel_ws < 0)
        sel_ws = 0;

    if (sel_ws >= WS_COUNT)
        sel_ws = WS_COUNT - 1;

    win->request_redraw();
}


static void goto_selected()
{
    const int bws =
        sel_ws + 1;

    int prof =
        st.ws_profiles[bws];

    if (prof < 1 ||
        prof > PROFILE_COUNT) {

        prof = 1;
    }

    const int hypr_id =
        HYPR_ID(bws, prof);

    reset_submap();

    char cmd[512];

    snprintf(
        cmd,
        sizeof(cmd),
        "hyprctl dispatch "
        "'hl.dsp.window.move({ workspace = \"special:overview\", "
        "window = \"title:ws-overview\", follow = false })' "
        ">/dev/null 2>&1; "
        "sleep 0.05; "
        "hyprctl dispatch "
        "'hl.dsp.focus({ workspace = %d })' "
        ">/dev/null 2>&1; "
        "touch " OV_RELOAD_FLAG
        " 2>/dev/null",
        hypr_id);

    // Не блокируем event loop (см. комментарий в reset_submap()).
    spawn_detached(cmd);
}


// ─────────────────────────── draw ────────────────────────────────

static void draw_func(
    cairo_t *cr,
    int width,
    int height)
{
    set_color(
        cr,
        C_BG,
        1.0);

    cairo_paint(cr);

    cairo_select_font_face(
        cr,
        "monospace",
        CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL);

    int cw =
        (width -
         OV_GAP * (WS_COLS + 1))
        / WS_COLS;

    int ch =
        cw * MON_H / MON_W;

    int mh =
        (height -
         OV_GAP * (WS_ROWS + 1) -
         OV_LABEL_H * WS_ROWS)
        / WS_ROWS;

    if (ch > mh) {
        ch = mh;
        cw = ch * MON_W / MON_H;
    }

    const int ox =
        (width -
         (cw * WS_COLS +
          OV_GAP * (WS_COLS + 1))) / 2;

    const int oy =
        (height -
         ((ch + OV_LABEL_H) * WS_ROWS +
          OV_GAP * (WS_ROWS + 1))) / 2;

    const double sx =
        static_cast<double>(cw) / MON_W;

    const double sy =
        static_cast<double>(ch) / MON_H;

    for (int ws = 0;
         ws < WS_COUNT;
         ++ws) {

        const int bws =
            ws + 1;

        const int col =
            ws % WS_COLS;

        const int row =
            ws / WS_COLS;

        const int cx =
            ox +
            OV_GAP +
            col * (cw + OV_GAP);

        const int cy =
            oy +
            OV_GAP +
            row * (ch + OV_LABEL_H + OV_GAP);

        const bool is_sel =
            ws == sel_ws;

        const bool is_cur =
            bws == st.cur_base_ws;

        const int prof =
            st.ws_profiles[bws];

        const unsigned int pcol =
            PROFILE_COLORS[
                (prof >= 1 &&
                 prof <= PROFILE_COUNT)
                    ? prof
                    : 1];

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

        if (st.thumbs[bws]) {

            const int tw =
                cairo_image_surface_get_width(
                    st.thumbs[bws]);

            const int th =
                cairo_image_surface_get_height(
                    st.thumbs[bws]);

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
                st.thumbs[bws],
                0,
                0);

            cairo_paint(cr);

            cairo_restore(cr);
        }
        else {

            for (int i = 0;
                 i < st.count;
                 ++i) {

                WinInfo *w =
                    &st.wins[i];

                if (BASE_WS(w->hypr_ws) != bws)
                    continue;

                if (PROFILE_N(w->hypr_ws) != prof)
                    continue;

                const double wx =
                    cx + w->x * sx;

                const double wy =
                    cy + w->y * sy;

                const double ww =
                    w->w * sx;

                const double wh =
                    w->h * sy;

                if (ww < 2 ||
                    wh < 2) {

                    continue;
                }

                const unsigned int wc =
                    app_color(w->cls);

                rounded_rect(
                    cr,
                    wx,
                    wy,
                    ww,
                    wh,
                    2.0);

                set_color(
                    cr,
                    C_WIN_BG,
                    1.0);

                cairo_fill(cr);

                const double bar =
                    std::min(
                        4.0,
                        wh * 0.15);

                cairo_rectangle(
                    cr,
                    wx,
                    wy,
                    ww,
                    bar);

                set_color(
                    cr,
                    wc,
                    0.85);

                cairo_fill(cr);

                if (ww > 30 &&
                    wh > 14) {

                    const char *lbl =
                        w->cls[0]
                            ? w->cls
                            : w->title;

                    const char *dot =
                        strrchr(lbl, '.');

                    if (dot &&
                        *(dot + 1)) {

                        lbl = dot + 1;
                    }

                    const double fsz =
                        std::max(
                            5.0,
                            std::min(
                                8.0,
                                wh * 0.18));

                    cairo_set_font_size(
                        cr,
                        fsz);

                    cairo_text_extents_t e;

                    cairo_text_extents(
                        cr,
                        lbl,
                        &e);

                    if (e.width <
                        ww - 4) {

                        set_color(
                            cr,
                            C_TEXT,
                            0.55);

                        cairo_move_to(
                            cr,
                            wx +
                                (ww - e.width) /
                                2.0 -
                                e.x_bearing,
                            wy +
                                (wh + e.height) /
                                2.0 -
                                e.y_bearing / 2 +
                                bar);

                        cairo_show_text(
                            cr,
                            lbl);
                    }
                }

                rounded_rect(
                    cr,
                    wx,
                    wy,
                    ww,
                    wh,
                    2.0);

                set_color(
                    cr,
                    C_DIM,
                    0.4);

                cairo_set_line_width(
                    cr,
                    0.5);

                cairo_stroke(cr);
            }
        }

        if (!is_sel && !is_cur) {
            set_color(
                cr,
                0x000000,
                0.42);

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

        if (is_cur) {
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
        }

        int lx = cx;
        int ly = cy + ch + 2;

        cairo_set_font_size(
            cr,
            is_sel ? 12.0 : 10.5);

        if (is_sel)
            set_color(cr, C_SEL, 1.0);
        else if (is_cur)
            set_color(cr, C_TEXT, 0.85);
        else
            set_color(cr, C_TEXT_DIM, 0.6);

        char wslbl[16];

        snprintf(
            wslbl,
            sizeof(wslbl),
            "ws_%d",
            bws);

        cairo_move_to(
            cr,
            lx + 4,
            ly + OV_LABEL_H - 5);

        cairo_show_text(
            cr,
            wslbl);

        cairo_set_font_size(
            cr,
            10.0);

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
            prof);

        cairo_text_extents_t pe;

        cairo_text_extents(
            cr,
            plbl,
            &pe);

        cairo_move_to(
            cr,
            lx + cw -
                pe.width -
                pe.x_bearing -
                5,
            ly + OV_LABEL_H - 5);

        cairo_show_text(
            cr,
            plbl);

        if (is_cur) {
            set_color(
                cr,
                C_SEL,
                1.0);

            cairo_arc(
                cr,
                lx + cw / 2,
                ly + 5,
                2.5,
                0,
                2 * M_PI);

            cairo_fill(cr);
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
        "hjkl / ←↑↓→ : nav    "
        "1-6 : прямой переход    "
        "Enter : перейти    "
        "q/Esc : закрыть");
}

// ─────────────────────────── keyboard ────────────────────────────

static void on_key(
    xkb_keysym_t sym)
{
    if (g_stop_requested) {
        close_overview();
        return;
    }

    const int col =
        sel_ws % WS_COLS;

    const int row =
        sel_ws / WS_COLS;

    switch (sym) {

        case XKB_KEY_h:
        case XKB_KEY_Left:
            if (col > 0) {
                --sel_ws;
                win->request_redraw();
            }
            break;

        case XKB_KEY_l:
        case XKB_KEY_Right:
            if (col < WS_COLS - 1) {
                ++sel_ws;
                win->request_redraw();
            }
            break;

        case XKB_KEY_k:
        case XKB_KEY_Up:
            if (row > 0) {
                sel_ws -= WS_COLS;
                win->request_redraw();
            }
            break;

        case XKB_KEY_j:
        case XKB_KEY_Down:
            if (row < WS_ROWS - 1) {
                sel_ws += WS_COLS;
                win->request_redraw();
            }
            break;

        case XKB_KEY_1:
        case XKB_KEY_2:
        case XKB_KEY_3:
        case XKB_KEY_4:
        case XKB_KEY_5:
        case XKB_KEY_6: {
            const int n =
                sym - XKB_KEY_1;

            if (n < WS_COUNT) {
                sel_ws = n;
                win->request_redraw();
            }

            break;
        }

        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            goto_selected();
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
    int cw =
        (width -
         OV_GAP * (WS_COLS + 1))
        / WS_COLS;

    int ch =
        cw * MON_H / MON_W;

    int mh =
        (height -
         OV_GAP * (WS_ROWS + 1) -
         OV_LABEL_H * WS_ROWS)
        / WS_ROWS;

    if (ch > mh) {
        ch = mh;
        cw = ch * MON_W / MON_H;
    }

    const int ox =
        (width -
         (cw * WS_COLS +
          OV_GAP * (WS_COLS + 1))) / 2;

    const int oy =
        (height -
         ((ch + OV_LABEL_H) * WS_ROWS +
          OV_GAP * (WS_ROWS + 1))) / 2;

    for (int ws = 0;
         ws < WS_COUNT;
         ++ws) {

        const int c =
            ws % WS_COLS;

        const int r =
            ws / WS_COLS;

        const int cx =
            ox +
            OV_GAP +
            c * (cw + OV_GAP);

        const int cy =
            oy +
            OV_GAP +
            r * (ch + OV_LABEL_H + OV_GAP);

        if (x >= cx &&
            x <= cx + cw &&
            y >= cy &&
            y <= cy + ch) {

            sel_ws = ws;

            if (n_press == 2)
                goto_selected();
            else
                win->request_redraw();

            return;
        }
    }
}

// ─────────────────────────── main ────────────────────────────────

int main()
{
    signal(
            SIGTERM,
            signal_handler);

    signal(
            SIGINT,
            signal_handler);

    /*
     * spawn_detached() не дожидается дочерних процессов — без этого
     * они оставались бы зомби. SIG_IGN для SIGCHLD заставляет ядро
     * пожинать их автоматически.
     */
    signal(
            SIGCHLD,
            SIG_IGN);


    /*
     * Никакого:
     *
     * pkill profile-overview
     *
     * Это принципиально важно.
     */
    if (!acquire_single_instance(
                "ws-overview")) {

        /*
         * Экземпляр уже существует.
         *
         * toggle-overview.sh спавнит новый процесс только в ветке
         * missing) — когда hyprctl clients -j не находит окна с таким
         * title. Раз мы вообще дошли до сюда (flock провалился),
         * значит окно физически есть, а скрипт его почему-то не
         * увидел — в норме такого не бывает. Ничего не делаем:
         * правильная реакция на "уже запущен" — тихо выйти, уже
         * работающий процесс никак не трогаем.
         */
        return 0;
    }



    memset(
        &st,
        0,
        sizeof(st));

    st.cur_base_ws = 1;
    st.cur_profile = 1;

    sel_ws = 0;

    LayerWindow window(
        "ws-overview");

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
            "ws-overview: "
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
        15,
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
                    OV_RELOAD_FLAG,
                    F_OK) == 0) {

                remove(OV_RELOAD_FLAG);

                reload_and_redraw();
            }
        });

    window.add_timer(
        1000,
        [] {
            if (g_stop_requested) {
                close_overview();
                return;
            }

            const int sv =
                sel_ws;

            load_state();

            sel_ws = sv;

            if (sel_ws < 0)
                sel_ws = 0;

            if (sel_ws >= WS_COUNT)
                sel_ws = WS_COUNT - 1;

            win->request_redraw();
        });

    const int result =
        window.run();

    /*
     * Нормальный процесс завершения.
     */
    reset_submap();

    release_single_instance();

    return result;
}

