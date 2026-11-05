#!/bin/bash
# ═══════════════════════════════════════════════════════════
# Центральная конфигурация скриптов Hyprland
# Изменяй здесь — все скрипты подхватят автоматически
# ═══════════════════════════════════════════════════════════

# ── Раскладка рабочих столов ─────────────────────────────
WS_COLS=3           # колонок
WS_ROWS=2           # строк
WS_COUNT=6          # всего рабочих столов (WS_COLS * WS_ROWS)
PROFILE_COUNT=9     # профилей на рабочий стол

# Формула ID воркспейса:
#   hypr_id = base_ws + (profile - 1) * WS_COUNT
#   base_ws = ((hypr_id - 1) % WS_COUNT) + 1
#   profile  = ((hypr_id - 1) / WS_COUNT) + 1

# ── Пути хранения ────────────────────────────────────────
PROFILE_STORAGE="/tmp/hypr-profiles"   # текущий профиль каждого ws
ALTAB_STORAGE="/tmp/hypr-altab"        # alt-tab партнёр каждого ws
WS_CACHE_DIR="/tmp/ws-cache"           # кэш скриншотов
NOTES_DIR="$HOME/Documents/Notes/dashboard"
DASHBOARD_NOTES="$HOME/.config/hypr/dashboard_notes.dat"
OV_RELOAD_FLAG="/tmp/ws-overview-reload"
POV_RELOAD_FLAG="/tmp/profile-overview-reload"

# Скрытые окна (Win+Alt+C), список свой у каждого (base_ws, профиль).
# Файл на пару: /tmp/hypr-hidden-apps/ws_<base>_p<prof>, строки вида
# "addr\x1fclass\x1ftitle", порядок — от старых к новым (хвост файла =
# последнее скрытое, см. hidden_pop_last).
HIDDEN_APPS_DIR="/tmp/hypr-hidden-apps"
HAV_RELOAD_FLAG="/tmp/hidden-apps-view-reload"
HAV_TARGET_FILE="/tmp/hidden-apps-view.target"   # какой (base,prof) сейчас
                                                  # должна показывать
                                                  # открытая hidden-apps-view

# ── Программы ────────────────────────────────────────────
TERMINAL="kitty"
EDITOR="nvim"
FILE_MANAGER="yazi"

# ── Задержки анимации (секунды) ──────────────────────────
ANIM_DELAY="0.35"     # ожидание после смены ws перед скриншотом

# ── Батарея (battery-guard.sh) ───────────────────────────
BAT_DIR="/sys/class/power_supply/BAT0"
BAT_WARN_PERCENT=35      # заряд, при котором срабатывает предупреждение
BAT_WARN_BRIGHTNESS=40   # до какой яркости снизить (линейные %, как в dashboard)
BAT_REARM_PERCENT=40     # заряд, выше которого предупреждение снова взводится
BAT_POLL_SEC=30          # как часто проверять заряд
BAT_WARNED_FLAG="$HOME/.cache/hypr-battery-warned"   # есть файл = уже сработало

# ── Звук уведомлений (notif-sound.sh) ────────────────────
NOTIF_SOUND_FILE="$HOME/.config/hypr/sounds/notification.mp3"
NOTIF_SOUND_STATE="$HOME/.config/hypr/notif_sound.dat"   # пишет dashboard

# ── Метки профилей ──────────────────────────────────────
PROFILE_LABELS=("" "P1" "P2" "P3" "P4" "P5" "P6" "P7" "P8" "P9")

# ── Утилиты ─────────────────────────────────────────────

# Текущий Hyprland WS ID
current_ws_id() {
    hyprctl activeworkspace -j 2>/dev/null \
        | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',1))"
}

# base_ws (1-6) из hypr id
base_ws_of() {
    echo $(( ($1 - 1) % WS_COUNT + 1 ))
}

# номер профиля (1-9) из hypr id
profile_of() {
    echo $(( ($1 - 1) / WS_COUNT + 1 ))
}

# hypr id из base_ws и profile
hypr_id_of() {
    echo $(( $1 + ($2 - 1) * WS_COUNT ))
}

# Сохранённый профиль для base_ws (или 1 если нет)
saved_profile() {
    local ws=$1
    local val
    val=$(grep "^ws_${ws}=" "$PROFILE_STORAGE" 2>/dev/null | cut -d= -f2)
    echo "${val:-1}"
}

# Сохранить профиль для base_ws
save_profile() {
    local ws=$1 prof=$2
    touch "$PROFILE_STORAGE"
    sed -i "/^ws_${ws}=/d" "$PROFILE_STORAGE"
    echo "ws_${ws}=${prof}" >> "$PROFILE_STORAGE"
}

# Alt-tab партнёр для base_ws (или 2 если нет)
saved_altab() {
    local ws=$1
    local val
    val=$(grep "^ws_${ws}=" "$ALTAB_STORAGE" 2>/dev/null | cut -d= -f2)
    echo "${val:-2}"
}

# Сохранить alt-tab партнёра
save_altab() {
    local ws=$1 prof=$2
    touch "$ALTAB_STORAGE"
    sed -i "/^ws_${ws}=/d" "$ALTAB_STORAGE"
    echo "ws_${ws}=${prof}" >> "$ALTAB_STORAGE"
}

# ── Скрытые окна (Win+Alt+C) — хранилище по профилям ─────

# Путь к файлу-бакету для (base_ws, профиль)
hidden_file() {
    echo "$HIDDEN_APPS_DIR/ws_${1}_p${2}"
}

# Добавить окно в конец списка скрытых для (base, prof)
hidden_push() {
    local base=$1 prof=$2 addr=$3 class=$4 title=$5
    local f
    f=$(hidden_file "$base" "$prof")
    mkdir -p "$HIDDEN_APPS_DIR"
    printf '%s\x1f%s\x1f%s\n' "$addr" "$class" "$title" >> "$f"
}

# Количество скрытых окон в (base, prof)
hidden_count() {
    local f
    f=$(hidden_file "$1" "$2")
    [ -s "$f" ] && wc -l < "$f" || echo 0
}

# Вернуть (и удалить) последнюю строку — самое недавно скрытое окно.
# Печатает строку целиком ("addr\x1fclass\x1ftitle") или ничего, если пусто.
hidden_pop_last() {
    local f
    f=$(hidden_file "$1" "$2")
    [ -s "$f" ] || return 0
    tail -n 1 "$f"
    sed -i '$d' "$f"
}

# Удалить конкретное окно (по адресу) из списка (base, prof) — не
# обязательно последнее; используется из hidden-apps-view при выборе
# произвольной строки.
hidden_remove_addr() {
    local base=$1 prof=$2 addr=$3
    local f
    f=$(hidden_file "$base" "$prof")
    [ -f "$f" ] || return 0
    grep -v "^${addr}"$'\x1f' "$f" > "${f}.tmp" 2>/dev/null
    mv "${f}.tmp" "$f"
}

# Возвращает 0 если какая-то другая эксклюзивная панель уже видна,
# ИЛИ если сейчас активен show-desktop (окна запаркованы в
# special:showdesktop) — в обоих случаях панелям (dashboard/
# ws-overview/profile-overview/hidden-apps-view) нельзя всплывать
# поверх, иначе они путают друг другу фокус и состояние.
# Использование: other_panel_open "имя_своего_класса_или_тайтла"
# (сам show-desktop.sh зовёт с me="showdesktop", чтобы не блокировать
# сам себя проверкой на свой же STATE_FILE)
#
# ws-overview/profile-overview/dashboard/hidden-apps-view — всё
# toplevel-окна (см. layer_window.cpp: xdg_toplevel), так что
# hyprctl clients -j их видит одинаково — по class или title.
other_panel_open() {
    local me="$1"

    if [ -f /tmp/showdesktop.state ] && [ "$me" != "showdesktop" ]; then
        return 0
    fi

    hyprctl clients -j 2>/dev/null | python3 -c "
import sys,json
me='$me'
panel_names = ('dashboard','ws-overview','profile-overview','hidden-apps-view')
for c in json.load(sys.stdin):
    cl=c.get('class',''); ti=c.get('title','')
    ws=c.get('workspace',{}).get('name','')
    if 'special' in ws: continue
    if cl==me or ti==me: continue
    if cl in panel_names or ti in panel_names:
        exit(0)
exit(1)
"
}

# Создать директории хранения
init_storage() {
    touch "$PROFILE_STORAGE" 2>/dev/null || true
    touch "$ALTAB_STORAGE"   2>/dev/null || true
    mkdir -p "$WS_CACHE_DIR" 2>/dev/null || true
    mkdir -p "$HIDDEN_APPS_DIR" 2>/dev/null || true
}

# Асинхронный захват скриншота профиля
# Сохраняет ws_BASE_pPROF.png (для profile-overview)
# и ws_BASE_latest.png (для ws-overview), затем поднимает флаги обновления
capture_ws_async() {
    local base=$1 prof=$2
    local pfile="$WS_CACHE_DIR/ws_${base}_p${prof}.png"
    local lfile="$WS_CACHE_DIR/ws_${base}_latest.png"
    (grim -o eDP-1 "$pfile" 2>/dev/null && \
     ln -f "$pfile" "$lfile" 2>/dev/null; \
     touch "$OV_RELOAD_FLAG"  2>/dev/null; \
     touch "$POV_RELOAD_FLAG" 2>/dev/null) &
}
