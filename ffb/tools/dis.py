"""Print disassembly of a function from speed.asm with symbolized call/jmp/data targets.
usage: python dis.py <addr|name-substring> [maxlines]"""
import re, sys, bisect, os
_HERE = os.path.dirname(__file__)
RE = next((p for p in (os.path.join(_HERE, '..', '..', 'vr', 're'), os.path.join(_HERE, '..', '..', 'nfsmw-vr', 're'))
           if os.path.isdir(p)), os.path.join(_HERE, '..', '..', 'vr', 're'))
syms = {}
for fn in ('funcs13.txt', 'data13.txt'):
    for line in open(os.path.join(RE, fn), encoding='utf-8', errors='replace'):
        p = line.rstrip('\n').split(' ', 1)
        if len(p) == 2:
            try: syms[int(p[0], 16)] = p[1]
            except ValueError: pass
addrs = sorted(syms)
funcs = sorted(int(l.split(' ',1)[0],16) for l in open(os.path.join(RE,'funcs13.txt')) if not 'Catch@' in l)
def name(a):
    return syms.get(a)
arg = sys.argv[1]
if re.fullmatch(r'(0x)?[0-9a-fA-F]{6,8}', arg):
    start = int(arg, 16)
else:
    m = [a for a in addrs if arg in syms[a]]
    for a in m[:20]: print(hex(a), syms[a])
    if len(m) != 1: sys.exit()
    start = m[0]
i = bisect.bisect_right(funcs, start)
end = funcs[i] if i < len(funcs) else start + 0x1000
maxl = int(sys.argv[2]) if len(sys.argv) > 2 else 400
print('==', hex(start), name(start), '-> end', hex(end))
hexre = re.compile(r'0x([0-9a-f]{6,7})\b')
n = 0
with open(os.path.join(RE, 'speed.asm'), encoding='utf-8', errors='replace') as f:
    on = False
    for line in f:
        m = re.match(r'\s+([0-9a-f]+):\t(.*)', line)
        if not m: continue
        a = int(m.group(1), 16)
        if a == start: on = True
        if on and a >= end: break
        if on:
            txt = m.group(2)
            ann = []
            for h in hexre.findall(txt):
                s = name(int(h, 16))
                if s: ann.append(s[:110])
            t2 = re.search(r'\b(call|j\w+)\s+([0-9a-f]{6})\b', txt)
            if t2:
                s = name(int(t2.group(2), 16))
                if s: ann.append(s[:110])
            print(f'{a:x}: {txt}' + ('    ; ' + ' | '.join(ann) if ann else ''))
            n += 1
            if n >= maxl: break
