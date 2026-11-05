# ============== wallpaper-switch.sh ==============
#!/bin/bash

STATE_FILE="/tmp/wallpaper-mode"
STATIC_DIR="$HOME/Pictures/wallpapers/dashboard"
LIVE_DIR="$HOME/Videos/wallpapers"

# ── Зависимости ──────────────────────────────────────────────
check_deps() {
    local missing=()
    for cmd in awww awww-daemon mpvpaper hyprctl jq; do
        command -v "$cmd" &>/dev/null || missing+=("$cmd")
    done
    (( ${#missing[@]} )) && notify-send "Wallpaper" "Не найдены: ${missing[*]}" && exit 1
}

# ── Монитор ───────────────────────────────────────────────────
get_monitor() {
    local mon
    mon=$(hyprctl monitors -j 2>/dev/null \
        | jq -r '.[] | select(.focused == true) | .name' \
        | head -1)
    echo "${mon:-eDP-1}"
}

# ── Демон ─────────────────────────────────────────────────────
ensure_daemon() {
    pgrep -x awww-daemon &>/dev/null && return
    awww-daemon &
    local i=0
    until awww query &>/dev/null || (( i++ >= 10 )); do sleep 0.2; done
}

# ── Случайный файл ────────────────────────────────────────────
random_file() {
    local dir="$1"; shift
    local args=()
    for p in "$@"; do args+=(-iname "$p" -o); done
    unset 'args[-1]'

    local file
    file=$(find "$dir" -type f \( "${args[@]}" \) 2>/dev/null | shuf -n 1)
    [[ -z "$file" ]] && notify-send "Wallpaper" "Нет файлов в $dir" && exit 1
    echo "$file"
}

# ── Режимы ────────────────────────────────────────────────────
set_static() {
    pkill mpvpaper 2>/dev/null
    local img
    img=$(random_file "$STATIC_DIR" "*.jpg" "*.jpeg" "*.png" "*.webp")
    awww img "$img" --transition-type fade --transition-duration 2
    echo "static" > "$STATE_FILE"
    notify-send -a "Wallpaper" -t 1500 "Static" "$(basename "$img")"
}

set_live() {
    pkill mpvpaper 2>/dev/null
    awww clear
    local video
    video=$(random_file "$LIVE_DIR" "*.mp4" "*.webm" "*.gif")
    mpvpaper -o "no-audio loop --hwdec=auto" "$MONITOR" "$video" &
    echo "live" > "$STATE_FILE"
    notify-send -a "Wallpaper" -t 1500 "Live" "$(basename "$video")"
}

# ── Точка входа ───────────────────────────────────────────────
check_deps
MONITOR=$(get_monitor)
ensure_daemon
current_mode=$(cat "$STATE_FILE" 2>/dev/null || echo "static")

case "$1" in
    static) set_static ;;
    live)   set_live ;;
    toggle) [[ "$current_mode" == "static" ]] && set_live  || set_static ;;
    next)   [[ "$current_mode" == "static" ]] && set_static || set_live ;;
    *)
        echo "Использование: $(basename "$0") {static|live|toggle|next}"
        exit 1 ;;
esac
