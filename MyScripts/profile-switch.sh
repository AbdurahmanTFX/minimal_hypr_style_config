#!/bin/bash
# profile-switch.sh — управление 9 профилями на рабочем столе
# Использование:
#   profile-switch.sh goto <1-9>        — перейти к профилю N
#   profile-switch.sh altab             — переключить profile1 ↔ alt-tab партнёр
#   profile-switch.sh set-altab <1-9>   — установить alt-tab партнёра (без перехода)
#   profile-switch.sh move-win <1-9>    — переместить окно в профиль N текущего ws

source ~/.config/hypr/MyScripts/config.sh
init_storage

# Hyprland 0.55+: "hyprctl dispatch <cmd> <args>" не работает под hyprland.lua.
_dispatch() {
    hyprctl dispatch "$1"
}

RAW_ID=$(current_ws_id)
BASE=$(base_ws_of $RAW_ID)
CURRENT_PROF=$(profile_of $RAW_ID)

notify_profile() {
    local prof=$1 base=$2
    notify-send -t 800 -h string:x-dunst-stack-tag:profile-indicator \
        "P${prof}" "Рабочий стол ${base}"
}

case "$1" in
    goto)
        TARGET_PROF="${2:-1}"
        if [ "$TARGET_PROF" -lt 1 ] || [ "$TARGET_PROF" -gt "$PROFILE_COUNT" ]; then
            exit 1
        fi
        # If coming from TAB → move TAB marker to the target profile
        # If going TO TAB → swap: TAB marker moves to where we came
        # from, so ACT (=target) and TAB never end up on the same
        # profile. If coming from PRV → move PRV marker to the target.
        TAB=$(saved_altab $BASE)
        PREV_FILE="/tmp/hypr-altab-prev_ws_${BASE}"
        if [ "$CURRENT_PROF" -eq "$TAB" ] && [ "$TARGET_PROF" -ne "$TAB" ]; then
            save_altab $BASE $TARGET_PROF
        elif [ "$TARGET_PROF" -eq "$TAB" ] && [ "$CURRENT_PROF" -ne "$TAB" ]; then
            save_altab $BASE $CURRENT_PROF
        elif [ -f "$PREV_FILE" ]; then
            PRV=$(cat "$PREV_FILE")
            if [ "$CURRENT_PROF" -eq "$PRV" ] && [ "$TARGET_PROF" -ne "$PRV" ]; then
                echo "$TARGET_PROF" > "$PREV_FILE"
            fi
        fi
        TARGET_ID=$(hypr_id_of $BASE $TARGET_PROF)
        save_profile $BASE $TARGET_PROF
        echo "$TARGET_PROF" > "$WS_CACHE_DIR/ws_${BASE}.profile"
        capture_ws $BASE $CURRENT_PROF
        _dispatch "hl.dsp.focus({ workspace = $TARGET_ID })"
        notify_profile $TARGET_PROF $BASE
        ;;

    altab)
        TAB=$(saved_altab $BASE)
        PREV_FILE="/tmp/hypr-altab-prev_ws_${BASE}"
        if [ "$CURRENT_PROF" -eq "$TAB" ]; then
            # Уже на TAB — возвращаемся туда откуда пришли
            TARGET=1
            if [ -f "$PREV_FILE" ]; then
                TARGET=$(cat "$PREV_FILE")
                if [ "$TARGET" -lt 1 ] || [ "$TARGET" -gt "$PROFILE_COUNT" ] || [ "$TARGET" -eq "$TAB" ]; then
                    TARGET=1
                fi
            fi
        else
            # Переходим к TAB, запоминаем текущий как точку возврата
            echo "$CURRENT_PROF" > "$PREV_FILE"
            TARGET=$TAB
            # ACT (=TARGET=TAB) и TAB иначе совпадут — меняем местами:
            # TAB теперь там, откуда мы пришли.
            save_altab $BASE $CURRENT_PROF
        fi
        TARGET_ID=$(hypr_id_of $BASE $TARGET)
        save_profile $BASE $TARGET
        echo "$TARGET" > "$WS_CACHE_DIR/ws_${BASE}.profile"
        capture_ws $BASE $CURRENT_PROF
        _dispatch "hl.dsp.focus({ workspace = $TARGET_ID })"
        notify_profile $TARGET $BASE
        ;;

    set-altab)
        TARGET_PROF="${2:-2}"
        if [ "$TARGET_PROF" -eq "$CURRENT_PROF" ]; then exit 0; fi
        save_altab $BASE $TARGET_PROF
        notify-send -t 800 -h string:x-dunst-stack-tag:profile-indicator \
            "Alt+Tab → P${TARGET_PROF}" "Рабочий стол ${BASE}"
        ;;

    move-win)
        TARGET_PROF="${2:-1}"
        if [ "$TARGET_PROF" -eq "$CURRENT_PROF" ]; then exit 0; fi
        TARGET_ID=$(hypr_id_of $BASE $TARGET_PROF)
        WIN_ADDR=$(hyprctl activewindow -j | jq -r '.address // ""')
        if [ -n "$WIN_ADDR" ]; then
            _dispatch "hl.dsp.window.move({ workspace = $TARGET_ID, window = \"address:$WIN_ADDR\", follow = false })"
            capture_ws $BASE $CURRENT_PROF
            _dispatch "hl.dsp.focus({ workspace = $TARGET_ID })"
            save_profile $BASE $TARGET_PROF
            echo "$TARGET_PROF" > "$WS_CACHE_DIR/ws_${BASE}.profile"
            notify_profile $TARGET_PROF $BASE
        fi
        ;;

    *)
        echo "usage: profile-switch.sh goto|altab|set-altab|move-win [1-9]" >&2
        exit 1
        ;;
esac

# Запросить обновление ws-overview если открыт
touch "$OV_RELOAD_FLAG" 2>/dev/null || true
