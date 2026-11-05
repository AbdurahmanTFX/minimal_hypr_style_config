#!/bin/bash
source ~/.config/hypr/MyScripts/config.sh

WS=${1:-1}  # base_ws 1-6
mkdir -p "$WS_CACHE_DIR"

# Не снимать если открыт overview
if hyprctl clients -j 2>/dev/null | python3 -c "
import sys,json
for c in json.load(sys.stdin):
    if c.get('title') in ('ws-overview','profile-overview'):
        ws = c.get('workspace',{}).get('name','')
        if 'special' not in ws: exit(0)
exit(1)
" 2>/dev/null; then
    exit 0
fi

sleep "$ANIM_DELAY"

# Текущий воркспейс
RAW=$(current_ws_id)
BASE=$(base_ws_of $RAW)
PROF=$(profile_of $RAW)

# Снимаем только если base_ws не изменился
if [ "$BASE" = "$WS" ]; then
    PFILE="$WS_CACHE_DIR/ws_${WS}_p${PROF}.png"
    grim -o eDP-1 "$PFILE" 2>/dev/null && \
        ln -f "$PFILE" "$WS_CACHE_DIR/ws_${WS}_latest.png" 2>/dev/null
    echo "$PROF" > "$WS_CACHE_DIR/ws_${WS}.profile"
fi
