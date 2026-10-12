# Print IAT slot addresses for imports of a PE32 file.
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
opt = pe+24; base = struct.unpack_from('<I', d, opt+28)[0]
secs = [struct.unpack_from('<8sIIII', d, opt+optsz+i*40) for i in range(nsec)]
def off(rva):
    for n, vs, va, rs, ro in secs:
        if va <= rva < va+max(vs, rs): return rva-va+ro
imp = struct.unpack_from('<I', d, opt+104)[0]
o = off(imp)
while True:
    ilt, _, _, name, iat = struct.unpack_from('<5I', d, o); o += 20
    if not name: break
    dll = d[off(name):].split(b'\0')[0].decode()
    t = off(ilt or iat); i = 0
    while True:
        e = struct.unpack_from('<I', d, t+i*4)[0]
        if not e: break
        fn = '#%d' % (e & 0xffff) if e & 0x80000000 else d[off(e)+2:].split(b'\0')[0].decode()
        print('%08x %s!%s' % (base+iat+i*4, dll, fn)); i += 1
