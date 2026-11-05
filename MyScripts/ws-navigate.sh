#!/bin/bash
# ws-navigate.sh — навигация по воркспейсам с 9-профильной системой

source ~/.config/hypr/MyScripts/config.sh
init_storage

# Hyprland 0.55+: "hyprctl dispatch <cmd> <args>" не работает под hyprland.lua.
_dispatch() {
    hyprctl dispatch "$1"
}

ACTION=$1

RAW_ID=$(current_ws_id)
BASE=$(base_ws_of $RAW_ID)
COL=$(( (BASE - 1) % WS_COLS ))
ROW=$(( (BASE - 1) / WS_COLS ))

save_cache_profile() {
    local ws=$1 prof=$2
    mkdir -p "$WS_CACHE_DIR"
    echo "$prof" > "$WS_CACHE_DIR/ws_${ws}.profile"
}

goto_ws() {
    local target_base=$1
    # Восстанавливаем сохранённый профиль целевого ws
    local target_prof
    target_prof=$(saved_profile $target_base)
    [ "$target_prof" -lt 1 ] || [ "$target_prof" -gt "$PROFILE_COUNT" ] && target_prof=1
    # Сохранить текущий профиль перед уходом
    local cur_prof
    cur_prof=$(profile_of $RAW_ID)
    save_profile $BASE $cur_prof
    save_cache_profile $BASE $cur_prof
    capture_ws $BASE $cur_prof
    # Перейти
    local target_id
    target_id=$(hypr_id_of $target_base $target_prof)
    _dispatch "hl.dsp.focus({ workspace = $target_id })"
    touch "$OV_RELOAD_FLAG" 2>/dev/null || true
}

edge_notify() {
    notify-send -t 400 -h string:x-dunst-stack-tag:ws-indicator "Край" "$1"
}

move_win_to_ws() {
    local target_base=$1
    local cur_prof
    cur_prof=$(profile_of $RAW_ID)
    save_profile $BASE $cur_prof
    save_cache_profile $BASE $cur_prof
    local target_prof
    target_prof=$(saved_profile $target_base)
    [ "$target_prof" -lt 1 ] || [ "$target_prof" -gt "$PROFILE_COUNT" ] && target_prof=1
    local target_id
    target_id=$(hypr_id_of $target_base $target_prof)
    _dispatch "hl.dsp.window.move({ workspace = $target_id })"
    touch "$OV_RELOAD_FLAG" 2>/dev/null || true
}

case "$ACTION" in
    left)
        [ $COL -gt 0 ] && goto_ws $((BASE - 1)) || edge_notify "◀ некуда" ;;
    right)
        [ $COL -lt $((WS_COLS - 1)) ] && goto_ws $((BASE + 1)) || edge_notify "▶ некуда" ;;
    up)
        [ $ROW -gt 0 ] && goto_ws $((BASE - WS_COLS)) || edge_notify "▲ некуда" ;;
    down)
        [ $ROW -lt $((WS_ROWS - 1)) ] && goto_ws $((BASE + WS_COLS)) || edge_notify "▼ некуда" ;;

    move-left)
        [ $COL -gt 0 ] && move_win_to_ws $((BASE - 1)) ;;
    move-right)
        [ $COL -lt $((WS_COLS - 1)) ] && move_win_to_ws $((BASE + 1)) ;;
    move-up)
        [ $ROW -gt 0 ] && move_win_to_ws $((BASE - WS_COLS)) ;;
    move-down)
        [ $ROW -lt $((WS_ROWS - 1)) ] && move_win_to_ws $((BASE + WS_COLS)) ;;

    goto)
        # goto <1-6> — перейти к конкретному рабочему столу
        TARGET_BASE="${2:-1}"
        if [ "$TARGET_BASE" -ge 1 ] && [ "$TARGET_BASE" -le "$WS_COUNT" ]; then
            goto_ws "$TARGET_BASE"
        fi
        ;;

    *)
        echo "usage: ws-navigate.sh left|right|up|down|move-left|move-right|move-up|move-down|goto <1-6>" >&2
        exit 1 ;;
esac
