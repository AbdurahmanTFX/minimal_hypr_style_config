#!/bin/bash
# show-desktop.sh
# Скрывает все окна текущего рабочего стола в special:showdesktop
# При повторном нажатии — возвращает обратно, сохраняя порядок фокуса
#
# ВАЖНО (фикс конфликта с панелями и со скрытыми через Win+Alt+C):
#   1. show-desktop берёт и возвращает ТОЛЬКО те окна, что были реально
#      на экране в момент нажатия — окна, уже спрятанные в
#      special:hiddenapps через Win+Alt+C, физически не на текущем
#      workspace и в выборку не попадают вообще (фильтр по
#      workspace.id ниже).
#   2. Панели (dashboard/ws-overview/profile-overview/hidden-apps-view)
#      исключаются явно, даже если они сейчас показаны поверх текущего
#      workspace ("e+0") — иначе show-desktop запарковал бы их в
#      special:showdesktop вперемешку с обычными окнами, а собственная
#      toggle-логика панели (которая тоже проверяет "special" в имени
#      воркспейса) перестала бы понимать, видна она или спрятана —
#      отсюда и были и "перекрытие окон", и сбитый фокус.
#   3. show-desktop не запускается, пока открыта любая панель (через
#      other_panel_open), и наоборот — ни одна панель не откроется,
#      пока активен show-desktop (см. other_panel_open в config.sh).

source ~/.config/hypr/MyScripts/config.sh
init_storage

STATE_FILE="/tmp/showdesktop.state"
HIDDEN_FILE="/tmp/showdesktop.hidden"
ACTIVE_FILE="/tmp/showdesktop.active"

# Hyprland 0.55+: "hyprctl dispatch <cmd> <args>" не работает под hyprland.lua.
_dispatch() {
    hyprctl dispatch "$1"
}

# me="showdesktop" — освобождает от проверки собственного STATE_FILE
# (см. config.sh), но всё ещё блокируется, если открыта dashboard/
# ws-overview/profile-overview/hidden-apps-view.
if other_panel_open "showdesktop"; then
    exit 0
fi

current_ws=$(current_ws_id)

if [ -f "$STATE_FILE" ] && [ "$(cat "$STATE_FILE")" = "$current_ws" ]; then
    # Возвращаем окна обратно — в порядке от давно неактивного к недавно
    # активному, чтобы последним восстановленным (и получившим фокус)
    # снова оказалось то самое окно, что было активно до сокрытия.
    if [ -f "$HIDDEN_FILE" ]; then
        while IFS='|' read -r addr ws_id; do
            _dispatch "hl.dsp.window.move({ workspace = \"$ws_id\", window = \"address:$addr\", follow = false })" 2>/dev/null
        done < "$HIDDEN_FILE"
    fi

    # Страховка: явно возвращаем фокус на окно, активное до сокрытия —
    # move() не всегда переключает фокус ровно так, как нужно.
    if [ -f "$ACTIVE_FILE" ]; then
        active_addr=$(cat "$ACTIVE_FILE")
        [ -n "$active_addr" ] && _dispatch "hl.dsp.focus({ window = \"address:$active_addr\" })" 2>/dev/null
    fi

    rm -f "$STATE_FILE" "$HIDDEN_FILE" "$ACTIVE_FILE"
else
    # Запоминаем активное окно — чтобы точно вернуть на него фокус
    hyprctl activewindow -j 2>/dev/null | jq -r '.address // ""' > "$ACTIVE_FILE"

    # Скрываем все окна текущего WS, запоминая порядок по focusHistoryID
    # (от давно активного к недавно активному) — этот же порядок
    # используется при восстановлении. Панели (dashboard/ws-overview/
    # profile-overview/hidden-apps-view) исключаются явно — они не
    # "обычные" окна текущего рабочего стола, даже когда видны поверх
    # него (workspace "e+0" во время показа).
    windows=$(hyprctl clients -j 2>/dev/null | jq -r --argjson ws "$current_ws" '
        def panel: IN("dashboard", "ws-overview", "profile-overview", "hidden-apps-view");
        [ .[] | select(.workspace.id == $ws and (.class | panel | not) and (.title | panel | not)) ]
        | sort_by(-(.focusHistoryID // 0))
        | .[] | "\(.address)|\($ws)"')

    if [ -z "$windows" ]; then
        rm -f "$ACTIVE_FILE"
        exit  # нечего скрывать
    fi

    echo "$windows" > "$HIDDEN_FILE"
    echo "$current_ws" > "$STATE_FILE"

    while IFS='|' read -r addr ws_id; do
        _dispatch "hl.dsp.window.move({ workspace = \"special:showdesktop\", window = \"address:$addr\", follow = false })" 2>/dev/null
    done < "$HIDDEN_FILE"
fi
