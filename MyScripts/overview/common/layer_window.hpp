#pragma once

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo/cairo.h>

#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

#include "xdg-shell-client-protocol.h"

class LayerWindow {
public:
    using DrawFn  = std::function<void(cairo_t*, int width, int height)>;
    using KeyFn   = std::function<void(xkb_keysym_t sym)>;
    using ClickFn = std::function<void(double x, double y, int click_count)>;
    using TimerFn = std::function<void()>;

    explicit LayerWindow(std::string namespace_id);
    ~LayerWindow();

    LayerWindow(const LayerWindow&) = delete;
    LayerWindow& operator=(const LayerWindow&) = delete;

    bool init();

    void stop();

    void set_draw_fn(DrawFn fn)   { draw_fn_ = std::move(fn); }
    void set_key_fn(KeyFn fn)     { key_fn_ = std::move(fn); }
    void set_click_fn(ClickFn fn) { click_fn_ = std::move(fn); }

    void add_timer(int interval_ms, TimerFn fn);

    void request_redraw() {
        dirty_ = true;
    }

    /*
     * Мгновенно убрать surface с экрана и, что особенно важно,
     * прекратить держать keyboard focus.
     *
     * Это вызывается перед блокирующим system()/hyprctl, а также
     * как часть toggle-режима (см. show_now()) — окно остаётся
     * полностью инициализированным (Wayland-соединение, xdg_surface/
     * xdg_toplevel и т.д. не разрушаются), просто не отображается и
     * не получает ввод. Event loop продолжает работать.
     */
    void hide_now();

    /*
     * Обратная операция к hide_now(): снова показать surface и
     * вернуть keyboard focus. Не требует повторной инициализации —
     * работает на уже существующем Wayland-соединении/поверхности.
     *
     * Достаточно один раз вызвать init(), после чего hide_now() и
     * show_now() можно чередовать сколько угодно раз в рамках
     * ОДНОГО процесса — это и есть toggle-режим без spawn/kill.
     */
    void show_now();

    bool is_hidden() const {
        return hidden_;
    }

    /*
     * В отличие от is_hidden() учитывает НЕ только текущее состояние,
     * но и то, куда идёт анимация, если она сейчас активна — то есть
     * "логическую" цель, а не финальный физический hidden_ (который
     * при fade_out становится true только в самом конце анимации).
     * Именно этим методом (а не is_hidden()) должен пользоваться
     * toggle: иначе повторное нажатие хоткея ПОКА окно ещё доигрывает
     * fade_out не развернёт анимацию обратно в показ, а просто
     * перезапустит то же скрытие.
     */
    bool wants_visible() const {
        return fade_active_ ? fade_to_one_ : !hidden_;
    }

    /*
     * Плавные версии hide_now()/show_now(): линейная (с ease-out)
     * анимация альфа-канала за duration_ms, БЕЗ пересоздания surface —
     * тот же принцип, тот же persistent-процесс, просто вместо
     * мгновенного attach(nullptr)/attach(buf) — несколько
     * промежуточных кадров с прозрачностью. Буфер уже ARGB8888 и без
     * opaque_region, так что per-pixel альфа и так честно уходит в
     * компоситор — не хватало только самой анимации.
     *
     * Можно звать поверх уже идущей анимации (например, показать,
     * пока не досказалось скрытие) — продолжит плавно с текущего
     * значения альфы, а не дёрнется.
     */
    void fade_in(int duration_ms = 140);
    void fade_out(int duration_ms = 140);

    /*
     * Завершить event loop.
     */
    void request_close() {
        running_ = false;
        dirty_ = false;
    }

    bool is_running() const {
        return running_;
    }

    int run();

private:
    struct Buffer {
        wl_buffer        *wlbuf = nullptr;
        void             *data = nullptr;
        size_t            size = 0;
        int                w = 0;
        int                h = 0;
        cairo_surface_t  *csurf = nullptr;
        cairo_t          *cr = nullptr;
        bool               busy = false;
    };

    struct TimerEntry {
        int fd = -1;
        TimerFn fn;
    };

    // ───────────────────── Wayland ─────────────────────

    wl_display    *display_     = nullptr;
    wl_registry   *registry_    = nullptr;
    wl_compositor *compositor_  = nullptr;
    wl_shm        *shm_         = nullptr;
    wl_seat       *seat_        = nullptr;
    wl_keyboard   *keyboard_    = nullptr;
    wl_pointer    *pointer_     = nullptr;

    xdg_wm_base  *wm_base_      = nullptr;
    wl_surface   *surface_      = nullptr;
    xdg_surface  *xdg_surface_  = nullptr;
    xdg_toplevel *xdg_toplevel_ = nullptr;

    xkb_context *xkb_ctx_    = nullptr;
    xkb_keymap  *xkb_keymap_ = nullptr;
    xkb_state   *xkb_state_  = nullptr;

    Buffer buffers_[2];

    int width_  = 0;
    int height_ = 0;

    bool configured_ = false;
    bool running_    = true;
    bool dirty_      = true;
    bool hidden_     = false;

    // ────────────────────── fade-анимация ──────────────────────
    double alpha_          = 1.0;
    bool   fade_active_    = false;
    bool   fade_to_one_    = true;   // true = fade_in, false = fade_out
    double fade_alpha_from_ = 1.0;   // альфа в момент старта анимации
    std::chrono::steady_clock::time_point fade_start_;
    int    fade_duration_ms_ = 140;

    std::string namespace_id_;

    DrawFn  draw_fn_;
    KeyFn   key_fn_;
    ClickFn click_fn_;

    double ptr_x_ = 0.0;
    double ptr_y_ = 0.0;

    uint32_t last_click_time_   = 0;
    double   last_click_x_      = -1000.0;
    double   last_click_y_      = -1000.0;
    uint32_t last_click_button_ = 0;

    std::vector<TimerEntry> timers_;

    // ───────────────────── listeners ───────────────────

    static void registry_global(
        void*,
        wl_registry*,
        uint32_t,
        const char*,
        uint32_t
    );

    static void registry_global_remove(
        void*,
        wl_registry*,
        uint32_t
    );

    static void shm_format(
        void*,
        wl_shm*,
        uint32_t
    );

    static void seat_capabilities(
        void*,
        wl_seat*,
        uint32_t
    );

    static void seat_name(
        void*,
        wl_seat*,
        const char*
    );

    static void kbd_keymap(
        void*,
        wl_keyboard*,
        uint32_t,
        int32_t,
        uint32_t
    );

    static void kbd_enter(
        void*,
        wl_keyboard*,
        uint32_t,
        wl_surface*,
        wl_array*
    );

    static void kbd_leave(
        void*,
        wl_keyboard*,
        uint32_t,
        wl_surface*
    );

    static void kbd_key(
        void*,
        wl_keyboard*,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t
    );

    static void kbd_modifiers(
        void*,
        wl_keyboard*,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t
    );

    static void kbd_repeat_info(
        void*,
        wl_keyboard*,
        int32_t,
        int32_t
    );

    static void ptr_enter(
        void*,
        wl_pointer*,
        uint32_t,
        wl_surface*,
        wl_fixed_t,
        wl_fixed_t
    );

    static void ptr_leave(
        void*,
        wl_pointer*,
        uint32_t,
        wl_surface*
    );

    static void ptr_motion(
        void*,
        wl_pointer*,
        uint32_t,
        wl_fixed_t,
        wl_fixed_t
    );

    static void ptr_button(
        void*,
        wl_pointer*,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t
    );

    static void ptr_axis(
        void*,
        wl_pointer*,
        uint32_t,
        uint32_t,
        wl_fixed_t
    );

    static void ptr_frame(
        void*,
        wl_pointer*
    );

    static void ptr_axis_source(
        void*,
        wl_pointer*,
        uint32_t
    );

    static void ptr_axis_stop(
        void*,
        wl_pointer*,
        uint32_t,
        uint32_t
    );

    static void ptr_axis_discrete(
        void*,
        wl_pointer*,
        uint32_t,
        int32_t
    );

    static void xdg_wm_base_ping(
        void*,
        xdg_wm_base*,
        uint32_t
    );

    static void xdg_surface_configure(
        void*,
        xdg_surface*,
        uint32_t
    );

    static void xdg_toplevel_configure(
        void*,
        xdg_toplevel*,
        int32_t,
        int32_t,
        wl_array*
    );

    static void xdg_toplevel_close(
        void*,
        xdg_toplevel*
    );

    /*
     * configure_bounds (с версии 4) и wm_capabilities (с версии 5) —
     * необязательные для нас события, но раз xdg_wm_base забинден
     * вплоть до версии 6 (см. registry_global), компоситор вправе их
     * прислать, а libwayland падает в abort, если у listener'а для
     * пришедшего опкода функция NULL. Оставляем no-op реализации —
     * без них было "listener function for opcode 3 of xdg_toplevel
     * is NULL".
     */
    static void xdg_toplevel_configure_bounds(
        void*,
        xdg_toplevel*,
        int32_t,
        int32_t
    );

    static void xdg_toplevel_wm_capabilities(
        void*,
        xdg_toplevel*,
        wl_array*
    );

    static void buffer_release(
        void*,
        wl_buffer*
    );

    // ───────────────────── internal ────────────────────

    void render();

    Buffer* acquire_buffer(
        int w,
        int h
    );

    void destroy_buffer(
        Buffer& b
    );

    void release_keyboard_focus();

    void destroy_wayland_objects();

    bool dispatch_pending();

    bool prepare_read();

    bool flush_display();
};

