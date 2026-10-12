# List named callees of a function: calls.py START_HEX SIZE_HEX  (reads re/speed.asm + re/symbols13.json)
import json, re, sys
start, size = int(sys.argv[1], 16), int(sys.argv[2], 16)
d = json.load(open('symbols13.json'))
names = {int(f['start'], 16): f['name'] for f in d['functions']}
seen = []
for line in open('speed.asm'):
    m = re.match(r'\s+([0-9a-f]+):\s+call\s+(0x[0-9a-f]+|DWORD PTR .*)$', line.rstrip())
    if not m: continue
    a = int(m.group(1), 16)
    if start <= a < start + size:
        t = m.group(2)
        tgt = names.get(int(t, 16), t) if t.startswith('0x') else t
        print('%x -> %s %s' % (a, t, tgt if tgt != t else ''))
