"""Dump a vtable from speed.exe with symbol names: vtbl.py ADDR [count]"""
import struct, sys, os
_HERE = os.path.dirname(__file__)
RE = next((p for p in (os.path.join(_HERE, '..', '..', 'vr', 're'), os.path.join(_HERE, '..', '..', 'nfsmw-vr', 're'))
           if os.path.isdir(p)), os.path.join(_HERE, '..', '..', 'vr', 're'))
syms = {}
for fn in ('funcs13.txt','data13.txt'):
    for l in open(os.path.join(RE, fn), encoding='utf-8', errors='replace'):
        p = l.rstrip('\n').split(' ',1)
        if len(p)==2:
            try: syms[int(p[0],16)] = p[1]
            except ValueError: pass
d = open(r'C:\Games\NFSMW\speed.exe','rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
opt = pe+24; base = struct.unpack_from('<I', d, opt+28)[0]
secs = [struct.unpack_from('<8sIIII', d, opt+optsz+i*40) for i in range(nsec)]
def rd(va):
    rva = va-base
    for n, vs, va_, rs, ro in secs:
        if va_ <= rva < va_+rs: return struct.unpack_from('<I', d, rva-va_+ro)[0]
a = int(sys.argv[1],16); n = int(sys.argv[2]) if len(sys.argv)>2 else 40
for i in range(n):
    v = rd(a+i*4)
    print('+0x%02x %08x %s' % (i*4, v, syms.get(v, '')[:120]))
