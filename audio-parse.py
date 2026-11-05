#!/usr/bin/env python3
import subprocess, re, json

try:
    data = subprocess.check_output(['pactl', 'list', 'sink-inputs'],
                                   stderr=subprocess.DEVNULL).decode()
except Exception:
    print('[]')
    raise SystemExit

blocks = re.split(r'Sink Input #', data)[1:]
res = []
for b in blocks:
    sid  = b.split('\n')[0].strip()
    nm   = re.search(r'application\.name = "(.+?)"', b)
    # Короткий звук уведомления (notif-sound.sh): у него своя постоянная строка в AUDIO
    if nm and nm.group(1) == 'notif-sound':
        continue
    pb   = re.search(r'application\.process\.binary = "(.+?)"', b)
    vm   = re.search(r'Volume:.*?(\d+)%', b)
    name = (nm or pb).group(1)[:22] if (nm or pb) else 'unknown'
    vol  = int(vm.group(1)) if vm else 100
    muted = 'yes' in b[b.find('Mute:'):b.find('Mute:')+10] if 'Mute:' in b else False
    res.append({'id': sid, 'name': name, 'vol': vol, 'muted': muted})

print(json.dumps(res))
