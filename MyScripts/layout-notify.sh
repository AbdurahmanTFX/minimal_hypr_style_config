#!/bin/bash
# Уведомление о смене раскладки. Hyprland сообщает о ней только событием
# activelayout в свой сокет, а само событие приходит и без смены (например, при
# перезагрузке конфига) и от других устройств: у мыши и наушников раскладка своя.
# Поэтому помним раскладку каждой клавиатуры и показываем только настоящую смену.
# Звука нет: правило own_layout в dunstrc помечает это уведомление как transient.

exec 9>"${XDG_RUNTIME_DIR:-/tmp}/layout-notify.lock"
flock -n 9 || exit 0

declare -A last
while IFS=$'\t' read -r name layout; do
    last[$name]=$layout
done < <(hyprctl devices -j | jq -r '.keyboards[] | "\(.name)\t\(.active_keymap)"')

socket="$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock"

while IFS= read -r line; do
    [[ $line == activelayout\>\>* ]] || continue
    line=${line#activelayout>>}
    name=${line%%,*}
    layout=${line#*,}

    prev=${last[$name]-}
    last[$name]=$layout
    [ -n "$prev" ] && [ "$prev" != "$layout" ] || continue

    code=$(hyprctl devices -j | jq -r --arg name "$name" \
        '.keyboards[] | select(.name == $name) | (.layout | split(","))[.active_layout_index]')
    summary=${code^^}
    notify-send -a Layout -t 800 -h string:x-dunst-stack-tag:layout-indicator \
        "${summary:-$layout}" "$layout"
# 9>&-: иначе socat держит блокировку и после смерти скрипта (см. battery-guard.sh)
done < <(socat -U - "UNIX-CONNECT:$socket" 9>&-)
