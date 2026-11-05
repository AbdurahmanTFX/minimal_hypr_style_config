#include "layer_window.hpp"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cmath>

// ─────────────────────────── registry ───────────────────────────

void LayerWindow::registry_global(
    void *data,
    wl_registry *reg,
    uint32_t name,
    const char *iface,
    uint32_t version)
{
    auto *self = static_cast<LayerWindow *>(data);
    std::string ifc = iface;

    if (ifc == wl_compositor_interface.name) {
        self->compositor_ = static_cast<wl_compositor *>(
            wl_registry_bind(
                reg,
                name,
                &wl_compositor_interface,
                std::min(version, 4u)));
    }
    else if (ifc == wl_shm_interface.name) {
        static const wl_shm_listener listener = {
            .format = shm_format
        };

        self->shm_ = static_cast<wl_shm *>(
            wl_registry_bind(
                reg,
                name,
                &wl_shm_interface,
                1));

        wl_shm_add_listener(self->shm_, &listener, self);
    }
    else if (ifc == wl_seat_interface.name) {
        static const wl_seat_listener listener = {
            .capabilities = seat_capabilities,
            .name         = seat_name,
        };

        self->seat_ = static_cast<wl_seat *>(
            wl_registry_bind(
                reg,
                name,
                &wl_seat_interface,
                std::min(version, 5u)));

        wl_seat_add_listener(self->seat_, &listener, self);
    }
    else if (ifc == xdg_wm_base_interface.name) {
        static const xdg_wm_base_listener listener = {
            .ping = xdg_wm_base_ping
        };

        self->wm_base_ = static_cast<xdg_wm_base *>(
            wl_registry_bind(
                reg,
                name,
                &xdg_wm_base_interface,
                std::min(version, 6u)));

        xdg_wm_base_add_listener(self->wm_base_, &listener, self);
    }
}

void LayerWindow::registry_global_remove(
    void *,
    wl_registry *,
    uint32_t)
{
}

void LayerWindow::shm_format(
    void *,
    wl_shm *,
    uint32_t)
{
}

// ─────────────────────────── seat / input ───────────────────────

void LayerWindow::seat_capabilities(
    void *data,
    wl_seat *seat,
    uint32_t caps)
{
    auto *self = static_cast<LayerWindow *>(data);

    const bool has_kbd = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    const bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;

    if (has_kbd && !self->keyboard_) {
        static const wl_keyboard_listener listener = {
            .keymap      = kbd_keymap,
            .enter       = kbd_enter,
            .leave       = kbd_leave,
            .key         = kbd_key,
            .modifiers   = kbd_modifiers,
            .repeat_info = kbd_repeat_info,
        };

        self->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(
            self->keyboard_,
            &listener,
            self);
    }
    else if (!has_kbd && self->keyboard_) {
        wl_keyboard_release(self->keyboard_);
        self->keyboard_ = nullptr;
    }

    if (has_ptr && !self->pointer_) {
        static const wl_pointer_listener listener = {
            .enter         = ptr_enter,
            .leave         = ptr_leave,
            .motion        = ptr_motion,
            .button        = ptr_button,
            .axis          = ptr_axis,
            .frame         = ptr_frame,
            .axis_source   = ptr_axis_source,
            .axis_stop     = ptr_axis_stop,
            .axis_discrete = ptr_axis_discrete,
        };

        self->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(
            self->pointer_,
            &listener,
            self);
    }
    else if (!has_ptr && self->pointer_) {
        wl_pointer_release(self->pointer_);
        self->pointer_ = nullptr;
    }
}

void LayerWindow::seat_name(
    void *,
    wl_seat *,
    const char *)
{
}

// ─────────────────────────── keyboard ───────────────────────────

void LayerWindow::kbd_keymap(
    void *data,
    wl_keyboard *,
    uint32_t format,
    int32_t fd,
    uint32_t size)
{
    auto *self = static_cast<LayerWindow *>(data);

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = static_cast<char *>(
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));

    if (map == MAP_FAILED) {
        close(fd);
        return;
    }

    if (self->xkb_state_) {
        xkb_state_unref(self->xkb_state_);
        self->xkb_state_ = nullptr;
    }

    if (self->xkb_keymap_) {
        xkb_keymap_unref(self->xkb_keymap_);
        self->xkb_keymap_ = nullptr;
    }

    self->xkb_keymap_ = xkb_keymap_new_from_string(
        self->xkb_ctx_,
        map,
        XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(map, size);
    close(fd);

    if (self->xkb_keymap_)
        self->xkb_state_ = xkb_state_new(self->xkb_keymap_);
}

void LayerWindow::kbd_enter(
    void *data,
    wl_keyboard *,
    uint32_t,
    wl_surface *,
    wl_array *)
{
    static_cast<LayerWindow *>(data)->keyboard_focused_ = true;
}

void LayerWindow::kbd_leave(
    void *data,
    wl_keyboard *,
    uint32_t,
    wl_surface *)
{
    static_cast<LayerWindow *>(data)->keyboard_focused_ = false;
}

void LayerWindow::kbd_key(
    void *data,
    wl_keyboard *,
    uint32_t,
    uint32_t,
    uint32_t key,
    uint32_t state)
{
    auto *self = static_cast<LayerWindow *>(data);

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    if (!self->xkb_state_)
        return;

    if (!self->running_)
        return;

    xkb_keysym_t sym =
        xkb_state_key_get_one_sym(
            self->xkb_state_,
            key + 8);

    if (sym == XKB_KEY_NoSymbol)
        return;

    // В русской раскладке буквы приходят кириллицей, а панели ждут латиницу:
    // берём символ той же клавиши из первой раскладки.
    if (xkb_keysym_to_utf32(sym) >= 0x80) {
        const xkb_keysym_t *syms = nullptr;
        const xkb_level_index_t level =
            xkb_state_key_get_level(
                self->xkb_state_,
                key + 8,
                xkb_state_key_get_layout(self->xkb_state_, key + 8));

        if (xkb_keymap_key_get_syms_by_level(
                self->xkb_keymap_, key + 8, 0, level, &syms) > 0) {

            sym = syms[0];
        }
    }

    if (self->key_fn_)
        self->key_fn_(sym);
}

void LayerWindow::kbd_modifiers(
    void *data,
    wl_keyboard *,
    uint32_t,
    uint32_t mods_depressed,
    uint32_t mods_latched,
    uint32_t mods_locked,
    uint32_t group)
{
    auto *self = static_cast<LayerWindow *>(data);

    if (self->xkb_state_) {
        xkb_state_update_mask(
            self->xkb_state_,
            mods_depressed,
            mods_latched,
            mods_locked,
            0,
            0,
            group);
    }
}

void LayerWindow::kbd_repeat_info(
    void *,
    wl_keyboard *,
    int32_t,
    int32_t)
{
}

// ─────────────────────────── pointer ─────────────────────────────

void LayerWindow::ptr_enter(
    void *data,
    wl_pointer *,
    uint32_t,
    wl_surface *,
    wl_fixed_t x,
    wl_fixed_t y)
{
    auto *self = static_cast<LayerWindow *>(data);

    self->ptr_x_ = wl_fixed_to_double(x);
    self->ptr_y_ = wl_fixed_to_double(y);
}

void LayerWindow::ptr_leave(
    void *,
    wl_pointer *,
    uint32_t,
    wl_surface *)
{
}

void LayerWindow::ptr_motion(
    void *data,
    wl_pointer *,
    uint32_t,
    wl_fixed_t x,
    wl_fixed_t y)
{
    auto *self = static_cast<LayerWindow *>(data);

    self->ptr_x_ = wl_fixed_to_double(x);
    self->ptr_y_ = wl_fixed_to_double(y);
}

void LayerWindow::ptr_button(
    void *data,
    wl_pointer *,
    uint32_t,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
    auto *self = static_cast<LayerWindow *>(data);

    if (state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    if (button != BTN_LEFT)
        return;

    if (!self->running_)
        return;

    int click_count = 1;

    const bool same_spot =
        std::abs(self->ptr_x_ - self->last_click_x_) < 4 &&
        std::abs(self->ptr_y_ - self->last_click_y_) < 4 &&
        self->last_click_button_ == button;

    if (same_spot &&
        (time - self->last_click_time_) < 400) {
        click_count = 2;
    }

    self->last_click_time_   = time;
    self->last_click_x_      = self->ptr_x_;
    self->last_click_y_      = self->ptr_y_;
    self->last_click_button_ = button;

    if (self->click_fn_)
        self->click_fn_(
            self->ptr_x_,
            self->ptr_y_,
            click_count);
}

void LayerWindow::ptr_axis(
    void *,
    wl_pointer *,
    uint32_t,
    uint32_t,
    wl_fixed_t)
{
}

void LayerWindow::ptr_frame(
    void *,
    wl_pointer *)
{
}

void LayerWindow::ptr_axis_source(
    void *,
    wl_pointer *,
    uint32_t)
{
}

void LayerWindow::ptr_axis_stop(
    void *,
    wl_pointer *,
    uint32_t,
    uint32_t)
{
}

void LayerWindow::ptr_axis_discrete(
    void *,
    wl_pointer *,
    uint32_t,
    int32_t)
{
}

// ─────────────────────────── xdg-shell ────────────────────────────

void LayerWindow::xdg_wm_base_ping(
    void *,
    xdg_wm_base *wm_base,
    uint32_t serial)
{
    /*
     * Обязательный pong — если компоситор не дождётся ответа на ping,
     * он вправе счесть клиент зависшим (то самое "окно не отвечает").
     * У layer-shell такого ритуала не было вообще.
     */
    xdg_wm_base_pong(wm_base, serial);
}

void LayerWindow::xdg_toplevel_configure(
    void *data,
    xdg_toplevel *,
    int32_t w,
    int32_t h,
    wl_array *)
{
    auto *self = static_cast<LayerWindow *>(data);

    /*
     * 0x0 на самом первом configure — норма протокола ("клиент, выбирай
     * сам"), обычно приходит ДО того, как компоситор применит
     * window_rule (float/fullscreen/size=100%) к ещё не замапленному
     * окну. Раньше (layer-shell) компоситор всегда присылал готовый
     * реальный размер сразу, фолбэк был не нужен — здесь же без него
     * width_/height_ так и оставались 0, render() (см. его guard) не
     * рисовал ничего НИКОГДА, буфер не прикреплялся, surface не
     * маплился вообще — окно не появлялось в hyprctl clients -j,
     * хотя процесс был жив. Даём разумный дефолт только для того,
     * чтобы дошло до первого commit и compositor увидел живое окно;
     * как только window_rule разрешится, придёт повторный configure
     * с настоящим размером и перерисует как надо.
     */
    if (w > 0)
        self->width_ = w;
    else if (self->width_ <= 0)
        self->width_ = 800;

    if (h > 0)
        self->height_ = h;
    else if (self->height_ <= 0)
        self->height_ = 600;
}

void LayerWindow::xdg_surface_configure(
    void *data,
    xdg_surface *surf,
    uint32_t serial)
{
    auto *self = static_cast<LayerWindow *>(data);

    xdg_surface_ack_configure(surf, serial);

    self->configured_ = true;
    self->dirty_      = true;
}

void LayerWindow::xdg_toplevel_close(
    void *data,
    xdg_toplevel *)
{
    auto *self = static_cast<LayerWindow *>(data);

    self->running_ = false;
}

void LayerWindow::xdg_toplevel_configure_bounds(
    void *,
    xdg_toplevel *,
    int32_t,
    int32_t)
{
    // Рекомендованный компоситором верхний предел размера — не нужен,
    // размер и так целиком диктует window_rule (size=100%/fullscreen).
}

void LayerWindow::xdg_toplevel_wm_capabilities(
    void *,
    xdg_toplevel *,
    wl_array *)
{
    // Список поддерживаемых компоситором toplevel-возможностей
    // (window menu/maximize/fullscreen/minimize) — не используем.
}

// ─────────────────────────── buffers ─────────────────────────────

void LayerWindow::buffer_release(
    void *data,
    wl_buffer *)
{
    auto *buffer = static_cast<Buffer *>(data);

    buffer->busy = false;
}

void LayerWindow::destroy_buffer(Buffer &b)
{
    if (b.cr) {
        cairo_destroy(b.cr);
        b.cr = nullptr;
    }

    if (b.csurf) {
        cairo_surface_destroy(b.csurf);
        b.csurf = nullptr;
    }

    if (b.wlbuf) {
        wl_buffer_destroy(b.wlbuf);
        b.wlbuf = nullptr;
    }

    if (b.data && b.data != MAP_FAILED) {
        munmap(b.data, b.size);
        b.data = nullptr;
    }

    b = Buffer{};
}

LayerWindow::Buffer *
LayerWindow::acquire_buffer(int w, int h)
{
    for (auto &b : buffers_) {
        if (!b.busy && b.w == w && b.h == h)
            return &b;
    }

    for (auto &b : buffers_) {
        if (b.busy)
            continue;

        destroy_buffer(b);

        const int stride =
            cairo_format_stride_for_width(
                CAIRO_FORMAT_ARGB32,
                w);

        const size_t size =
            static_cast<size_t>(stride) *
            static_cast<size_t>(h);

        const int fd =
            memfd_create(
                "hypr-overview-shm",
                MFD_CLOEXEC);

        if (fd < 0)
            return nullptr;

        if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
            close(fd);
            return nullptr;
        }

        void *data =
            mmap(
                nullptr,
                size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0);

        if (data == MAP_FAILED) {
            close(fd);
            return nullptr;
        }

        wl_shm_pool *pool =
            wl_shm_create_pool(
                shm_,
                fd,
                static_cast<int32_t>(size));

        if (!pool) {
            munmap(data, size);
            close(fd);
            return nullptr;
        }

        wl_buffer *wlbuf =
            wl_shm_pool_create_buffer(
                pool,
                0,
                w,
                h,
                stride,
                WL_SHM_FORMAT_ARGB8888);

        wl_shm_pool_destroy(pool);
        close(fd);

        if (!wlbuf) {
            munmap(data, size);
            return nullptr;
        }

        b.wlbuf = wlbuf;
        b.data  = data;
        b.size  = size;
        b.w     = w;
        b.h     = h;

        b.csurf =
            cairo_image_surface_create_for_data(
                static_cast<unsigned char *>(data),
                CAIRO_FORMAT_ARGB32,
                w,
                h,
                stride);

        if (cairo_surface_status(b.csurf) !=
            CAIRO_STATUS_SUCCESS) {
            destroy_buffer(b);
            return nullptr;
        }

        b.cr = cairo_create(b.csurf);

        if (cairo_status(b.cr) != CAIRO_STATUS_SUCCESS) {
            destroy_buffer(b);
            return nullptr;
        }

        static const wl_buffer_listener listener = {
            .release = buffer_release
        };

        wl_buffer_add_listener(
            wlbuf,
            &listener,
            &b);

        return &b;
    }

    return nullptr;
}

// ─────────────────────────── rendering ───────────────────────────

void LayerWindow::render()
{
    if (!running_)
        return;

    if (!configured_)
        return;

    if (hidden_)
        return;

    if (width_ <= 0 || height_ <= 0)
        return;

    /*
     * Шаг fade-анимации: считаем текущую альфу по прошедшему времени
     * (ease-out — быстрый старт, плавное затухание к цели), и если
     * анимация уже доиграна — фиксируем финальное значение.
     */
    if (fade_active_) {
        const double target = fade_to_one_ ? 1.0 : 0.0;

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - fade_start_)
                .count();

        double t = elapsed_ms / fade_duration_ms_;

        if (t >= 1.0) {
            t = 1.0;
            fade_active_ = false;
        }

        const double eased = 1.0 - (1.0 - t) * (1.0 - t);

        alpha_ = fade_alpha_from_ + (target - fade_alpha_from_) * eased;

        if (!fade_active_ && target == 0.0) {
            /*
             * fade_out доигран до конца — теперь делаем ровно то же,
             * что раньше делал мгновенный hide_now(): убираем буфер
             * с экрана и отпускаем keyboard focus. До этого момента
             * surface оставался замаплен (просто всё прозрачнее),
             * чтобы анимация вообще было видно.
             */
            hidden_ = true;
            alpha_  = 0.0;

            wl_surface_attach(surface_, nullptr, 0, 0);
            wl_surface_commit(surface_);
            wl_display_flush(display_);
            return;
        }
    }

    Buffer *buf =
        acquire_buffer(width_, height_);

    if (!buf)
        return;

    if (alpha_ >= 0.999) {
        /*
         * Обычный (не анимируемый) путь — рисуем прямо в буфер,
         * без лишнего промежуточного surface и compositing'а.
         */
        cairo_save(buf->cr);

        if (draw_fn_)
            draw_fn_(
                buf->cr,
                width_,
                height_);

        cairo_restore(buf->cr);
    }
    else {
        /*
         * Мидфейд: рисуем в отдельный ARGB-surface, затем очищаем
         * буфер до полностью прозрачного и композитим нарисованное
         * поверх него с текущей альфой (cairo_paint_with_alpha).
         * Буфер уже ARGB8888 без opaque_region — компоситор честно
         * учтёт per-pixel альфу.
         */
        cairo_surface_t *tmp_surf =
            cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32,
                width_,
                height_);

        cairo_t *tmp_cr = cairo_create(tmp_surf);

        if (draw_fn_)
            draw_fn_(
                tmp_cr,
                width_,
                height_);

        cairo_destroy(tmp_cr);

        cairo_save(buf->cr);

        cairo_set_operator(buf->cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(buf->cr);

        cairo_set_operator(buf->cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_surface(buf->cr, tmp_surf, 0, 0);
        cairo_paint_with_alpha(buf->cr, alpha_);

        cairo_restore(buf->cr);

        cairo_surface_destroy(tmp_surf);
    }

    cairo_surface_flush(buf->csurf);

    wl_surface_attach(
        surface_,
        buf->wlbuf,
        0,
        0);

    wl_surface_damage_buffer(
        surface_,
        0,
        0,
        width_,
        height_);

    wl_surface_commit(surface_);

    buf->busy = true;
}

// ─────────────────────────── ctor / dtor ─────────────────────────

LayerWindow::LayerWindow(std::string namespace_id)
    : namespace_id_(std::move(namespace_id))
{
    xkb_ctx_ =
        xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

LayerWindow::~LayerWindow()
{
    running_ = false;

    for (auto &t : timers_) {
        if (t.fd >= 0)
            close(t.fd);
    }

    timers_.clear();

    for (auto &b : buffers_)
        destroy_buffer(b);

    if (xdg_toplevel_) {
        xdg_toplevel_destroy(xdg_toplevel_);
        xdg_toplevel_ = nullptr;
    }

    if (xdg_surface_) {
        xdg_surface_destroy(xdg_surface_);
        xdg_surface_ = nullptr;
    }

    if (surface_) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }

    if (keyboard_) {
        wl_keyboard_destroy(keyboard_);
        keyboard_ = nullptr;
    }

    if (pointer_) {
        wl_pointer_destroy(pointer_);
        pointer_ = nullptr;
    }

    if (seat_) {
        wl_seat_destroy(seat_);
        seat_ = nullptr;
    }

    if (wm_base_) {
        xdg_wm_base_destroy(wm_base_);
        wm_base_ = nullptr;
    }

    if (shm_) {
        wl_shm_destroy(shm_);
        shm_ = nullptr;
    }

    if (compositor_) {
        wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
    }

    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }

    if (xkb_state_) {
        xkb_state_unref(xkb_state_);
        xkb_state_ = nullptr;
    }

    if (xkb_keymap_) {
        xkb_keymap_unref(xkb_keymap_);
        xkb_keymap_ = nullptr;
    }

    if (xkb_ctx_) {
        xkb_context_unref(xkb_ctx_);
        xkb_ctx_ = nullptr;
    }

    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

// ─────────────────────────── init ────────────────────────────────

bool LayerWindow::init()
{
    display_ = wl_display_connect(nullptr);

    if (!display_)
        return false;

    registry_ =
        wl_display_get_registry(display_);

    static const wl_registry_listener listener = {
        .global        = registry_global,
        .global_remove = registry_global_remove,
    };

    wl_registry_add_listener(
        registry_,
        &listener,
        this);

    if (wl_display_roundtrip(display_) < 0)
        return false;

    if (!compositor_ ||
        !shm_ ||
        !wm_base_) {
        return false;
    }

    surface_ =
        wl_compositor_create_surface(compositor_);

    if (!surface_)
        return false;

    xdg_surface_ =
        xdg_wm_base_get_xdg_surface(
            wm_base_,
            surface_);

    if (!xdg_surface_)
        return false;

    static const xdg_surface_listener xs_listener = {
        .configure = xdg_surface_configure,
    };

    xdg_surface_add_listener(
        xdg_surface_,
        &xs_listener,
        this);

    xdg_toplevel_ =
        xdg_surface_get_toplevel(xdg_surface_);

    if (!xdg_toplevel_)
        return false;

    static const xdg_toplevel_listener xt_listener = {
        .configure        = xdg_toplevel_configure,
        .close            = xdg_toplevel_close,
        .configure_bounds = xdg_toplevel_configure_bounds,
        .wm_capabilities  = xdg_toplevel_wm_capabilities,
    };

    xdg_toplevel_add_listener(
        xdg_toplevel_,
        &xt_listener,
        this);

    // title/app_id — то самое, чего не было у layer-shell: именно по
    // ним hyprctl clients -j и window_rule (class/title) видят окно.
    xdg_toplevel_set_title(
        xdg_toplevel_,
        namespace_id_.c_str());

    xdg_toplevel_set_app_id(
        xdg_toplevel_,
        namespace_id_.c_str());

    wl_surface_commit(surface_);

    if (wl_display_roundtrip(display_) < 0)
        return false;

    running_ = configured_;

    return running_;
}

// ─────────────────────────── control ─────────────────────────────

void LayerWindow::stop()
{
    if (!running_)
        return;

    running_ = false;

    if (surface_) {
        wl_surface_attach(
            surface_,
            nullptr,
            0,
            0);

        wl_surface_commit(surface_);

        wl_display_flush(display_);
    }
}

void LayerWindow::hide_now()
{
    if (!surface_ || !display_)
        return;

    hidden_ = true;

    wl_surface_attach(
        surface_,
        nullptr,
        0,
        0);

    wl_surface_commit(surface_);

    wl_display_flush(display_);
}

void LayerWindow::show_now()
{
    if (!surface_ || !display_)
        return;

    hidden_ = false;

    /*
     * Не рисуем/не коммитим прямо здесь — это может вызываться из
     * контекста, где ещё не готовы актуальные данные для отрисовки
     * (например, сразу после сигнала toggle, до вызова load_state()
     * в коде приложения). Вместо этого просто помечаем dirty — и на
     * следующей итерации event loop render() сам прикрепит буфер и
     * закоммитит surface, тем самым показав окно и вернув keyboard
     * focus (см. комментарий у render()/hidden_).
     */
    dirty_ = true;
}

void LayerWindow::fade_in(int duration_ms)
{
    if (!surface_ || !display_)
        return;

    hidden_ = false;

    fade_alpha_from_  = alpha_;
    fade_to_one_      = true;
    fade_duration_ms_ = duration_ms > 0 ? duration_ms : 1;
    fade_start_       = std::chrono::steady_clock::now();
    fade_active_      = true;

    dirty_ = true;
}

void LayerWindow::fade_out(int duration_ms)
{
    if (!surface_ || !display_)
        return;

    /*
     * hidden_ НЕ трогаем здесь — окно должно продолжать рендериться
     * (со снижающейся альфой) до конца анимации. hidden_ = true и
     * фактический detach буфера произойдут в конце анимации, в
     * render() (см. там) — так же, как раньше это делал hide_now(),
     * просто с задержкой на duration_ms.
     */
    fade_alpha_from_  = alpha_;
    fade_to_one_      = false;
    fade_duration_ms_ = duration_ms > 0 ? duration_ms : 1;
    fade_start_       = std::chrono::steady_clock::now();
    fade_active_      = true;

    dirty_ = true;
}

void LayerWindow::add_timer(
    int interval_ms,
    TimerFn fn)
{
    if (interval_ms <= 0 || !fn)
        return;

    const int fd =
        timerfd_create(
            CLOCK_MONOTONIC,
            TFD_CLOEXEC);

    if (fd < 0)
        return;

    itimerspec spec{};

    spec.it_value.tv_sec =
        interval_ms / 1000;

    spec.it_value.tv_nsec =
        (interval_ms % 1000) * 1000000L;

    spec.it_interval = spec.it_value;

    if (timerfd_settime(
            fd,
            0,
            &spec,
            nullptr) < 0) {
        close(fd);
        return;
    }

    timers_.push_back({
        fd,
        std::move(fn)
    });
}

// ─────────────────────────── event loop ──────────────────────────

int LayerWindow::run()
{
    if (!display_ || !running_)
        return 1;

    const int wl_fd =
        wl_display_get_fd(display_);

    if (wl_fd < 0)
        return 1;

    std::vector<pollfd> pfds;

    while (running_) {
        pfds.clear();

        pfds.push_back({
            wl_fd,
            POLLIN,
            0
        });

        for (auto &t : timers_) {
            pfds.push_back({
                t.fd,
                POLLIN,
                0
            });
        }

        /*
         * Подготавливаем чтение Wayland.
         *
         * Если pending events уже есть — сначала их обрабатываем.
         */
        while (wl_display_prepare_read(display_) != 0) {
            if (wl_display_dispatch_pending(display_) < 0) {
                running_ = false;
                break;
            }
        }

        if (!running_)
            break;

        if (wl_display_flush(display_) < 0) {
            if (errno != EAGAIN) {
                wl_display_cancel_read(display_);
                running_ = false;
                break;
            }
        }

        /*
         * Если dirty — не блокируемся.
         */
        const int timeout_ms =
            dirty_ ? 0 : -1;

        const int ret =
            poll(
                pfds.data(),
                pfds.size(),
                timeout_ms);

        if (ret < 0) {
            wl_display_cancel_read(display_);

            if (errno == EINTR)
                continue;

            running_ = false;
            break;
        }

        if (pfds[0].revents &
            (POLLIN | POLLHUP | POLLERR)) {

            if (wl_display_read_events(display_) < 0) {
                running_ = false;
                break;
            }
        }
        else {
            wl_display_cancel_read(display_);
        }

        if (wl_display_dispatch_pending(display_) < 0) {
            running_ = false;
            break;
        }

        for (size_t i = 1;
             i < pfds.size();
             ++i) {

            if (!(pfds[i].revents & POLLIN))
                continue;

            uint64_t expirations = 0;

            const ssize_t n =
                read(
                    pfds[i].fd,
                    &expirations,
                    sizeof(expirations));

            if (n != sizeof(expirations))
                continue;

            /*
             * Индекс timer'а соответствует
             * i - 1.
             */
            if (i - 1 < timers_.size() &&
                timers_[i - 1].fn &&
                running_) {

                timers_[i - 1].fn();
            }
        }

        if (dirty_ &&
            configured_ &&
            running_) {

            render();

            /*
             * Пока идёт fade-анимация — не сбрасываем dirty:
             * следующая итерация должна тут же нарисовать
             * следующий кадр (poll() увидит dirty_==true и не
             * будет блокироваться, см. timeout_ms выше). render()
             * сама выставляет fade_active_ = false, когда анимация
             * доиграна; тогда мы наконец вернёмся к обычному
             * "рисуем только когда действительно что-то попросили".
             */
            dirty_ = fade_active_;
        }
    }

    return 0;
}

