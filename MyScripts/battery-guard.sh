#!/bin/bash
# Один раз за разряд: когда заряд падает до порога, предупреждает и снижает яркость.
# Больше яркость не трогает, так что её можно сразу вернуть вручную.
# Флаг переживает перезагрузку и снимается, когда батарею снова зарядили.
source ~/.config/hypr/MyScripts/config.sh

[ -r "$BAT_DIR/capacity" ] || exit 0

# Второй экземпляр не нужен (например, после перелогина)
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/battery-guard.lock"
flock -n 9 || exit 0

warn() {
    local cap=$1 before value
    before=$(( $(brightnessctl get) * 100 / $(brightnessctl max) ))
    value="${before}%"

    # Только вниз: если уже темнее порога, яркость остаётся как есть.
    # Без -e4: проценты линейные, как их показывает dashboard.
    if (( before > BAT_WARN_BRIGHTNESS )); then
        brightnessctl -q set "${BAT_WARN_BRIGHTNESS}%"
        value="${before}% → ${BAT_WARN_BRIGHTNESS}%"
    fi

    notify-send -u critical -a battery-guard "Низкий заряд: ${cap}%" \
        "<span foreground='#8ea4a2'>brightness = </span>${value}"
}

while :; do
    cap=$(<"$BAT_DIR/capacity")
    status=$(<"$BAT_DIR/status")

    if (( cap >= BAT_REARM_PERCENT )); then
        rm -f "$BAT_WARNED_FLAG"
    elif [ "$status" = "Discharging" ] && (( cap <= BAT_WARN_PERCENT )) \
            && [ ! -e "$BAT_WARNED_FLAG" ]; then
        touch "$BAT_WARNED_FLAG"
        warn "$cap"
    fi

    # 9>&-: иначе sleep держит блокировку и после смерти скрипта,
    # и быстрый перезапуск молча выходит на flock
    sleep "$BAT_POLL_SEC" 9>&-
done
