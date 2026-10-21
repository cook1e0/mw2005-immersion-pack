"""Builds scripts\\nfsmw_hud_icons.bin: a small texture pack with the HUD icons the gameplay mod
shows in races (the ABS and TC icons above the minimap), copied from the game's own front end
pack (FRONTEND\\FrontB.lzc, Global\\FrontEndTextures.tpk), which the game unloads in races.

The pack is laid out like GLOBAL\\HUDTEXDRAG.BIN, which the game loads in races: a TPK chunk
(B3300000) holding the header (33310001: version 5, pipeline, name, the name's bStringHash at
+0x60), the texture hashes (33310002, 8 bytes each, sorted), the texture infos (33310004, 124
bytes each: name +0x0C, hash +0x24, data offset +0x30, size +0x38 / +0x40, width / height +0x44)
and their formats (33310005, 32 bytes each), then the data chunk (B3320000: 33320001 with the
pack's hash, and 33320002 whose texture data starts 0x78 bytes in, at a 0x80 boundary). Padding
chunks (id 0) keep both at 0x80 boundaries, as in the game's packs.

usage: python make_icon_pack.py <game folder>
The game's files are only read; the output is written to <game folder>\\scripts\\nfsmw_hud_icons.bin.
"""
import os
import struct
import sys

ICONS = [b'PERFORMANCE_BRAKES', b'MAIN_ICON_QUICK_RACE']
PACK_NAME = 'Global\\NfsmwModHudIcons.tpk'


def bhash(s):
    h = 0xFFFFFFFF
    for c in s.encode() if isinstance(s, str) else s:
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def chunk(cid, body):
    return struct.pack('<II', cid, len(body)) + body


def find_chunk(d, start, end, cid):
    off = start
    while off + 8 <= end:
        c, size = struct.unpack_from('<II', d, off)
        if c == cid:
            return off, size
        off += 8 + size
    return None


def main():
    game = sys.argv[1]
    src = open(os.path.join(game, 'FRONTEND', 'FrontB.lzc'), 'rb').read()
    # The front end texture pack: the TPK chunk whose header names FrontEndTextures.tpk.
    tpk = None
    off = 0
    while off + 8 <= len(src):
        c, size = struct.unpack_from('<II', src, off)
        if c == 0xB3300000 and b'FrontEndTextures.tpk' in src[off:off + 512]:
            tpk = (off, size)
            break
        off += 8 + size
    if not tpk:
        sys.exit('FrontEndTextures.tpk not found in FrontB.lzc')
    t0, tsize = tpk
    hdr_parent = find_chunk(src, t0 + 8, t0 + 8 + tsize, 0xB3310000)
    data_parent = find_chunk(src, t0 + 8, t0 + 8 + tsize, 0xB3320000)
    hp, hsize = hdr_parent
    infos = find_chunk(src, hp + 8, hp + 8 + hsize, 0x33310004)
    fmts = find_chunk(src, hp + 8, hp + 8 + hsize, 0x33310005)
    head = find_chunk(src, hp + 8, hp + 8 + hsize, 0x33310001)
    dp, dsize = data_parent
    data = find_chunk(src, dp + 8, dp + 8 + dsize, 0x33320002)
    data_start = data[0] + 8 + 0x78
    n = infos[1] // 124
    picked = []
    for k in range(n):
        e = src[infos[0] + 8 + k * 124:infos[0] + 8 + (k + 1) * 124]
        name = e[0x0C:0x24].split(b'\0')[0]
        if name in ICONS:
            f = src[fmts[0] + 8 + k * 32:fmts[0] + 8 + (k + 1) * 32]
            offset, _, size = struct.unpack_from('<III', e, 0x30)
            pixels = src[data_start + offset:data_start + offset + size]
            picked.append((struct.unpack_from('<I', e, 0x24)[0], name, bytearray(e), f, pixels))
    if len(picked) != len(ICONS):
        sys.exit('icons not all found: %s' % [p[1] for p in picked])
    picked.sort()   # by hash, as the hash table is

    pack_hash = bhash(PACK_NAME)
    header = bytearray(src[head[0] + 8:head[0] + 8 + 124])
    header[0x04:0x20] = b'INGAME'.ljust(0x1C, b'\0')
    header[0x20:0x60] = PACK_NAME.encode().ljust(0x40, b'\0')
    struct.pack_into('<I', header, 0x60, pack_hash)
    hashes = b''.join(struct.pack('<II', h, 0) for h, _, _, _, _ in picked)
    blob = bytearray()
    info_bytes = b''
    for h, name, e, f, pixels in picked:
        struct.pack_into('<II', e, 0x30, len(blob), 0)   # data offset, and +0x34 cleared as in HUDTEXDRAG
        info_bytes += bytes(e)
        blob += pixels
        while len(blob) % 0x80:
            blob += b'\0'
    fmt_bytes = b''.join(f for _, _, _, f, _ in picked)
    hdr = chunk(0x33310001, bytes(header)) + chunk(0x33310002, hashes) + chunk(0x33310004, info_bytes) + \
        chunk(0x33310005, fmt_bytes)

    out = bytearray()
    out += struct.pack('<II', 0xB3300000, 0)          # size patched below
    out += chunk(0, bytes(48))                          # as in HUDTEXDRAG.BIN: the header at 0x40
    out += chunk(0xB3310000, hdr)
    pad = (-(len(out) + 8)) % 0x80
    out += chunk(0, bytes(pad))                         # the data chunk at a 0x80 boundary
    data_hdr = chunk(0x33320001, struct.pack('<IIIIII', 0, 0, 1, pack_hash, 0, 0))
    inner = data_hdr
    pad2 = (-(len(out) + 8 + len(inner) + 8)) % 0x80
    inner += chunk(0, bytes(pad2))                      # 33320002 at a 0x80 boundary
    inner += chunk(0x33320002, bytes(0x78) + bytes(blob))   # its data 0x78 in: a 0x80 boundary
    out += chunk(0xB3320000, inner)
    struct.pack_into('<I', out, 4, len(out) - 8)
    path = os.path.join(game, 'scripts', 'nfsmw_hud_icons.bin')
    open(path, 'wb').write(out)
    print('%s: %d bytes, %d textures (%s), pack hash %08X' % (path, len(out), len(picked),
          ', '.join(p[1].decode() for p in picked), pack_hash))


if __name__ == '__main__':
    main()
