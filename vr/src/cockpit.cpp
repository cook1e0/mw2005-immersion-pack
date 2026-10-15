// Cockpit: driver's seat and the driver model's head.
//
// Every car has a driver model (<CAR>_KIT00_DRIVER_A..D: body, arms and head in one draw entry,
// hands in another), normally only seen through the windows. In VR the eyes are put where its
// head is, so you sit in the driver's seat, and the head is left out so it isn't in your face.
//
// eViewPlatInterface::Render submits a model one draw entry (a range of the solid's index buffer)
// at a time. The submit call is hooked: the first time a driver solid comes through, each entry's
// triangles are reordered so the head (high up, at the back of the model) comes last, in the
// game's copy of the indices and in the index buffer. Instances whose head is near the eyes then
// get a copy of the entry that stops before the head; other cars keep their whole driver.
//
// The seat: the head of the driver nearest the game camera is measured each frame, in the space of
// the car body it is drawn with. In the bumper and hood cameras (camera close to the head) the
// eyes are put there, plus a per-car offset adjusted with the seat keys and saved to [Seat] in
// nfsmw_vr.ini, and they take the car body's orientation, not the game camera's. The bumper and
// hood cameras lag and shake against the car, so a seat that followed the game camera (as it did
// at first, from the previous frame's measurement) wobbled inside the car. The body matrix is
// rebuilt before the eyes render the same way CarRenderConn::OnRender builds it, so the eyes
// stay fixed to the car model in the same frame.
#include "cockpit.h"
#include "mirror_glass.h"
#include "d3d9hook.h"
#include <d3d9.h>
#include <algorithm>
#include <string.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

float g_steeringWheelAngle = 0.0f;
float g_seatInput[4] = {};

namespace {

// --- speed.exe v1.3 -------------------------------------------------------------------------
constexpr uintptr_t kCallSubmitEntry = 0x6DAD17;    // eViewPlatInterface::Render: call sub_6E0AB0(entry, ...)
constexpr uintptr_t kSubmitEntry = 0x6E0AB0;        // cdecl, queues one draw entry
constexpr uintptr_t kPlayerView = 0x919650;         // &eViews[EVIEW_PLAYER1]
constexpr int kViewCamera = 0x40;
constexpr int kCamPosition = 0x40;

// CarRenderConn (the car's render connection) and how OnRender (0x750E20) places the body.
constexpr uintptr_t kCarRenderConnVtbl = 0x8B5254;
constexpr int kVtblOnRender = 3;                    // OnRender(eView*, int reflection), thiscall
constexpr uintptr_t kOnRender = 0x750E20;
constexpr uintptr_t kFindConn = 0x744D40;           // VehicleRenderConn::Find(world id), cdecl
constexpr int kConnWorldId = 0x30;
constexpr int kConnBodyMatrix = 0x34;               // bMatrix4*: the body's matrix (in-car cameras)
constexpr int kConnCog = 0x50;                      // bVector4: centre of mass offset, body space
constexpr int kConnRenderMatrix = 0x330;            // bMatrix4: smoothed render matrix (other cameras)
constexpr int kViewMovers = 0x44;                   // eView: list of camera movers (node = mover + 4)
constexpr int kMoverGetAnchor = 0x10 / 4;           // CameraMover vtable: GetAnchor()
constexpr int kAnchorWorldId = 0x94;
constexpr int kAnchorPov = 0xC8;                    // int16: 1 = in-car (bumper / hood) camera

// eModel / eSolid / plat info / draw entry
constexpr int kModelSolid = 0x0C;
constexpr int kSolidPlat = 0x00;
constexpr int kSolidMin = 0x20, kSolidMax = 0x30;   // model-space bounding box (x forward, y left, z up)
constexpr int kSolidName = 0xA0;
constexpr int kPlatNumEntries = 0x10, kPlatEntries = 0x14;
constexpr int kPlatIndices = 0x24, kPlatIndexBuffer = 0x28, kPlatNumIndices = 0x2C;
constexpr int kEntrySize = 0x68;
constexpr int kEntryFormat = 0x34;                  // vertex format: +8 stride
constexpr int kEntryTris = 0x40, kEntryFirstIndex = 0x44;
constexpr int kEntryVertices = 0x4C;                // vertex data (first entry only), float xyz first
constexpr int kEntryVertexBytes = 0x50;
constexpr int kEntryNumIndices = 0x5C;

// Head: triangles above the top of the model minus HeadSize, in its rear part (the arms reach
// forward to the wheel at about shoulder height).
constexpr float kHeadDepth = 0.4f;
constexpr float kHeadNear = 0.6f;                   // head within this of the eyes: left out
constexpr float kSeatRange = 3.0f;                  // camera within this of the head: bumper / hood camera
constexpr float kSeatSpeed = 0.1f;                  // m/s while a seat key is held
constexpr float kSeatLimit = 0.6f;

template <typename T> T& At(void* base, int offset) { return *(T*)((char*)base + offset); }

// Each draw entry's triangles are reordered body, arms (and hands), head, so the seated driver
// can be drawn without any of the last two: one or two ranges of the entry.
struct Driver {
    bool ok = false;
    std::string car;
    float head[3] = {};                             // head centre, model space
    std::vector<int> body, arms, heads;             // triangles per entry, in that order
    std::vector<uint8_t> parts;                     // per entry: two ranges to draw (kEntrySize each)
};

// The steering wheel inside a car's interior model (<CAR>_KIT00_INTERIOR_*), found as a ring of
// vertices in front of and below the driver's head: a disc in a plane tilted back from vertical,
// searched over tilt, centre and radius (checked offline against every car in the game). Its
// triangles are moved to the end of each draw entry, so the entry can be drawn as the rest of
// the interior and, with a matrix turned about the steering column, the wheel.
struct Wheel {
    bool ok = false;
    float centre[3] = {};                           // model space (x forward, y left, z up)
    float axis[3] = {};                             // steering column, toward the dashboard
    float radius = 0;
    std::vector<int> kept, turned;                  // per entry: triangles before the wheel, wheel triangles
    std::vector<uint8_t> parts;                     // per entry: the entry without the wheel, the wheel alone
};
struct HeadPos { float v[3]; };
std::unordered_map<std::string, HeadPos> s_carHead; // driver's head per car (model space)

// Solids are streamed: a traffic car's or a reloaded car's model can be freed and another loaded
// at the same address. Each cache entry remembers what the solid looked like when it was seen, and
// is prepared again when that changed. Drawing a freed model's buffers through a stale entry made
// garbage polygons in the cockpit and corrupted d3d9 until it crashed releasing a buffer.
struct SolidInfo {
    Driver* driver = nullptr;                       // nullptr = not a driver
    Wheel* wheel = nullptr;                         // interiors, once the car's driver head is known
    bool interior = false;
    int mirrorKind = -1;                            // -1 not looked at, 0 none, 1 side mirror solid, 2 body, 3 interior
    MirrorGlass glass[3];                           // left, right, rear view (model space)
    char name[64] = {};
    uint8_t* plat = nullptr;
    uint8_t* entries = nullptr;
    void* ib = nullptr;
    uint16_t* indices = nullptr;
    uint32_t indexHash = 0;                         // of the game's index copy, after reordering
};
std::unordered_map<uint8_t*, SolidInfo> s_solids;
std::vector<Driver*> s_retired;                     // replaced this frame; queued draws may still use them
std::vector<Wheel*> s_retiredWheels;

// The game camera (before the seat and the eyes) this frame, as camera -> world.
M4 s_base;
bool s_haveBase = false;

// Nearest driver head to the game camera seen this frame, and the one in use.
struct Seen {
    float dist = 1e9f;
    float cam[3];
    std::string car;
    uint32_t worldId = 0;                           // never retain a render connection across frames
    float body[3];                                  // head in the car body's space (x fwd, y left, z up)
} s_frame;
struct {
    bool valid = false;
    float cam[3];                                   // head in game camera space (right, down, forward)
    std::string car;
    uint32_t worldId = 0;
    float body[3];
    double time = 0;
} s_head;

// The car being drawn in the player view (inside CarRenderConn::OnRender) and its body matrix.
uint8_t* s_curConn = nullptr;
M4 s_curBody;
bool s_curPov;
// Compare only a seat actually applied in this frame, to the same car's player-view draw.
uint8_t* s_seatedConn = nullptr;
M4 s_seatedBody;
float s_bodyMismatch = 0;
unsigned s_bodyDraws = 0;

float s_offset[3];                                  // right, up, forward (m) for s_head.car
std::string s_offsetCar;
bool s_offsetDirty = false;

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

const char* IniPath() {
    static char path[MAX_PATH];
    if (!path[0]) ConfigFile("nfsmw_vr.ini", path, false);
    return path;
}

void LoadOffset(const std::string& car) {
    char buf[96];
    GetPrivateProfileStringA("Seat", car.c_str(), "", buf, sizeof(buf), IniPath());
    if (!buf[0]) GetPrivateProfileStringA("Seat", "Default", "0,0,0", buf, sizeof(buf), IniPath());
    s_offset[0] = s_offset[1] = s_offset[2] = 0;
    sscanf(buf, "%f,%f,%f", &s_offset[0], &s_offset[1], &s_offset[2]);
    s_offsetCar = car;
    Log("cockpit: seat for %s: right %.2f up %.2f forward %.2f", car.c_str(), s_offset[0], s_offset[1], s_offset[2]);
}

void SaveOffset() {
    char buf[96];
    snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f", s_offset[0], s_offset[1], s_offset[2]);
    WritePrivateProfileStringA("Seat", s_offsetCar.c_str(), buf, IniPath());
    s_offsetDirty = false;
    Log("cockpit: seat for %s saved: %s", s_offsetCar.c_str(), buf);
}

// Arms and hands: in front of the head, above the lap, and out to the side (the chest is in the
// middle). Checked against every car's driver model offline.
constexpr float kArmsForward = 0.10f, kArmsBelowHead = 0.42f, kArmsSideways = 0.06f;

// Reorders each entry's triangles body, arms, head (head: high up at the back of the model).
Driver* Prepare(uint8_t* solid, const char* name) {
    Driver* d = new Driver;
    const char* kit = strstr(name, "_KIT");
    d->car.assign(name, kit ? kit - name : strlen(name));
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    int n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    uint8_t* entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    uint16_t* indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    auto* ib = plat ? At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) : nullptr;
    int numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    if (n <= 0 || n > 64 || !entries || !indices || !ib) {
        Log("cockpit: %s: no draw entries", name);
        return d;
    }
    uint8_t* verts = nullptr;
    int stride = 0, numVerts = 0;
    for (int i = 0; i < n && !verts; i++) {
        uint8_t* e = entries + i * kEntrySize;
        verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        stride = fmt ? At<int>(fmt, 8) : 0;
        numVerts = stride > 0 ? At<int>(e, kEntryVertexBytes) / stride : 0;
    }
    D3DINDEXBUFFER_DESC desc;
    if (!verts || stride < 12 || stride > 128 || FAILED(ib->GetDesc(&desc)) || desc.Format != D3DFMT_INDEX16 ||
        desc.Size < (UINT)numIndices * 2) {
        Log("cockpit: %s: unexpected vertex or index data (stride %d)", name, stride);
        return d;
    }
    auto pos = [&](int v) { return (const float*)(verts + v * stride); };
    auto centroid = [&](const uint16_t* tri, float* c) {
        c[0] = c[1] = c[2] = 0;
        for (int k = 0; k < 3; k++) {
            if (tri[k] >= numVerts) return false;
            for (int a = 0; a < 3; a++) c[a] += pos(tri[k])[a] / 3;
        }
        return true;
    };
    float topZ = At<float>(solid, kSolidMax + 8) - g_cfg.headSize;
    float backX = At<float>(solid, kSolidMin) + kHeadDepth;
    auto isHead = [&](const float* c) { return c[2] > topZ && c[0] < backX; };

    // The head's centre first: the arms are found relative to it.
    float sum[3] = {};
    int headVerts = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        if (tris <= 0 || first < 0 || first + tris * 3 > numIndices) continue;
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = indices + first + t * 3;
            float c[3];
            if (!centroid(tri, c) || !isHead(c)) continue;
            for (int k = 0; k < 3; k++, headVerts++)
                for (int a = 0; a < 3; a++) sum[a] += pos(tri[k])[a];
        }
    }
    if (headVerts)
        for (int a = 0; a < 3; a++) d->head[a] = sum[a] / headVerts;

    d->parts.assign(n * 2 * kEntrySize, 0);
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        d->body.push_back(tris);
        d->arms.push_back(0);
        d->heads.push_back(0);
        if (tris <= 0 || first < 0 || first + tris * 3 > numIndices) continue;
        uint16_t* idx = indices + first;
        std::vector<uint16_t> body, arms, head;
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = idx + t * 3;
            float c[3];
            bool valid = centroid(tri, c);
            std::vector<uint16_t>* to = &body;
            if (valid && isHead(c)) to = &head;
            else if (valid && headVerts && c[0] > d->head[0] + kArmsForward && c[2] > d->head[2] - kArmsBelowHead &&
                     fabsf(c[1] - d->head[1]) > kArmsSideways)
                to = &arms;
            to->insert(to->end(), tri, tri + 3);
        }
        d->body[i] = (int)body.size() / 3;
        d->arms[i] = (int)arms.size() / 3;
        d->heads[i] = (int)head.size() / 3;
        if (arms.empty() && head.empty()) continue;
        body.insert(body.end(), arms.begin(), arms.end());
        body.insert(body.end(), head.begin(), head.end());
        void* p = nullptr;
        if (FAILED(ib->Lock(first * 2, tris * 6, &p, 0)) || !p) {
            Log("cockpit: %s: index buffer lock failed", name);
            return d;
        }
        memcpy(p, body.data(), tris * 6);
        ib->Unlock();
        memcpy(idx, body.data(), tris * 6);
    }
    if (headVerts) {
        d->ok = true;
        HeadPos& h = s_carHead[d->car];
        memcpy(h.v, d->head, sizeof(h.v));
    }
    Log("cockpit: %s: head %.2f %.2f %.2f, triangles per entry (body/arms/head):%s", name, d->head[0], d->head[1],
        d->head[2], [&] {
            static char buf[256];
            buf[0] = 0;
            for (int i = 0; i < n && strlen(buf) < 200; i++) sprintf(buf + strlen(buf), " %d/%d/%d", d->body[i], d->arms[i], d->heads[i]);
            return buf;
        }());
    return d;
}

// Ring search, in planes tilted back by 0..40 degrees: for each centre, count vertices by distance
// along the column and by radius, and take the bin with the most vertices spread all round.
bool FindWheel(const std::vector<V3>& pts, const float head[3], Wheel& w) {
    std::vector<V3> cand;
    for (const V3& p : pts)
        if (p.x > head[0] + 0.2f && p.x < head[0] + 0.85f && fabsf(p.y - head[1]) < 0.32f && p.z > head[2] - 0.55f &&
            p.z < head[2] - 0.02f)
            cand.push_back(p);
    if (cand.size() < 12) return false;
    struct Bin { int count; uint8_t octants; };
    static Bin bins[160][20];
    std::vector<int> used;
    int bestScore = 0;
    float best[5] = {};                             // tilt, centre y, centre height, column distance, radius
    std::vector<float> d(cand.size()), wv(cand.size());
    for (int ti = 0; ti <= 16; ti++) {
        float th = ti * 2.5f * 3.14159265f / 180.0f;
        float n[2] = {cosf(th), -sinf(th)}, up[2] = {sinf(th), cosf(th)};   // (x, z) of the column and wheel-up axes
        for (size_t k = 0; k < cand.size(); k++) {
            d[k] = cand[k].x * n[0] + cand[k].z * n[1];
            wv[k] = cand[k].x * up[0] + cand[k].z * up[1];
        }
        float headW = head[0] * up[0] + head[2] * up[1];
        for (int cu10 = -3; cu10 <= 3; cu10++) {
            float cu = head[1] + cu10 * 0.02f;
            for (int cwi = 0; cwi < 50; cwi++) {
                float cw = headW - 0.55f + cwi * 0.01f;
                for (int key : used) bins[key / 20][key % 20] = {0, 0};
                used.clear();
                for (size_t k = 0; k < cand.size(); k++) {
                    float du = cand[k].y - cu, dw = wv[k] - cw;
                    float r = sqrtf(du * du + dw * dw);
                    if (r < 0.12f || r > 0.24f) continue;
                    int db = (int)lrintf(d[k] / 0.02f) + 80, rb = (int)lrintf(r / 0.015f);
                    if (db < 0 || db >= 160 || rb < 0 || rb >= 20) continue;
                    Bin& b = bins[db][rb];
                    if (!b.count) used.push_back(db * 20 + rb);
                    b.count++;
                    int oct = (int)((atan2f(dw, du) + 3.14159265f) / (3.14159265f / 4)) & 7;
                    b.octants |= (uint8_t)(1 << oct);
                }
                for (int key : used) {
                    const Bin& b = bins[key / 20][key % 20];
                    int octs = 0;
                    for (int o = 0; o < 8; o++) octs += (b.octants >> o) & 1;
                    if (octs < 7 || b.count * octs <= bestScore) continue;
                    bestScore = b.count * octs;
                    best[0] = th; best[1] = cu; best[2] = cw;
                    best[3] = (key / 20 - 80) * 0.02f; best[4] = (key % 20) * 0.015f;
                }
            }
        }
    }
    if (!bestScore) return false;
    float th = best[0];
    // Back from (column distance, wheel-up height) to model x and z.
    float c0[3] = {best[3] * cosf(th) + best[2] * sinf(th), best[1], -best[3] * sinf(th) + best[2] * cosf(th)};
    // The search bins are coarse (any tilt within a few degrees holds the whole rim), so fit the rim
    // vertices: centre = their mean, tilt = the plane through them (column kept in the x-z plane).
    float sum[3] = {}, a = 0, b = 0, c = 0;
    int count = 0;
    std::vector<V3> rim;
    for (const V3& p : cand) {
        float rel[3] = {p.x - c0[0], p.y - c0[1], p.z - c0[2]};
        float along = rel[0] * cosf(th) - rel[2] * sinf(th);
        float r = sqrtf(std::max(0.0f, rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] - along * along));
        if (fabsf(along) > 0.03f || fabsf(r - best[4]) > 0.03f) continue;
        rim.push_back(p);
        sum[0] += p.x; sum[1] += p.y; sum[2] += p.z;
        count++;
    }
    if (count < 8) return false;
    float centre[3] = {sum[0] / count, sum[1] / count, sum[2] / count};
    for (const V3& p : rim) {
        float dx = p.x - centre[0], dz = p.z - centre[2];
        a += dx * dx; b += dz * dz; c += dx * dz;
    }
    // Minimises the squared distances along the column (cos t, 0, -sin t): t = atan2(2c, b - a) / 2.
    float fitted = 0.5f * atan2f(2 * c, b - a);
    if (fabsf(fitted - th) < 0.2f) th = fitted;
    w.axis[0] = cosf(th); w.axis[1] = 0; w.axis[2] = -sinf(th);
    memcpy(w.centre, centre, sizeof(centre));
    w.radius = best[4];
    return true;
}

// Whether a vertex lies on the wheel: near its plane and within its rim.
bool OnWheel(const Wheel& w, const float* p) {
    float rel[3] = {p[0] - w.centre[0], p[1] - w.centre[1], p[2] - w.centre[2]};
    float along = rel[0] * w.axis[0] + rel[1] * w.axis[1] + rel[2] * w.axis[2];
    float off2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] - along * along;
    float rim = w.radius + 0.035f;
    return along > -0.06f && along < 0.05f && off2 < rim * rim;
}

Wheel* PrepareWheel(uint8_t* solid, const char* name, const float head[3]) {
    Wheel* w = new Wheel;
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    int n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    uint8_t* entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    uint16_t* indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    auto* ib = plat ? At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) : nullptr;
    int numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    if (n <= 0 || n > 64 || !entries || !indices || !ib) return w;
    uint8_t* verts = nullptr;
    int stride = 0, numVerts = 0;
    for (int i = 0; i < n && !verts; i++) {
        uint8_t* e = entries + i * kEntrySize;
        verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        stride = fmt ? At<int>(fmt, 8) : 0;
        numVerts = stride > 0 ? At<int>(e, kEntryVertexBytes) / stride : 0;
    }
    D3DINDEXBUFFER_DESC desc;
    if (!verts || stride < 12 || stride > 128 || numVerts <= 0 || FAILED(ib->GetDesc(&desc)) ||
        desc.Format != D3DFMT_INDEX16 || desc.Size < (UINT)numIndices * 2)
        return w;
    auto pos = [&](int v) { return (const float*)(verts + v * stride); };
    std::vector<V3> pts(numVerts);
    for (int v = 0; v < numVerts; v++) pts[v] = {pos(v)[0], pos(v)[1], pos(v)[2]};
    if (!FindWheel(pts, head, *w)) {
        Log("cockpit: %s: no steering wheel found", name);
        return w;
    }
    w->parts.assign(n * 2 * kEntrySize, 0);
    int total = 0;
    std::vector<std::vector<uint16_t>> orders(n);
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        w->kept.push_back(tris);
        w->turned.push_back(0);
        if (tris <= 0 || first < 0 || first + tris * 3 > numIndices) continue;
        std::vector<uint16_t> keep, wheel;
        const uint16_t* idx = indices + first;
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = idx + t * 3;
            bool on = tri[0] < numVerts && tri[1] < numVerts && tri[2] < numVerts;
            for (int k = 0; k < 3 && on; k++) on = OnWheel(*w, pos(tri[k]));
            (on ? wheel : keep).insert((on ? wheel : keep).end(), tri, tri + 3);
        }
        w->kept[i] = (int)keep.size() / 3;
        w->turned[i] = (int)wheel.size() / 3;
        total += w->turned[i];
        keep.insert(keep.end(), wheel.begin(), wheel.end());
        orders[i].swap(keep);
    }
    // A real wheel has well over a hundred triangles; a few means something else matched.
    if (total < 80) {
        Log("cockpit: %s: steering wheel candidate rejected (%d triangles)", name, total);
        return w;
    }
    for (int i = 0; i < n; i++) {
        if (!w->turned[i]) continue;
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        void* p = nullptr;
        if (FAILED(ib->Lock(first * 2, tris * 6, &p, 0)) || !p) {
            Log("cockpit: %s: index buffer lock failed", name);
            return w;
        }
        memcpy(p, orders[i].data(), tris * 6);
        ib->Unlock();
        memcpy(indices + first, orders[i].data(), tris * 6);
    }
    w->ok = true;
    Log("cockpit: %s: steering wheel at %.2f %.2f %.2f, column %.2f %.2f %.2f, radius %.3f, %d triangles", name,
        w->centre[0], w->centre[1], w->centre[2], w->axis[0], w->axis[1], w->axis[2], w->radius, total);
    return w;
}

// The car-model -> world matrix with the wheel turned by `degrees` (+ = clockwise, to the right,
// as the driver sees it) about the column: row vectors, so M' = T * M with T turning about the
// centre in model space. A right-handed turn about the column (toward the dash) lifts the left
// side of the wheel (+y) toward the top (+z): clockwise from the driver's seat.
void TurnedMatrix(const Wheel& w, float degrees, const float* m, float* out) {
    float a = degrees * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a), t = 1 - c;
    const float* k = w.axis;
    float R[3][3] = {   // column-vector rotation about k, transposed below for row vectors
        {c + k[0] * k[0] * t, k[0] * k[1] * t - k[2] * s, k[0] * k[2] * t + k[1] * s},
        {k[1] * k[0] * t + k[2] * s, c + k[1] * k[1] * t, k[1] * k[2] * t - k[0] * s},
        {k[2] * k[0] * t - k[1] * s, k[2] * k[1] * t + k[0] * s, c + k[2] * k[2] * t}};
    float T[4][4] = {};
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) T[r][col] = R[col][r];
    for (int col = 0; col < 3; col++) {
        float cr = 0;
        for (int r = 0; r < 3; r++) cr += w.centre[r] * T[r][col];
        T[3][col] = w.centre[col] - cr;
    }
    T[3][3] = 1;
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) {
            float v = 0;
            for (int j = 0; j < 4; j++) v += T[r][j] * m[j * 4 + col];
            out[r * 4 + col] = v;
        }
}

uint32_t IndexHash(const uint16_t* indices, int count) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < count; i++) h = (h ^ indices[i]) * 16777619u;
    return h;
}

// The solid's current plat info, entries, index buffer and index copy (null where missing).
void ReadSolid(uint8_t* solid, SolidInfo& s) {
    memcpy(s.name, solid + kSolidName, sizeof(s.name));
    s.plat = At<uint8_t*>(solid, kSolidPlat);
    s.entries = s.plat ? At<uint8_t*>(s.plat, kPlatEntries) : nullptr;
    s.ib = s.plat ? At<void*>(s.plat, kPlatIndexBuffer) : nullptr;
    s.indices = s.plat ? At<uint16_t*>(s.plat, kPlatIndices) : nullptr;
}

uint32_t CurrentHash(const SolidInfo& s) {
    int n = s.plat ? At<int>(s.plat, kPlatNumIndices) : 0;
    return s.indices && n > 0 && n < (1 << 20) ? IndexHash(s.indices, n) : 0;
}

const HeadPos* CarHead(const char* name) {
    const char* kit = strstr(name, "_KIT");
    auto it = s_carHead.find(std::string(name, kit ? kit - name : strlen(name)));
    return it != s_carHead.end() ? &it->second : nullptr;
}

SolidInfo* GetSolid(uint8_t* solid) {
    SolidInfo now;
    ReadSolid(solid, now);
    char name[65] = {};
    memcpy(name, now.name, 64);
    auto it = s_solids.find(solid);
    if (it != s_solids.end()) {
        SolidInfo& was = it->second;
        bool same = !memcmp(was.name, now.name, sizeof(now.name)) && was.plat == now.plat &&
                    was.entries == now.entries && was.ib == now.ib && was.indices == now.indices;
        // A model reloaded in place has its original triangle order back.
        if (same && ((was.driver && was.driver->ok) || (was.wheel && was.wheel->ok)))
            same = CurrentHash(now) == was.indexHash;
        if (same) {
            // An interior seen before its car's driver: look for the wheel once the head is known.
            if (was.interior && !was.wheel && g_cfg.steeringWheel) {
                if (const HeadPos* head = CarHead(name)) {
                    was.wheel = PrepareWheel(solid, name, head->v);
                    was.indexHash = CurrentHash(now);
                }
            }
            return &was;
        }
        if (was.driver || was.wheel) Log("cockpit: %.64s changed or reloaded, preparing it again", was.name);
        if (was.driver) s_retired.push_back(was.driver);
        if (was.wheel) s_retiredWheels.push_back(was.wheel);
    }
    now.driver = strstr(name, "_DRIVER") ? Prepare(solid, name) : nullptr;
    now.interior = strstr(name, "_INTERIOR") != nullptr;
    if (now.interior && g_cfg.steeringWheel)
        if (const HeadPos* head = CarHead(name)) now.wheel = PrepareWheel(solid, name, head->v);
    now.indexHash = CurrentHash(now);
    SolidInfo& stored = s_solids[solid];
    stored = now;
    return &stored;
}

Driver* GetDriver(uint8_t* solid) { return GetSolid(solid)->driver; }

// Whether CarRenderConn::OnRender draws the car in `view` with the in-car camera's body matrix.
// Decided while the car is drawn: before ___eDisplayFrame the camera's anchor can still say
// otherwise (seen in the hood camera, which draws with the render matrix).
uint8_t* ViewAnchor(uint8_t* view) {
    uint8_t* list = view + kViewMovers;
    uint8_t* node = At<uint8_t*>(list, 0);
    if (node == list || !node) return nullptr;
    uint8_t* mover = node - 4;
    typedef uint8_t*(__thiscall * GetAnchor_t)(uint8_t*);
    return ((GetAnchor_t)(At<void**>(mover, 0)[kMoverGetAnchor]))(mover);
}

bool DrawsWithPov(uint8_t* conn, uint8_t* view) {
    uint8_t* anchor = ViewAnchor(view);
    return anchor && At<int>(anchor, kAnchorWorldId) == At<int>(conn, kConnWorldId) && At<int16_t>(anchor, kAnchorPov) == 1;
}

// The body matrix CarRenderConn::OnRender draws the car with: with `pov`, the body's own
// matrix moved by its centre of mass offset, otherwise the smoothed render matrix. Both are up
// to date before ___eDisplayFrame. Translation in row 3.
M4 BodyMatrix(uint8_t* conn, bool pov) {
    M4 m;
    memcpy(&m, conn + kConnRenderMatrix, sizeof(m));
    const M4* body = pov ? At<const M4*>(conn, kConnBodyMatrix) : nullptr;
    if (!body) return m;
    m = *body;
    const float* cog = &At<float>(conn, kConnCog);
    for (int a = 0; a < 3; a++)
        m.m[3][a] -= cog[0] * m.m[0][a] + cog[1] * m.m[1][a] + cog[2] * m.m[2][a] + cog[3] * m.m[3][a];
    return m;
}

void __fastcall hkOnRender(uint8_t* conn, void* edx, uint8_t* view, int reflection) {
    uint8_t* prev = s_curConn;
    M4 prevBody = s_curBody;
    bool prevPov = s_curPov;
    s_curConn = !reflection && (uintptr_t)view == kPlayerView ? conn : nullptr;
    if (s_curConn) {
        s_curPov = DrawsWithPov(conn, view);
        s_curBody = BodyMatrix(conn, s_curPov);
    }
    if (s_curConn && conn == s_seatedConn) {
        float d = 0;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) d = std::max(d, fabsf(s_curBody.m[i][j] - s_seatedBody.m[i][j]));
        s_bodyMismatch = std::max(s_bodyMismatch, d);
        s_bodyDraws++;
    }
    typedef void(__fastcall * OnRender_t)(uint8_t*, void*, uint8_t*, int);
    ((OnRender_t)kOnRender)(conn, edx, view, reflection);
    s_curConn = prev;
    s_curBody = prevBody;
    s_curPov = prevPov;
}

float Dist(const float* a, const float* b) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

bool DrawingPlayerCar();

// --- side mirrors ------------------------------------------------------------------------------
// The player car's mirror glass drawn in the current eye, with the matrix it was drawn with, for
// the mirror image (game.cpp) to be laid over it.
struct MirrorDraw {
    uint8_t* solid;
    int side;                                       // 0 left, 1 right, 2 rear view (interior)
    bool body;                                      // found in the body (cars without mirror solids)
    MirrorGlass glass;
    M4 m;
};
std::vector<MirrorDraw> s_mirrorDraws;

// All of a solid's triangles (three points each), for which `keep` is true.
template <class Keep> void SolidTriangles(uint8_t* solid, std::vector<V3>& out, Keep keep) {
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    int n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    uint8_t* entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    uint16_t* indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    int numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    if (n <= 0 || n > 64 || !entries || !indices) return;
    uint8_t* verts = nullptr;
    int stride = 0, numVerts = 0;
    for (int i = 0; i < n && !verts; i++) {
        uint8_t* e = entries + i * kEntrySize;
        verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        stride = fmt ? At<int>(fmt, 8) : 0;
        numVerts = stride > 0 ? At<int>(e, kEntryVertexBytes) / stride : 0;
    }
    if (!verts || stride < 12 || stride > 128 || numVerts <= 0) return;
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        if (tris <= 0 || first < 0 || first + tris * 3 > numIndices) continue;
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = indices + first + t * 3;
            if (tri[0] >= numVerts || tri[1] >= numVerts || tri[2] >= numVerts) continue;
            V3 p[3];
            for (int k = 0; k < 3; k++) {
                const float* f = (const float*)(verts + tri[k] * stride);
                p[k] = {f[0], f[1], f[2]};
            }
            if (keep(p)) out.insert(out.end(), p, p + 3);
        }
    }
}

// Side mirrors: the game draws their glass as its own draw entry (the MIRROR material). That entry
// is handed to the d3d9 side, which gives its vertices texture coordinates across the glass and
// draws it with the rear view as its texture: the game's own glass, showing the rear view.
std::unordered_map<std::string, bool> s_hasMirrorSolids;

std::string CarOf(const char* name) {
    const char* kit = strstr(name, "_KIT");
    return std::string(name, kit ? kit - name : strlen(name));
}

void RegisterGlassEntry(uint8_t* solid, SolidInfo& info, int side) {
    char name[65] = {};
    memcpy(name, info.name, 64);
    if (info.mirrorKind == 2 && s_hasMirrorSolids.count(CarOf(name))) return;
    const MirrorGlass& g = info.glass[side];
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    int n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    uint8_t* entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    uint16_t* indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    int numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    void* ib = plat ? At<void*>(plat, kPlatIndexBuffer) : nullptr;
    if (n <= 0 || n > 64 || !entries || !indices || !ib) return;
    uint8_t* verts = nullptr;
    int stride = 0;
    for (int i = 0; i < n && !verts; i++) {
        uint8_t* e = entries + i * kEntrySize;
        verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        stride = fmt ? At<int>(fmt, 8) : 0;
    }
    if (!verts || stride < 12) return;
    float d = mirror::Dot(g.centre, g.normal), cu = mirror::Dot(g.centre, g.u), cv = mirror::Dot(g.centre, g.v);
    auto onGlass = [&](const float* p) {
        V3 q = {p[0], p[1], p[2]};
        return fabsf(mirror::Dot(q, g.normal) - d) < 0.012f && fabsf(mirror::Dot(q, g.u) - cu) < g.halfU + 0.01f &&
               fabsf(mirror::Dot(q, g.v) - cv) < g.halfV + 0.01f;
    };
    // The entry made (almost) only of the glass.
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        if (tris < 2 || first < 0 || first + tris * 3 > numIndices) continue;
        int on = 0;
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = indices + first + t * 3;
            bool all = true;
            for (int k = 0; k < 3 && all; k++) all = onGlass((const float*)(verts + tri[k] * stride));
            on += all;
        }
        if (on * 10 < tris * 8) continue;
        float u0 = side == 0 ? 0.0f : 0.5f, u1 = u0 + 0.5f;
        if (g_cfg.mirrorFlip) {
            float a = 1.0f - u1, b = 1.0f - u0;
            u0 = b;
            u1 = a;
        }
        const float basis[14] = {g.centre.x, g.centre.y, g.centre.z, g.u.x, g.u.y, g.u.z, g.v.x, g.v.y, g.v.z,
                                 g.halfU, g.halfV, u0, u1, 0};
        std::vector<uint16_t> used(indices + first, indices + first + tris * 3);
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        D3D_RegisterMirrorGlass(ib, first, tris, basis, used.data(), (int)used.size());
        Log("mirrors: %s: %s glass is draw entry %d (%d triangles)", name, side ? "right" : "left", i, tris);
        return;
    }
    Log("mirrors: %s: %s glass has no draw entry of its own", name, side ? "right" : "left");
}

// Finds the rear view mirror's glass in the player car's interior the first time it is drawn,
// once its driver's head is known (as for the steering wheel), and the side mirrors' glass.
void FindMirrors(uint8_t* solid, SolidInfo& info) {
    char name[65] = {};
    memcpy(name, info.name, 64);
    if (info.interior) {
        const HeadPos* head = CarHead(name);
        if (!head) return;
        std::vector<V3> tris;
        const float* h = head->v;
        SolidTriangles(solid, tris, [h](const V3* p) { return mirror::InRearViewRegion(p, h); });
        info.glass[2] = mirror::FindGlass(tris, false, mirror::GlassKind::RearView);
        info.mirrorKind = 3;
        const MirrorGlass& g = info.glass[2];
        if (g.ok)
            Log("mirrors: %s: rear view mirror at %.2f %.2f %.2f, %.3f x %.3f, facing %.2f %.2f %.2f", name, g.centre.x,
                g.centre.y, g.centre.z, 2 * g.halfU, 2 * g.halfV, g.normal.x, g.normal.y, g.normal.z);
        else
            Log("mirrors: %s: no rear view mirror found", name);
        return;
    }
    info.mirrorKind = 0;
    bool left = strstr(name, "_LEFT_SIDE_MIRROR") != nullptr, right = strstr(name, "_RIGHT_SIDE_MIRROR") != nullptr;
    if (left || right) {
        std::vector<V3> tris;
        SolidTriangles(solid, tris, [](const V3*) { return true; });
        info.glass[left ? 0 : 1] = mirror::FindGlass(tris);
        info.mirrorKind = 1;
        if (info.glass[left ? 0 : 1].ok) s_hasMirrorSolids[CarOf(name)] = true;
    } else if (strstr(name, "_KIT") && strstr(name, "_BODY_")) {
        for (int side = 0; side < 2; side++) {
            std::vector<V3> tris;
            float s = side == 0 ? 1.0f : -1.0f;
            SolidTriangles(solid, tris, [s](const V3* p) { return mirror::InBodyMirrorRegion(p, s); });
            info.glass[side] = mirror::FindGlass(tris, false);
        }
        info.mirrorKind = 2;
    }
    for (int side = 0; side < 2; side++) {
        const MirrorGlass& g = info.glass[side];
        if (g.ok)
            Log("mirrors: %s: %s glass at %.2f %.2f %.2f, %.3f x %.3f, facing %.2f %.2f %.2f", name, side ? "right" : "left",
                g.centre.x, g.centre.y, g.centre.z, 2 * g.halfU, 2 * g.halfV, g.normal.x, g.normal.y, g.normal.z);
    }
    if (info.mirrorKind == 1 && !info.glass[0].ok && !info.glass[1].ok) Log("mirrors: %s: no glass found", name);
    for (int side = 0; side < 2; side++)
        if (info.glass[side].ok) RegisterGlassEntry(solid, info, side);
}

// The glass's triangles in world space, `lift` metres toward the driver and shrunk by `scale`
// about the glass's centre, with their place in the glass (0..1 across, 0..1 down) as texture
// coordinates: x, y, z, u, v per vertex.
void GlassVertices(const MirrorGlass& g, const M4& m, std::vector<float>& out, float lift = 0.008f, float scale = 1.0f) {
    float cu = mirror::Dot(g.centre, g.u), cv = mirror::Dot(g.centre, g.v);
    out.clear();
    for (const V3& p0 : g.tris) {
        V3 p = {g.centre.x + (p0.x - g.centre.x) * scale, g.centre.y + (p0.y - g.centre.y) * scale,
                g.centre.z + (p0.z - g.centre.z) * scale};
        V3 q = {p.x + g.normal.x * lift, p.y + g.normal.y * lift, p.z + g.normal.z * lift};
        for (int a = 0; a < 3; a++) out.push_back(q.x * m.m[0][a] + q.y * m.m[1][a] + q.z * m.m[2][a] + m.m[3][a]);
        out.push_back(g.halfU > 0 ? (mirror::Dot(p, g.u) - (cu - g.halfU)) / (2 * g.halfU) : 0.0f);
        out.push_back(g.halfV > 0 ? ((cv + g.halfV) - mirror::Dot(p, g.v)) / (2 * g.halfV) : 0.0f);
    }
}

void NoteMirrors(uint8_t* solid, SolidInfo& info, const float* m) {
    if (info.mirrorKind < 0) FindMirrors(solid, info);
    if (info.mirrorKind <= 0) return;
    for (int side = 2; side < 3; side++) {   // the side mirrors are drawn by the game (RegisterGlassEntry)
        if (!info.glass[side].ok) continue;
        bool seen = false;
        for (const MirrorDraw& d : s_mirrorDraws) seen = seen || (d.solid == solid && d.side == side);
        if (seen) continue;
        MirrorDraw d;
        d.solid = solid;
        d.side = side;
        d.body = false;
        d.glass = info.glass[side];
        memcpy(&d.m, m, sizeof(d.m));
        s_mirrorDraws.push_back(d);
        // To be drawn with the model: a dark rim the shape of the glass, and the rear view on the
        // glass shrunk inside it, as if set into the housing.
        std::vector<float> verts, frame;
        float inner = std::min(std::max(g_cfg.mirrorScale, 0.5f), 1.0f);
        GlassVertices(d.glass, d.m, frame, 0.006f, 1.0f);
        GlassVertices(d.glass, d.m, verts, 0.008f, inner);
        float u0 = 0.0f, u1 = 1.0f;
        if (g_cfg.mirrorFlip) {
            float a = 1.0f - u1, b = 1.0f - u0;
            u0 = b;
            u1 = a;
        }
        for (size_t i = 3; i < verts.size(); i += 5) verts[i] = u0 + (u1 - u0) * verts[i];
        uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
        void* ib = plat ? At<void*>(plat, kPlatIndexBuffer) : nullptr;
        D3D_AddMirror(ib, verts.data(), (int)(verts.size() / 5), inner < 0.999f ? frame.data() : nullptr,
                      inner < 0.999f ? (int)(frame.size() / 5) : 0);
    }
}

// The seated driver's ranges set by OnSubmit: s_driverParts entries from s_driverEntry.
uint8_t* s_driverEntry;
int s_driverParts;

// Returns the entry to submit (the first of the seated driver's ranges), or null for none.
uint8_t* __cdecl OnSubmit(uint8_t* model, const float* m, uint8_t* entry) {
    uint8_t* solid = model ? At<uint8_t*>(model, kModelSolid) : nullptr;
    SolidInfo* info = solid ? GetSolid(solid) : nullptr;
    if (info && m && g_cfg.mirrors && DrawingPlayerCar()) NoteMirrors(solid, *info, m);
    Driver* d = info ? info->driver : nullptr;
    if (!d || !d->ok || !m) return entry;
    const float* h = d->head;
    float world[3];
    for (int a = 0; a < 3; a++) world[a] = h[0] * m[a] + h[1] * m[4 + a] + h[2] * m[8 + a] + m[12 + a];

    // Seat measurement: the head in the game camera's space.
    uint8_t* anchor = ViewAnchor((uint8_t*)kPlayerView);
    if (s_haveBase && s_curConn && anchor && At<uint32_t>(anchor, kAnchorWorldId) == At<uint32_t>(s_curConn, kConnWorldId)) {
        float rel[3], cam[3];
        for (int a = 0; a < 3; a++) rel[a] = world[a] - s_base.m[3][a];
        for (int r = 0; r < 3; r++) cam[r] = rel[0] * s_base.m[r][0] + rel[1] * s_base.m[r][1] + rel[2] * s_base.m[r][2];
        float dist = sqrtf(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
        if (dist < s_frame.dist) {
            s_frame.dist = dist;
            memcpy(s_frame.cam, cam, sizeof(cam));
            if (s_frame.car != d->car) s_frame.car = d->car;
            s_frame.worldId = At<uint32_t>(s_curConn, kConnWorldId);
            if (s_curConn) {
                // Rows 0..2 of the body matrix are its axes: project onto them.
                const M4& b = s_curBody;
                for (int r = 0; r < 3; r++) {
                    float len2 = b.m[r][0] * b.m[r][0] + b.m[r][1] * b.m[r][1] + b.m[r][2] * b.m[r][2];
                    float dot = (world[0] - b.m[3][0]) * b.m[r][0] + (world[1] - b.m[3][1]) * b.m[r][1] +
                                (world[2] - b.m[3][2]) * b.m[r][2];
                    s_frame.body[r] = len2 > 1e-8f ? dot / len2 : 0.0f;
                }
            }
        }
    }

    bool hideArms = g_cfg.hideArms > 0.5f;
    if (!g_cfg.hideHead && !hideArms) return entry;
    void* cam = At<void*>((void*)kPlayerView, kViewCamera);
    if (!cam || Dist(world, &At<float>(cam, kCamPosition)) > kHeadNear) return entry;
    uint8_t* entries = At<uint8_t*>(At<uint8_t*>(solid, kSolidPlat), kPlatEntries);
    int i = (int)(entry - entries) / kEntrySize;
    if (entry < entries || i >= (int)d->body.size()) return entry;
    // The seated driver: body, then the arms unless hidden, then the head unless hidden. Copied
    // from the live entry each time, so its vertex buffer and data are the current ones.
    int first = At<int>(entry, kEntryFirstIndex);
    int ranges[2][2] = {{first, d->body[i] + (hideArms ? 0 : d->arms[i])}, {0, 0}};
    if (!g_cfg.hideHead) {
        if (hideArms) { ranges[1][0] = first + (d->body[i] + d->arms[i]) * 3; ranges[1][1] = d->heads[i]; }
        else ranges[0][1] += d->heads[i];
    }
    s_driverParts = 0;
    for (auto& r : ranges) {
        if (r[1] <= 0) continue;
        uint8_t* part = &d->parts[(i * 2 + s_driverParts++) * kEntrySize];
        memcpy(part, entry, kEntrySize);
        At<int>(part, kEntryFirstIndex) = r[0];
        At<int>(part, kEntryTris) = r[1];
        At<int>(part, kEntryNumIndices) = r[1] * 3;
    }
    if (!s_driverParts) return nullptr;
    s_driverEntry = &d->parts[i * 2 * kEntrySize];
    return s_driverEntry;
}

// Whether the car drawn now in the player view is the player's own (the camera's anchor).
bool DrawingPlayerCar() {
    if (!s_curConn) return false;
    uint8_t* anchor = ViewAnchor((uint8_t*)kPlayerView);
    return anchor && At<uint32_t>(anchor, kAnchorWorldId) == At<uint32_t>(s_curConn, kConnWorldId);
}

// sub_6E0AB0 queues an entry for drawing, keeping pointers to it and to the matrix (its 6th
// argument, eViewPlatInterface::Render's local-to-world matrix) until the queue is drawn: turned
// matrices live in a ring of slots that outlasts a frame.
typedef void(__cdecl* SubmitEntry_t)(uint8_t* entry, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                     const float* matrix, uint32_t a7, uint32_t a8, uint32_t a9);
uint8_t* s_submitModel;
float s_turned[256][16];
unsigned s_turnedNext;

void __cdecl SubmitEntry(uint8_t* entry, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, const float* matrix,
                         uint32_t a7, uint32_t a8, uint32_t a9) {
    auto submit = (SubmitEntry_t)kSubmitEntry;
    uint8_t* model = s_submitModel;
    s_driverEntry = nullptr;
    uint8_t* e = OnSubmit(model, matrix, entry);
    if (!e) return;
    if (e == s_driverEntry) {
        for (int k = 0; k < s_driverParts; k++) submit(e + k * kEntrySize, a2, a3, a4, a5, matrix, a7, a8, a9);
        return;
    }
    uint8_t* solid = model ? At<uint8_t*>(model, kModelSolid) : nullptr;
    auto it = solid && e == entry ? s_solids.find(solid) : s_solids.end();
    Wheel* w = it != s_solids.end() ? it->second.wheel : nullptr;
    float angle = g_steeringWheelAngle;
    if (w && w->ok && matrix && fabsf(angle) > 0.05f && DrawingPlayerCar()) {
        uint8_t* entries = At<uint8_t*>(At<uint8_t*>(solid, kSolidPlat), kPlatEntries);
        int i = (int)(entry - entries) / kEntrySize;
        if (entry >= entries && i < (int)w->turned.size() && w->turned[i] > 0) {
            // Copied from the live entry each time, like the seated driver's ranges.
            uint8_t* rest = &w->parts[(i * 2) * kEntrySize];
            uint8_t* wheel = &w->parts[(i * 2 + 1) * kEntrySize];
            memcpy(rest, entry, kEntrySize);
            memcpy(wheel, entry, kEntrySize);
            At<int>(rest, kEntryTris) = w->kept[i];
            At<int>(rest, kEntryNumIndices) = w->kept[i] * 3;
            At<int>(wheel, kEntryFirstIndex) = At<int>(entry, kEntryFirstIndex) + w->kept[i] * 3;
            At<int>(wheel, kEntryTris) = w->turned[i];
            At<int>(wheel, kEntryNumIndices) = w->turned[i] * 3;
            float* turned = s_turned[s_turnedNext++ % 256];
            TurnedMatrix(*w, angle, matrix, turned);
            if (w->kept[i] > 0) submit(rest, a2, a3, a4, a5, matrix, a7, a8, a9);
            submit(wheel, a2, a3, a4, a5, turned, a7, a8, a9);
            return;
        }
    }
    submit(e, a2, a3, a4, a5, matrix, a7, a8, a9);
}

// Replaces eViewPlatInterface::Render's call of sub_6E0AB0, keeping its arguments: the model
// (Render's first argument) is passed on the side.
__declspec(naked) void hkSubmitEntry() {
    __asm {
        mov eax, dword ptr [ebp + 8]
        mov s_submitModel, eax
        jmp SubmitEntry
    }
}

bool PatchCall(uintptr_t site, uintptr_t target, void* hook) {
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != target) {
        Log("cockpit: unexpected code at 0x%X, not patched", (unsigned)site);
        return false;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)hook - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool Held(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000); }

}  // namespace

void Cockpit_Install() {
    if (!PatchCall(kCallSubmitEntry, kSubmitEntry, (void*)&hkSubmitEntry)) return;
    void** slot = (void**)kCarRenderConnVtbl + kVtblOnRender;
    if (*slot == (void*)kOnRender) {
        DWORD old;
        VirtualProtect(slot, 4, PAGE_READWRITE, &old);
        *slot = (void*)&hkOnRender;
        VirtualProtect(slot, 4, old, &old);
    } else {
        Log("cockpit: unexpected CarRenderConn::OnRender, body-locked seat unavailable");
    }
    Log("cockpit: installed (seat %s, hide head %s)", g_cfg.seat ? "on" : "off", g_cfg.hideHead ? "on" : "off");
}

bool Cockpit_ApplySeat(M4& view) {
    s_seatedConn = nullptr;
    s_base = MInvRigid(view);
    s_haveBase = true;
    if (!g_cfg.seat || !s_head.valid || Now() - s_head.time > 0.5) return false;
    float d = sqrtf(s_head.cam[0] * s_head.cam[0] + s_head.cam[1] * s_head.cam[1] + s_head.cam[2] * s_head.cam[2]);
    if (d > kSeatRange) return false;
    if (s_head.car != s_offsetCar) {
        if (s_offsetDirty) SaveOffset();
        LoadOffset(s_head.car);
    }
    uint8_t* anchor = ViewAnchor((uint8_t*)kPlayerView);
    if (!anchor || At<uint32_t>(anchor, kAnchorWorldId) != s_head.worldId) return false;
    // The previous car may have unloaded. Resolve the current connection by id, rather than
    // inspecting yesterday's pointer (which may already be freed or reused by a traffic car).
    auto find = (uint8_t*(__cdecl*)(uint32_t))kFindConn;
    uint8_t* conn = find(s_head.worldId);
    if (!conn || At<uintptr_t>(conn, 0) != kCarRenderConnVtbl) return false;
    M4 w = s_base;
    {
        // Fixed to the car body as it is drawn this frame. Camera rows: right = -left, down = -up,
        // forward; the body's rows are forward, left, up.
        M4 b = BodyMatrix(conn, DrawsWithPov(conn, (uint8_t*)kPlayerView));
        s_seatedBody = b;
        const int row[3] = {1, 2, 0};
        const float sgn[3] = {-1, -1, 1};
        for (int i = 0; i < 3; i++) {
            const float* r = b.m[row[i]];
            float len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (len < 1e-6f) return false;
            for (int a = 0; a < 3; a++) w.m[i][a] = sgn[i] * r[a] / len;
        }
        // The head, then the seat offset (right, up, forward) along the car's axes.
        const float* h = s_head.body;
        float local[3] = {h[0] + s_offset[2], h[1] - s_offset[0], h[2] + s_offset[1]};
        for (int a = 0; a < 3; a++)
            w.m[3][a] = b.m[3][a] + local[0] * b.m[0][a] + local[1] * b.m[1][a] + local[2] * b.m[2][a];
    }
    s_seatedConn = conn;
    view = MInvRigid(w);
    return true;
}

void Cockpit_EndFrame() {
    for (Driver* d : s_retired) delete d;
    s_retired.clear();
    for (Wheel* w : s_retiredWheels) delete w;
    s_retiredWheels.clear();
    static double lastReport;
    if (s_bodyDraws && Now() - lastReport > 5.0) {
        lastReport = Now();
        Log("cockpit: seat/body max difference %.6f over %u draws", s_bodyMismatch, s_bodyDraws);
        s_bodyMismatch = 0;
        s_bodyDraws = 0;
    }
    if (s_frame.dist < 1e8f) {
        // The seat is a fixed point in the body. Re-measuring it each frame makes LOD changes
        // and different head meshes move the eyes around inside the cockpit.
        bool newCar = !s_head.valid || s_head.worldId != s_frame.worldId || s_head.car != s_frame.car;
        s_head.valid = true;
        memcpy(s_head.cam, s_frame.cam, sizeof(s_head.cam));
        s_head.car = s_frame.car;
        s_head.worldId = s_frame.worldId;
        if (newCar) memcpy(s_head.body, s_frame.body, sizeof(s_head.body));
        s_head.time = Now();
    }
    s_frame.dist = 1e9f;
    s_frame.worldId = 0;
    s_seatedConn = nullptr;
    s_haveBase = false;
}

void Cockpit_Keys() {
    static double last = Now();
    double now = Now();
    float dt = (float)std::min(now - last, 0.1);
    last = now;
    if (!g_cfg.seat || s_offsetCar.empty() || !GameFocused()) return;
    bool reset = Held(g_cfg.keySeatReset) || g_seatInput[3] > 0.5f;
    if (reset) {
        s_offset[0] = s_offset[1] = s_offset[2] = 0;
        s_offsetDirty = true;
    }
    const int keys[3][2] = {{g_cfg.keySeatLeft, g_cfg.keySeatRight},
                            {g_cfg.keySeatDown, g_cfg.keySeatUp},
                            {g_cfg.keySeatBack, g_cfg.keySeatForward}};
    bool moving = false;
    for (int a = 0; a < 3; a++) {
        int dir = (Held(keys[a][1]) ? 1 : 0) - (Held(keys[a][0]) ? 1 : 0);
        if (!dir && g_seatInput[a] != 0.0f) dir = g_seatInput[a] > 0 ? 1 : -1;
        if (!dir) continue;
        s_offset[a] = std::max(-kSeatLimit, std::min(kSeatLimit, s_offset[a] + dir * kSeatSpeed * dt));
        moving = s_offsetDirty = true;
    }
    if (s_offsetDirty && !moving && !reset) SaveOffset();
}

void Cockpit_BeginEye() { s_mirrorDraws.clear(); }
