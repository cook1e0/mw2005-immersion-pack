# Read values from speed.exe at virtual addresses: peread.py exe f|d|i|s ADDR [count]
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
opt = pe+24; base = struct.unpack_from('<I', d, opt+28)[0]
secs = [struct.unpack_from('<8sIIII', d, opt+optsz+i*40) for i in range(nsec)]
def off(va):
    rva = va-base
    for n, vs, va_, rs, ro in secs:
        if va_ <= rva < va_+rs: return rva-va_+ro
    raise ValueError('unmapped/bss %x' % va)
kind, a = sys.argv[2], int(sys.argv[3], 16); n = int(sys.argv[4]) if len(sys.argv) > 4 else 1
fmt = {'f': '<f', 'd': '<d', 'i': '<I'}.get(kind)
for i in range(n):
    if kind == 's': print(d[off(a):].split(b'\0')[0]); break
    sz = struct.calcsize(fmt); print('%08x' % (a+i*sz), struct.unpack_from(fmt, d, off(a+i*sz))[0])
