#!/bin/bash
# toggle-touchpad.sh
#
# Hyprland 0.55+: под hyprland.lua команда "hyprctl keyword" отключена
# (см. man hyprctl: "This will not work if your config provider is lua"),
# а "hyprctl getoption" для per-device опций (device[...]:enabled) в
# актуальных версиях не работает вообще. Единственный официальный путь
# менять настройки на лету под Lua-конфигом — "hyprctl eval" с прямым
# вызовом hl.device({...}), той же функции, что применяет per-device
# настройки при загрузке конфига. Состояние храним сами в STATE_FILE,
# так как читать его обратно через hyprctl больше нельзя.

DEVICE="synps/2-synaptics-touchpad"
STATE_FILE="/tmp/touchpad-state"

current=$(cat "$STATE_FILE" 2>/dev/null || echo "enabled")

if [ "$current" = "enabled" ]; then
    hyprctl eval "hl.device({ name = \"${DEVICE}\", enabled = false })" >/dev/null 2>&1
    echo "disabled" > "$STATE_FILE"
    notify-send -a "Touchpad" -t 1200 "Touchpad" "Выключен"
else
    hyprctl eval "hl.device({ name = \"${DEVICE}\", enabled = true })" >/dev/null 2>&1
    echo "enabled" > "$STATE_FILE"
    notify-send -a "Touchpad" -t 1200 "Touchpad" "Включён"
fi
