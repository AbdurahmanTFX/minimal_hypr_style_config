#!/bin/bash
# toggle-hidden-apps.sh — панель "скрытые окна профиля" (Win+Alt+V).
# Тот же паттерн toplevel-окна, что и у toggle-overview.sh /
# toggle-profile-overview.sh (см. layer_window.cpp: xdg_toplevel), title
# "hidden-apps-view".
#
# Использование:
#   toggle-hidden-apps.sh                — для активного профиля
#   toggle-hidden-apps.sh <base> <prof>  — для явного профиля (так
#                                          вызывает profile-overview
#                                          по клавише "s" на выбранной
#                                          ячейке, даже если она не
#                                          активна прямо сейчас; в этом
#                                          случае панель запоминает, что
#                                          нужно вернуться в
#                                          profile-overview по q/Esc,
#                                          а не просто закрыться)

source ~/.config/hypr/MyScripts/config.sh
init_storage

_dispatch() {
    hyprctl dispatch "$1"
}

# Явные base/prof передаются ТОЛЬКО из profile-overview (клавиша "s") —
# используем это как признак "вернуться туда по Esc", см.
# HAV_TARGET_FILE (третье поле, читает hidden_apps_view.cpp).
if [ -n "$1" ] && [ -n "$2" ]; then
    BASE="$1"
    PROF="$2"
    RETURN_POV=1
else
    RAW_ID=$(current_ws_id)
    BASE=$(base_ws_of $RAW_ID)
    PROF=$(profile_of $RAW_ID)
    RETURN_POV=0
fi
TARGET_ID=$(hypr_id_of "$BASE" "$PROF")

# Какой (base,prof) показывать и куда вернуться по Esc — читает сам
# hidden-apps-view (и при первом запуске, и при последующих
# переключениях на уже работающем процессе через HAV_RELOAD_FLAG).
echo "$BASE $PROF $RETURN_POV" > "$HAV_TARGET_FILE"

visible=$(panel_state title hidden-apps-view)

# Открываем только если нет другой панели; закрывать всегда можно
if [ "$visible" != "visible" ] && other_panel_open "hidden-apps-view"; then
    exit 0
fi

case $visible in
    visible)
        _dispatch 'hl.dsp.window.move({ workspace = "special:hiddenview", window = "title:hidden-apps-view", follow = false })'
        _dispatch 'hl.dsp.submap("reset")'
        ;;
    hidden)
        touch "$HAV_RELOAD_FLAG" 2>/dev/null
        _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "title:hidden-apps-view", follow = false })'
        _dispatch 'hl.dsp.focus({ window = "title:hidden-apps-view" })'
        _dispatch 'hl.dsp.submap("hidden-apps-view")'
        ;;
    missing)
        _dispatch 'hl.dsp.window.close({ window = "title:hidden-apps-view" })' 2>/dev/null
        ~/.config/hypr/hidden-apps-view "$TARGET_ID" &
        found=0
        for i in $(seq 1 40); do
            sleep 0.05
            if [ "$(panel_state title hidden-apps-view)" != missing ]; then
                found=1
                break
            fi
        done
        if [ "$found" -eq 1 ]; then
            _dispatch 'hl.dsp.window.move({ workspace = "e+0", window = "title:hidden-apps-view", follow = false })'
            _dispatch 'hl.dsp.focus({ window = "title:hidden-apps-view" })'
            _dispatch 'hl.dsp.submap("hidden-apps-view")'
        fi
        ;;
esac
