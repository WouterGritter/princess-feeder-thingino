#!/usr/bin/env python3
"""Decode strace read/write(3) lines of the feeder's ipc <-> MCU (ttyS0) into Tuya MCU frames.
Usage: mcu-decode.py LOG [since HH:MM:SS] ; hides the idle 0xF2 poll (shows F2 only when nonzero)."""
import re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
since = sys.argv[2] if len(sys.argv) > 2 else ''
ev = []
for l in lines:
    m = re.search(r'(\d\d:\d\d:\d\d\.\d+) (read|write)\(3, "(.*)", \d+\)', l)
    if not m:
        continue
    t, op, s = m.groups()
    ev.append((t, op, bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-f]{2})', s))))
frames, rb, rt = [], b'', None
for t, op, b in ev:
    if op == 'write':
        frames.append((t, 'T31>', b)); continue
    if b.startswith(b'\x55\xaa') and rb:
        frames.append((rt, 'MCU>', rb)); rb = b''
    if not rb:
        rt = t
    rb += b
if rb:
    frames.append((rt, 'MCU>', rb))
frames.sort()
for t, d, b in frames:
    if t < since or len(b) < 7:
        continue
    cmd = b[3]
    if cmd == 0:
        continue  # heartbeat
    data = b[6:-1]
    desc = ''
    if cmd in (6, 7) and len(data) >= 4:
        dp, typ, ln = data[0], data[1], int.from_bytes(data[2:4], 'big')
        val = int.from_bytes(data[4:4 + ln], 'big') if ln else None
        if dp == 0xF2 and not val:
            continue
        desc = f'DP {dp} type {typ} = {val}'
    print(t, d, f'cmd {cmd:02x}', desc, b.hex(' '))
