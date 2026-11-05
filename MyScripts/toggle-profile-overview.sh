#!/bin/bash
# toggle-profile-overview.sh — profile-overview снова обычное
# toplevel-окно (см. layer_window.cpp: xdg_toplevel), title
# "profile-overview", видно в hyprctl clients -j.

source ~/.config/hypr/MyScripts/config.sh
init_storage

_dispatch() {
    hyprctl dispatch "$1"
}

CUR_WS=$(hyprctl activeworkspace -j 2>/dev/null | jq -r '.id // 1')
BASE=$(base_ws_of $CUR_WS)
PROF=$(profile_of $CUR_WS)

visible=$(hyprctl clients -j 2>/dev/null | jq -r '
  ([ .[] | select(.title=="profile-overview") ] | first) as $w |
  if $w == null then "missing"
  elif ($w.workspace.name | contains("special")) then "hidden"
  else "visible" end
')

# Открываем только если нет другой панели; закрывать всегда можно
if [ "$visible" != "visible" ] && other_panel_open "profile-overview"; then
    exit 0
fi

case $visible in
    visible)
        _dispatch 'hl.dsp.window.move({ workspace = "special:profiles", window = "title:profile-overview", follow = false })'
        _dispatch 'hl.dsp.submap("reset")'
        ;;
    hidden)
        # Быстрый путь: процесс жив, просто показываем и обновляем
        capture_ws_async $BASE $PROF
        _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "title:profile-overview", follow = false })'
        _dispatch 'hl.dsp.focus({ window = "title:profile-overview" })'
        _dispatch 'hl.dsp.submap("profile-overview")'
        touch "$POV_RELOAD_FLAG" 2>/dev/null
        ;;
    missing)
        # Медленный путь: первый запуск или процесс погиб
        _dispatch 'hl.dsp.window.close({ window = "title:profile-overview" })' 2>/dev/null
        capture_ws_async $BASE $PROF
        ~/.config/hypr/profile-overview "$CUR_WS" &
        found=0
        for i in $(seq 1 40); do
            sleep 0.05
            if hyprctl clients -j 2>/dev/null | jq -e \
                'any(.[]; .title=="profile-overview")' >/dev/null 2>&1; then
                found=1
                break
            fi
        done
        if [ "$found" -eq 1 ]; then
            hyprctl setprop title:profile-overview noanim 1
            _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "title:profile-overview", follow = false })'
            _dispatch 'hl.dsp.focus({ window = "title:profile-overview" })'
            _dispatch 'hl.dsp.submap("profile-overview")'
        fi
        ;;
esac
