/*
 * hidden_apps_view.cpp
 *
 * Панель "скрытые окна профиля" — список того, что было спрятано по
 * Win+Alt+C для конкретного (base_ws, профиль). Открывается по
 * Win+Alt+V (см. toggle-hidden-apps.sh) или клавишей "s" внутри
 * profile-overview для произвольной ячейки.
 *
 * Тот же принцип, что и profile_overview.cpp: обычный xdg_toplevel
 * (см. layer_window.cpp/hpp), собственный lock-файл, submap, park в
 * special-воркспейс вместо уничтожения процесса.
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

#define HAV_ROW_H   34
#define HAV_HDR_H   46
#define HAV_MAX     64

// ─────────────────────────── signal state ────────────────────────

static volatile sig_atomic_t g_stop_requested = 0;

static void signal_handler(int)
{
    /* Никаких system()/malloc()/Wayland-вызовов внутри обработчика. */
    g_stop_requested = 1;
}

// ─────────────────────────── submap ──────────────────────────────

static void reset_submap()
{
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

    snprintf(lock_path, sizeof(lock_path), "/tmp/%s.lock", app_name);

    g_lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (g_lock_fd < 0)
        return false;

    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            char pid_buf[32] = {0};
            lseek(g_lock_fd, 0, SEEK_SET);
            ssize_t n = read(g_lock_fd, pid_buf, sizeof(pid_buf) - 1);
            fprintf(stderr,
                "hidden-apps-view: уже запущен (pid %s) — выхожу\n",
                (n > 0) ? pid_buf : "?");
        }
        close(g_lock_fd);
        g_lock_fd = -1;
        return false;
    }

    if (ftruncate(g_lock_fd, 0) == 0) {
        char pid_buf[32];
        snprintf(pid_buf, sizeof(pid_buf), "%d\n", (int)getpid());
        write(g_lock_fd, pid_buf, strlen(pid_buf));
        lseek(g_lock_fd, 0, SEEK_SET);
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

struct HAEntry {
    char addr[24];
    char cls[48];
    char title[96];
};

static HAEntry entries[HAV_MAX];
static int     entry_count = 0;
static int     sel         = 0;

static int g_base = 1;
static int g_prof = 1;

// Панель открыта из profile-overview (клавиша "s" на ВЫБРАННОЙ, не
// обязательно активной ячейке) — в этом случае q/Esc должен вернуть
// именно туда, а не просто закрыть себя. Пишет toggle-hidden-apps.sh
// третьим полем в HAV_TARGET_FILE (1 — да, 0 — открыто напрямую по
// Win+Alt+V).
static bool g_return_to_pov = false;

static LayerWindow *win = nullptr;

// ─────────────────────────── storage ─────────────────────────────

/* Разбивает "addr\x1fclass\x1ftitle\n" на поля e; портит буфер line. */
static void split_line(char *line, HAEntry *e)
{
    e->addr[0] = e->cls[0] = e->title[0] = '\0';

    char *f[3] = { nullptr, nullptr, nullptr };
    int   fi   = 0;

    f[fi++] = line;

    for (char *p = line; *p; ++p) {
        if (*p == '\x1f' && fi < 3) {
            *p = '\0';
            f[fi++] = p + 1;
        } else if (*p == '\n') {
            *p = '\0';
        }
    }

    if (f[0]) snprintf(e->addr,  sizeof(e->addr),  "%s", f[0]);
    if (f[1]) snprintf(e->cls,   sizeof(e->cls),   "%s", f[1]);
    if (f[2]) snprintf(e->title, sizeof(e->title), "%s", f[2]);
}

/* Перечитывает, какой (base,prof) сейчас нужно показывать — пишется
 * toggle-hidden-apps.sh при каждом открытии/переключении, в т.ч. на
 * уже работающем процессе (см. HAV_RELOAD_FLAG). */
static void load_target()
{
    FILE *f = fopen(HAV_TARGET_FILE, "r");
    if (!f)
        return;

    int b = 0, p = 0, ret = 0;
    int n = fscanf(f, "%d %d %d", &b, &p, &ret);
    if (n >= 2) {
        if (b >= 1 && b <= WS_COUNT)
            g_base = b;
        if (p >= 1 && p <= PROFILE_COUNT)
            g_prof = p;
    }
    g_return_to_pov = (n >= 3) && (ret != 0);
    fclose(f);
}

static void load_entries()
{
    entry_count = 0;

    char path[128];
    snprintf(path, sizeof(path), "%s/ws_%d_p%d", HIDDEN_APPS_DIR, g_base, g_prof);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[256];
    while (entry_count < HAV_MAX && fgets(line, sizeof(line), f)) {
        if (line[0] == '\0' || line[0] == '\n')
            continue;
        split_line(line, &entries[entry_count]);
        ++entry_count;
    }
    fclose(f);

    if (sel >= entry_count)
        sel = (entry_count > 0) ? entry_count - 1 : 0;
    if (sel < 0)
        sel = 0;
}

static void reload()
{
    load_target();
    load_entries();
    if (win)
        win->request_redraw();
}

// ─────────────────────────── actions ─────────────────────────────

/*
 * Прячет панель ровно так же, как park_hidden() в profile_overview.cpp
 * и ws_overview.cpp: ТОЛЬКО window.move на special-workspace + сброс
 * submap. Никакого win->hide_now() здесь быть не должно — hide_now()
 * реально отсоединяет буфер (wl_surface_attach(nullptr) + commit), что
 * по xdg-shell анмапит поверхность, а обратно (show_now()) её никто не
 * звал — из-за этого после первого q/Esc панель переставала открываться
 * вообще. Surface остаётся замапленным, просто сидит на неактивном
 * special-воркспейсе, невидимом пользователю, — как и у остальных
 * панелей.
 */
static void park()
{
    spawn_detached(
        "hyprctl dispatch "
        "'hl.dsp.window.move({ workspace = \"special:hiddenview\", "
        "window = \"title:hidden-apps-view\", follow = false })' "
        ">/dev/null 2>&1");

    reset_submap();
}

/*
 * q/Esc и «закрыть после восстановления окна»: если панель была открыта
 * из profile-overview (клавиша "s" — см. g_return_to_pov), прячем себя
 * и сразу же снова открываем profile-overview (она уже запаркована тем
 * же способом своим собственным park_hidden(), просто повторно дёргаем
 * её toggle-скрипт — он видит "hidden" и переоткрывает уже работающий
 * процесс). Небольшая sleep — та же гонка с other_panel_open(), что и
 * везде в этом кодовом пути (см. profile_overview.cpp:
 * open_hidden_for_selected()), приемлемо.
 *
 * Если панель была открыта напрямую по Win+Alt+V — просто прячем себя,
 * как раньше.
 */
static void close_or_back()
{
    if (g_return_to_pov) {
        spawn_detached(
            "hyprctl dispatch "
            "'hl.dsp.window.move({ workspace = \"special:hiddenview\", "
            "window = \"title:hidden-apps-view\", follow = false })' "
            ">/dev/null 2>&1; "
            "sleep 0.05; "
            "~/.config/hypr/MyScripts/toggle-profile-overview.sh "
            ">/dev/null 2>&1");

        reset_submap();
    } else {
        park();
    }
}

/*
 * Настоящее завершение процесса — только по сигналу (SIGTERM/SIGINT),
 * не по обычному q/Esc. См. тот же принцип в profile_overview.cpp:
 * close_overview() (win->stop()) vs park_hidden().
 */
static void terminate()
{
    reset_submap();

    if (win)
        win->stop();
}

static void restore_selected()
{
    if (entry_count == 0 || sel < 0 || sel >= entry_count)
        return;

    char cmd[512];
    snprintf(
        cmd, sizeof(cmd),
        "~/.config/hypr/MyScripts/hidden-apps.sh restore-addr '%s' %d %d "
        ">/dev/null 2>&1",
        entries[sel].addr, g_base, g_prof);

    spawn_detached(cmd);
    close_or_back();
}

// ─────────────────────────── draw ────────────────────────────────

static void draw_func(cairo_t *cr, int width, int height)
{
    set_color(cr, C_BG, 1.0);
    cairo_paint(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18.0);
    set_color(cr, C_TEXT, 1.0);

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "Скрытые окна — Рабочий стол %d · P%d", g_base, g_prof);
    cairo_move_to(cr, OV_GAP, 30);
    cairo_show_text(cr, hdr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    if (entry_count == 0) {
        cairo_set_font_size(cr, 14.0);
        set_color(cr, C_TEXT_DIM, 0.8);
        cairo_move_to(cr, OV_GAP, HAV_HDR_H + 24);
        cairo_show_text(cr, "Нет скрытых окон в этом профиле — Win+Alt+C, чтобы скрыть активное");
    }

    for (int i = 0; i < entry_count; ++i) {
        int ry = HAV_HDR_H + i * HAV_ROW_H;

        if (i == sel) {
            set_color(cr, C_CELL_BG, 1.0);
            cairo_rectangle(cr, OV_GAP / 2, ry, width - OV_GAP, HAV_ROW_H - 4);
            cairo_fill(cr);

            set_color(cr, C_BORDER_SEL, 1.0);
            cairo_rectangle(cr, OV_GAP / 2, ry, width - OV_GAP, HAV_ROW_H - 4);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }

        cairo_set_font_size(cr, 13.0);
        set_color(cr, (i == sel) ? C_TEXT : C_TEXT_DIM, 1.0);

        char idx[8];
        snprintf(idx, sizeof(idx), "%2d.", i + 1);
        cairo_move_to(cr, OV_GAP + 4, ry + HAV_ROW_H / 2 + 5);
        cairo_show_text(cr, idx);

        const char *t = entries[i].title[0] ? entries[i].title : entries[i].cls;
        char label[192];
        snprintf(label, sizeof(label), "%s  —  %s", t, entries[i].cls);
        cairo_move_to(cr, OV_GAP + 36, ry + HAV_ROW_H / 2 + 5);
        cairo_show_text(cr, label);
    }

    cairo_set_font_size(cr, 10.0);
    set_color(cr, C_TEXT_DIM, 0.55);
    cairo_move_to(cr, OV_GAP, height - 7);
    cairo_show_text(cr,
        g_return_to_pov
            ? "j/k : выбор    Enter : вернуть    "
              "r : обновить    q/Esc : назад к профилям"
            : "j/k : выбор    Enter : вернуть    "
              "r : обновить    q/Esc : закрыть");
}

// ─────────────────────────── keyboard ────────────────────────────

static void on_key(xkb_keysym_t sym)
{
    if (g_stop_requested) {
        terminate();
        return;
    }

    switch (sym) {

        case XKB_KEY_j:
        case XKB_KEY_Down:
            if (sel < entry_count - 1) {
                ++sel;
                win->request_redraw();
            }
            break;

        case XKB_KEY_k:
        case XKB_KEY_Up:
            if (sel > 0) {
                --sel;
                win->request_redraw();
            }
            break;

        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_a:
        case XKB_KEY_A:
            restore_selected();
            break;

        case XKB_KEY_r:
        case XKB_KEY_R:
            reload();
            break;

        case XKB_KEY_q:
        case XKB_KEY_Q:
        case XKB_KEY_Escape:
            close_or_back();
            break;

        default:
            break;
    }
}

// ─────────────────────────── mouse ───────────────────────────────

static void on_click(double x, double y, int n_press, int width, int height)
{
    (void)width;
    (void)height;

    if (y < HAV_HDR_H)
        return;

    int i = (int)((y - HAV_HDR_H) / HAV_ROW_H);
    if (i < 0 || i >= entry_count)
        return;

    sel = i;

    if (n_press == 2)
        restore_selected();
    else if (win)
        win->request_redraw();
}

// ─────────────────────────── main ────────────────────────────────

int main(int argc, char *argv[])
{
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    signal(SIGCHLD, SIG_IGN);

    if (!acquire_single_instance("hidden-apps-view"))
        return 0;

    int raw = (argc > 1) ? atoi(argv[1]) : 1;

    g_base = BASE_WS(raw);
    g_prof = PROFILE_N(raw);

    if (g_base < 1 || g_base > WS_COUNT)
        g_base = 1;
    if (g_prof < 1 || g_prof > PROFILE_COUNT)
        g_prof = 1;

    sel = 0;

    LayerWindow window("hidden-apps-view");
    win = &window;

    int last_width  = 0;
    int last_height = 0;

    window.set_draw_fn([&](cairo_t *cr, int w, int h) {
        last_width  = w;
        last_height = h;
        draw_func(cr, w, h);
    });

    window.set_key_fn(on_key);

    window.set_click_fn([&](double x, double y, int n_press) {
        on_click(x, y, n_press, last_width, last_height);
    });

    if (!window.init()) {
        fprintf(stderr, "hidden-apps-view: не удалось инициализировать окно\n");
        release_single_instance();
        return 1;
    }

    window.add_timer(10, [] {
        static bool initial = true;
        if (!initial)
            return;
        if (g_stop_requested) {
            terminate();
            return;
        }
        reload();
        initial = false;
    });

    window.add_timer(100, [] {
        if (g_stop_requested) {
            terminate();
            return;
        }
        if (access(HAV_RELOAD_FLAG, F_OK) == 0) {
            remove(HAV_RELOAD_FLAG);
            reload();
        }
    });

    window.add_timer(1000, [] {
        if (g_stop_requested) {
            terminate();
            return;
        }
        reload();
    });

    const int result = window.run();

    reset_submap();
    release_single_instance();

    return result;
}
