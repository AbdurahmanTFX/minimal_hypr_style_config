# ============== wallpaper-pick.sh ==============
#!/bin/bash

STATE_FILE="/tmp/wallpaper-mode"
STATIC_DIR="$HOME/Pictures/wallpapers/dashboard"
LIVE_DIR="$HOME/Videos/wallpapers"
PICKER_CLASS="wallpaper-picker"

# ── Toggle: если окно пикера уже открыто — закрыть и выйти ────
picker_is_open() {
    hyprctl clients -j 2>/dev/null \
        | jq -e --arg cls "$PICKER_CLASS" '.[] | select(.class == $cls)' >/dev/null 2>&1
}

close_picker() {
    hyprctl dispatch "hl.dsp.window.close({ window = \"class:^(${PICKER_CLASS})\$\" })" >/dev/null 2>&1
}

# ── Зависимости ──────────────────────────────────────────────
check_deps() {
    local missing=()
    for cmd in awww mpvpaper hyprctl jq kitty yazi; do
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

# ── Применить обои ────────────────────────────────────────────
apply_static() {
    local file="$1"
    awww img "$file" --transition-type fade --transition-duration 2
    echo "static" > "$STATE_FILE"
    notify-send -a "Wallpaper" -t 1500 "Static" "$(basename "$file")"
}

apply_live() {
    local file="$1"
    pkill mpvpaper 2>/dev/null
    sleep 0.2
    mpvpaper -o "no-audio loop --hwdec=auto" "$MONITOR" "$file" &
    echo "live" > "$STATE_FILE"
    notify-send -a "Wallpaper" -t 1500 "Live" "$(basename "$file")"
}

# ── Точка входа ───────────────────────────────────────────────
if picker_is_open; then
    close_picker
    exit 0
fi

check_deps
MONITOR=$(get_monitor)

current_mode=$(cat "$STATE_FILE" 2>/dev/null || echo "static")
[[ "$current_mode" == "static" ]] && target_dir="$STATIC_DIR" || target_dir="$LIVE_DIR"

tmp=$(mktemp /tmp/yazi-pick-XXXXXX)

kitty --class wallpaper-picker \
      --override "remember_window_size=no" \
      --override "initial_window_width=1200" \
      --override "initial_window_height=700" \
      yazi "$target_dir" --chooser-file "$tmp"

selected=$(cat "$tmp" 2>/dev/null)
rm -f "$tmp"
[[ -z "$selected" ]] && exit 0

[[ "$current_mode" == "static" ]] && apply_static "$selected" || apply_live "$selected"
