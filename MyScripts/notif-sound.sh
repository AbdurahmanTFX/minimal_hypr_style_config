#!/bin/bash
# Звук нового уведомления. Запускается правилом notif_sound из dunstrc,
# а ещё dashboard при смене громкости, чтобы сразу было слышно результат.
source ~/.config/hypr/MyScripts/config.sh

volume=50
muted=0
[ -r "$NOTIF_SOUND_STATE" ] && source "$NOTIF_SOUND_STATE"

(( muted )) && exit 0
# Низкий приоритет по смыслу не должен отвлекать звуком
[ "$DUNST_URGENCY" = "LOW" ] && exit 0

# Пока звук играет, новые молчат: пачка сообщений не должна звучать хором
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/notif-sound.lock"
flock -n 9 || exit 0

# paplay считает громкость в той же шкале процентов, что и строки AUDIO.
# Имя клиента notif-sound прячет этот короткий поток из списка (audio-parse.py).
# media.role=event не ставить: такие потоки наследуют сохранённое системой
# состояние этой роли, и звук может оказаться заглушённым.
paplay --client-name=notif-sound --volume=$(( volume * 65536 / 100 )) "$NOTIF_SOUND_FILE"
