// scripts\nfsmw_hud_icons.bin built from the game's own files on the first start (see iconpack.h);
// the same pack gameplay\tools\make_icon_pack.py writes, byte for byte.
//
// The front end texture pack (FRONTEND\FrontB.lzc, the TPK chunk B3300000 naming
// FrontEndTextures.tpk) has the two icons. The new pack is laid out like GLOBAL\HUDTEXDRAG.BIN: a
// TPK chunk holding the header (33310001: the source's, with "INGAME", the pack's name and its
// bStringHash at +0x60), the texture hashes (33310002, 8 bytes each, sorted), the texture infos
// (33310004, 124 bytes each: name +0x0C, hash +0x24, data offset +0x30, size +0x38) and their
// formats (33310005, 32 bytes each), then the data chunk (B3320000: 33320001 with the pack's hash,
// and 33320002 whose texture data starts 0x78 bytes in, at a 0x80 boundary). Padding chunks (id 0)
// keep both at 0x80 boundaries, as in the game's packs.
#include "iconpack.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

typedef std::vector<uint8_t> Bytes;

const char* const kIcons[] = {"PERFORMANCE_BRAKES", "MAIN_ICON_QUICK_RACE"};
const char kPackName[] = "Global\\NfsmwModHudIcons.tpk";

uint32_t U32(const Bytes& d, size_t at) {
    uint32_t v;
    memcpy(&v, &d[at], 4);
    return v;
}

void Put32(Bytes& d, size_t at, uint32_t v) { memcpy(&d[at], &v, 4); }

uint32_t BHash(const char* s) {
    uint32_t h = 0xFFFFFFFF;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

void Chunk(Bytes& out, uint32_t id, const Bytes& body) {
    size_t at = out.size();
    out.resize(at + 8);
    Put32(out, at, id);
    Put32(out, at + 4, (uint32_t)body.size());
    out.insert(out.end(), body.begin(), body.end());
}

// The chunk `id` among the chunks in [start, end): its offset, or false.
bool Find(const Bytes& d, size_t start, size_t end, uint32_t id, size_t* at, uint32_t* size) {
    for (size_t off = start; off + 8 <= end;) {
        uint32_t c = U32(d, off), s = U32(d, off + 4);
        if (off + 8 + s > end) return false;
        if (c == id) {
            *at = off;
            *size = s;
            return true;
        }
        off += 8 + s;
    }
    return false;
}

bool ReadFile(const std::string& path, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

struct Picked {
    uint32_t hash;
    Bytes info, format, pixels;
};

}  // namespace

bool IconPack_Build(const std::string& gameDir, std::string* error) {
    Bytes src;
    if (!ReadFile(gameDir + "FRONTEND\\FrontB.lzc", src)) return *error = "FRONTEND\\FrontB.lzc not readable", false;
    // The front end texture pack: the TPK chunk whose header names FrontEndTextures.tpk.
    size_t t0 = 0;
    uint32_t tsize = 0;
    bool found = false;
    for (size_t off = 0; off + 8 <= src.size() && !found;) {
        uint32_t c = U32(src, off), s = U32(src, off + 4);
        if (off + 8 + s > src.size()) break;
        size_t look = std::min<size_t>(512, src.size() - off);
        static const char kName[] = "FrontEndTextures.tpk";
        if (c == 0xB3300000 && std::search(src.begin() + off, src.begin() + off + look, kName, kName + sizeof(kName) - 1) !=
                                   src.begin() + off + look) {
            t0 = off, tsize = s, found = true;
            break;
        }
        off += 8 + s;
    }
    if (!found) return *error = "FrontEndTextures.tpk not found in FrontB.lzc", false;
    size_t hp, dp, infos, fmts, head, data;
    uint32_t hsize, dsize, infosSize, fmtsSize, headSize, dataSize;
    if (!Find(src, t0 + 8, t0 + 8 + tsize, 0xB3310000, &hp, &hsize) ||
        !Find(src, t0 + 8, t0 + 8 + tsize, 0xB3320000, &dp, &dsize) ||
        !Find(src, hp + 8, hp + 8 + hsize, 0x33310004, &infos, &infosSize) ||
        !Find(src, hp + 8, hp + 8 + hsize, 0x33310005, &fmts, &fmtsSize) ||
        !Find(src, hp + 8, hp + 8 + hsize, 0x33310001, &head, &headSize) ||
        !Find(src, dp + 8, dp + 8 + dsize, 0x33320002, &data, &dataSize) || headSize < 124)
        return *error = "unexpected layout of FrontEndTextures.tpk", false;
    size_t dataStart = data + 8 + 0x78;
    std::vector<Picked> picked;
    for (uint32_t k = 0; k < infosSize / 124; k++) {
        size_t e = infos + 8 + k * 124;
        char name[25] = {};
        memcpy(name, &src[e + 0x0C], 24);
        for (const char* icon : kIcons) {
            if (strcmp(name, icon) != 0) continue;
            size_t f = fmts + 8 + k * 32;
            uint32_t offset = U32(src, e + 0x30), size = U32(src, e + 0x38);
            if (f + 32 > src.size() || dataStart + offset + size > src.size())
                return *error = std::string("bad texture data for ") + icon, false;
            Picked p;
            p.hash = U32(src, e + 0x24);
            p.info.assign(src.begin() + e, src.begin() + e + 124);
            p.format.assign(src.begin() + f, src.begin() + f + 32);
            p.pixels.assign(src.begin() + dataStart + offset, src.begin() + dataStart + offset + size);
            picked.push_back(p);
        }
    }
    if (picked.size() != sizeof(kIcons) / sizeof(kIcons[0])) return *error = "the icons aren't all in FrontB.lzc", false;
    std::sort(picked.begin(), picked.end(), [](const Picked& a, const Picked& b) { return a.hash < b.hash; });

    uint32_t packHash = BHash(kPackName);
    Bytes header(src.begin() + head + 8, src.begin() + head + 8 + 124);
    std::fill(header.begin() + 0x04, header.begin() + 0x60, 0);
    memcpy(&header[0x04], "INGAME", 6);
    memcpy(&header[0x20], kPackName, sizeof(kPackName) - 1);
    Put32(header, 0x60, packHash);
    Bytes hashes, infoBytes, fmtBytes, blob;
    for (const Picked& p : picked) {
        size_t at = hashes.size();
        hashes.resize(at + 8, 0);
        Put32(hashes, at, p.hash);
    }
    for (Picked& p : picked) {
        Put32(p.info, 0x30, (uint32_t)blob.size());   // data offset, and +0x34 cleared as in HUDTEXDRAG
        Put32(p.info, 0x34, 0);
        infoBytes.insert(infoBytes.end(), p.info.begin(), p.info.end());
        blob.insert(blob.end(), p.pixels.begin(), p.pixels.end());
        blob.resize((blob.size() + 0x7F) & ~(size_t)0x7F, 0);
        fmtBytes.insert(fmtBytes.end(), p.format.begin(), p.format.end());
    }
    Bytes hdr;
    Chunk(hdr, 0x33310001, header);
    Chunk(hdr, 0x33310002, hashes);
    Chunk(hdr, 0x33310004, infoBytes);
    Chunk(hdr, 0x33310005, fmtBytes);

    Bytes out(8, 0);                                    // B3300000, its size patched below
    Put32(out, 0, 0xB3300000);
    Chunk(out, 0, Bytes(48, 0));                        // as in HUDTEXDRAG.BIN: the header at 0x40
    Chunk(out, 0xB3310000, hdr);
    Chunk(out, 0, Bytes((0x80 - (out.size() + 8) % 0x80) % 0x80, 0));   // the data chunk at a 0x80 boundary
    Bytes inner, dataHdr(24, 0);
    Put32(dataHdr, 8, 1);
    Put32(dataHdr, 12, packHash);
    Chunk(inner, 0x33320001, dataHdr);
    Chunk(inner, 0, Bytes((0x80 - (out.size() + 8 + inner.size() + 8) % 0x80) % 0x80, 0));   // 33320002 at a 0x80 boundary
    Bytes texData(0x78, 0);                             // its data 0x78 in: a 0x80 boundary
    texData.insert(texData.end(), blob.begin(), blob.end());
    Chunk(inner, 0x33320002, texData);
    Chunk(out, 0xB3320000, inner);
    Put32(out, 4, (uint32_t)(out.size() - 8));

    std::string path = gameDir + "scripts\\nfsmw_hud_icons.bin", tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return *error = "can't write " + tmp, false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return *error = "can't write " + path, false;
    }
    return true;
}
