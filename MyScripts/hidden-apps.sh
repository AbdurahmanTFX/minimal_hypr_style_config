#!/bin/bash
# hidden-apps.sh — список окон, скрытых по Win+Alt+C, СВОЙ У КАЖДОГО
# ПРОФИЛЯ (base_ws + профиль). Раньше Win+Alt+C кидал окно в один общий
# special:hidden на все профили сразу — теперь у каждого (workspace,
# профиль) отдельный список, см. HIDDEN_APPS_DIR в config.sh.
#
# Использование:
#   hidden-apps.sh hide
#       — спрятать активное окно в список ТЕКУЩЕГО профиля
#   hidden-apps.sh restore-last
#       — вернуть последнее спрятанное окно текущего профиля
#         (Win+Alt+S)
#   hidden-apps.sh restore-addr <addr> [base] [prof]
#       — вернуть конкретное окно по адресу из списка (base,prof)
#         (используется из hidden-apps-view; base/prof по умолчанию —
#         текущий профиль)

source ~/.config/hypr/MyScripts/config.sh
init_storage

_dispatch() {
    hyprctl dispatch "$1"
}

# Панели никогда не должны сами попадать в список скрытых — они и так
# управляются toggle-*.sh/special-воркспейсами, отдельным списком их
# трогать нельзя (см. комментарий в config.sh:other_panel_open).
is_panel() {
    case "$1" in dashboard|ws-overview|profile-overview|hidden-apps-view) return 0 ;; esac
    case "$2" in dashboard|ws-overview|profile-overview|hidden-apps-view) return 0 ;; esac
    return 1
}

RAW_ID=$(current_ws_id)
BASE=$(base_ws_of $RAW_ID)
PROF=$(profile_of $RAW_ID)

case "$1" in
    hide)
        # Пока на этом рабочем столе активен show-desktop (окна
        # запаркованы в special:showdesktop), "активное окно" может
        # оказаться совсем не тем, что видно на экране, а окном с
        # другого профиля/workspace, случайно оставшимся в фокусе.
        # Скрывать его в список ЭТОГО профиля было бы багом — вместо
        # этого просим сначала вернуть рабочий стол.
        if [ -f /tmp/showdesktop.state ] && [ "$(cat /tmp/showdesktop.state 2>/dev/null)" = "$RAW_ID" ]; then
            notify-send -t 900 -h string:x-dunst-stack-tag:hidden-indicator \
                "Сначала верните рабочий стол" "Ctrl+Alt+D"
            exit 0
        fi

        WIN_JSON=$(hyprctl activewindow -j 2>/dev/null)
        [ -z "$WIN_JSON" ] && exit 0
        [ "$WIN_JSON" = "null" ] && exit 0

        read -r ADDR WIN_WS <<<"$(echo "$WIN_JSON" | jq -r '"\(.address // "") \(.workspace.id // 0)"')"
        [ -z "$ADDR" ] && exit 0
        # Активное окно должно физически быть на текущем workspace —
        # иначе (см. комментарий выше про show-desktop) мы бы скрыли
        # чужое окно под ЭТИМ профилем.
        [ "$WIN_WS" != "$RAW_ID" ] && exit 0

        CLASS=$(echo "$WIN_JSON" | jq -r '.class // ""')
        TITLE=$(echo "$WIN_JSON" | jq -r '.title // ""')

        if is_panel "$CLASS" "$TITLE"; then
            exit 0
        fi

        hidden_push "$BASE" "$PROF" "$ADDR" "$CLASS" "$TITLE"
        _dispatch "hl.dsp.window.move({ workspace = \"special:hiddenapps\", window = \"address:$ADDR\", follow = false })"
        notify-send -t 700 -h string:x-dunst-stack-tag:hidden-indicator \
            "Скрыто (P${PROF})" "${TITLE:-$CLASS}"
        touch "$HAV_RELOAD_FLAG" 2>/dev/null
        ;;

    restore-last)
        LINE=$(hidden_pop_last "$BASE" "$PROF")
        if [ -z "$LINE" ]; then
            notify-send -t 600 -h string:x-dunst-stack-tag:hidden-indicator \
                "Скрытых окон нет" "Рабочий стол ${BASE} · P${PROF}"
            exit 0
        fi
        ADDR=$(printf '%s' "$LINE" | cut -d$'\x1f' -f1)
        TITLE=$(printf '%s' "$LINE" | cut -d$'\x1f' -f3)
        _dispatch "hl.dsp.window.move({ workspace = $RAW_ID, window = \"address:$ADDR\", follow = false })"
        _dispatch "hl.dsp.focus({ window = \"address:$ADDR\" })"
        notify-send -t 700 -h string:x-dunst-stack-tag:hidden-indicator \
            "Возвращено" "${TITLE:-$ADDR}"
        touch "$HAV_RELOAD_FLAG" 2>/dev/null
        ;;

    restore-addr)
        ADDR="$2"
        TB="${3:-$BASE}"
        TP="${4:-$PROF}"
        [ -z "$ADDR" ] && exit 1

        hidden_remove_addr "$TB" "$TP" "$ADDR"
        TARGET_ID=$(hypr_id_of "$TB" "$TP")
        _dispatch "hl.dsp.window.move({ workspace = $TARGET_ID, window = \"address:$ADDR\", follow = false })"

        # Фокус забираем только если восстанавливаем в текущий активный
        # профиль — иначе (например, вызов из hidden-apps-view для
        # ЧУЖОГО профиля через клавишу "s" в profile-overview) мы бы
        # выдернули пользователя туда без спроса.
        if [ "$TB" -eq "$BASE" ] && [ "$TP" -eq "$PROF" ]; then
            _dispatch "hl.dsp.focus({ window = \"address:$ADDR\" })"
        fi
        touch "$HAV_RELOAD_FLAG" 2>/dev/null
        ;;

    *)
        echo "usage: hidden-apps.sh hide|restore-last|restore-addr <addr> [base] [prof]" >&2
        exit 1
        ;;
esac
