#!/bin/bash
# toggle-overview.sh — ws-overview снова обычное toplevel-окно (см.
# layer_window.cpp: xdg_toplevel вместо layer-shell), title
# "ws-overview", видно в hyprctl clients -j — точно так же, как
# toggle-dashboard.sh уже работает для dashboard.

source ~/.config/hypr/MyScripts/config.sh
init_storage

_dispatch() {
    hyprctl dispatch "$1"
}

visible=$(panel_state title ws-overview)

# Открываем только если нет другой панели; закрывать всегда можно
if [ "$visible" != "visible" ] && other_panel_open "ws-overview"; then
    exit 0
fi

case $visible in
    visible)
        _dispatch 'hl.dsp.window.move({ workspace = "special:overview", window = "title:ws-overview", follow = false })'
        _dispatch 'hl.dsp.submap("reset")'
        ;;
    hidden)
        capture_ws $(base_ws_of $(current_ws_id)) $(profile_of $(current_ws_id))
        touch /tmp/ws-overview-reload
        _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "title:ws-overview", follow = false })'
        _dispatch 'hl.dsp.focus({ window = "title:ws-overview" })'
        _dispatch 'hl.dsp.submap("overview")'
        ;;
    missing)
        # Если "missing" определилось ошибочно (осиротевшее окно ещё
        # физически существует) — сначала закрыть его, потом спавнить
        # новый процесс.
        _dispatch 'hl.dsp.window.close({ window = "title:ws-overview" })' 2>/dev/null
        capture_ws $(base_ws_of $(current_ws_id)) $(profile_of $(current_ws_id))
        ~/.config/hypr/ws-overview &
        found=0
        for i in $(seq 1 20); do
            sleep 0.1
            if [ "$(panel_state title ws-overview)" != missing ]; then
                found=1
                break
            fi
        done
        if [ "$found" -eq 1 ]; then
            touch /tmp/ws-overview-reload
            _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "title:ws-overview", follow = false })'
            _dispatch 'hl.dsp.focus({ window = "title:ws-overview" })'
            _dispatch 'hl.dsp.submap("overview")'
        fi
        ;;
esac
