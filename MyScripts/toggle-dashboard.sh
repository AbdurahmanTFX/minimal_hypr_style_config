#!/bin/bash
source ~/.config/hypr/MyScripts/config.sh

# Hyprland 0.55+: "hyprctl dispatch <cmd> <args>" не работает под hyprland.lua.
_dispatch() {
    hyprctl dispatch "$1"
}

visible=$(hyprctl clients -j 2>/dev/null | python3 -c "
import sys,json
for c in json.load(sys.stdin):
    if c.get('class') == 'dashboard':
        ws = c.get('workspace',{}).get('name','')
        print('visible' if 'special' not in ws else 'hidden')
        exit()
print('missing')
")

# Открываем только если нет другой панели; закрывать всегда можно
if [ "$visible" != "visible" ] && other_panel_open "dashboard"; then
    exit 0
fi

case $visible in
    visible)
        _dispatch 'hl.dsp.window.move({ workspace = "special:dashboard", window = "class:dashboard", follow = false })'
        _dispatch 'hl.dsp.submap("reset")'
        ;;
    hidden)
        _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "class:dashboard", follow = false })'
        _dispatch 'hl.dsp.focus({ window = "class:dashboard" })'
        _dispatch 'hl.dsp.submap("dashboard")'
        ;;
    missing)
        kitty --class dashboard \
              --config ~/.config/kitty/dashboard.conf \
              --override "remember_window_size=no" \
              --override "initial_window_width=1366" \
              --override "initial_window_height=768" \
              --detach \
              ~/.config/hypr/dashboard
        # window_rule "dashboard-hide" уводит свежесозданное окно класса
        # dashboard на special:dashboard сразу при появлении (как и для
        # ws-overview/profile-overview) — без явного move+focus здесь
        # первый запуск оставлял бы пустой экран внутри сабмапа "dashboard".
        for i in $(seq 1 20); do
            sleep 0.1
            hyprctl clients -j 2>/dev/null | python3 -c "
import sys,json
exit(0 if any(c.get('class')=='dashboard' for c in json.load(sys.stdin)) else 1)
" 2>/dev/null && break
        done
        _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "class:dashboard", follow = false })'
        _dispatch 'hl.dsp.focus({ window = "class:dashboard" })'
        _dispatch 'hl.dsp.submap("dashboard")'
        ;;
esac
