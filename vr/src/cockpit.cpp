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
#include <d3d9.h>
#include <algorithm>
#include <string.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

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

struct Driver {
    bool ok = false;
    std::string car;
    float head[3] = {};                             // head centre, model space
    std::vector<uint8_t> trimmed;                   // entries without the head (kEntrySize each)
    std::vector<int> kept;                          // triangles left per entry
};

// Solids are streamed: a traffic car's or a reloaded car's model can be freed and another loaded
// at the same address. Each cache entry remembers what the solid looked like when it was seen, and
// is prepared again when that changed. Drawing a freed model's buffers through a stale entry made
// garbage polygons in the cockpit and corrupted d3d9 until it crashed releasing a buffer.
struct SolidInfo {
    Driver* driver = nullptr;                       // nullptr = not a driver
    char name[64] = {};
    uint8_t* plat = nullptr;
    uint8_t* entries = nullptr;
    void* ib = nullptr;
    uint16_t* indices = nullptr;
    uint32_t indexHash = 0;                         // of the game's index copy, after reordering
};
std::unordered_map<uint8_t*, SolidInfo> s_solids;
std::vector<Driver*> s_retired;                     // replaced this frame; queued draws may still use them

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

// Reorders each entry's triangles so the head comes last and makes the trimmed entries.
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
    float topZ = At<float>(solid, kSolidMax + 8) - g_cfg.headSize;
    float backX = At<float>(solid, kSolidMin) + kHeadDepth;
    float sum[3] = {};
    int headVerts = 0;
    d->trimmed.assign(entries, entries + n * kEntrySize);
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        d->kept.push_back(tris);
        if (tris <= 0 || first < 0 || first + tris * 3 > numIndices) continue;
        uint16_t* idx = indices + first;
        std::vector<uint16_t> keep, head;
        for (int t = 0; t < tris; t++) {
            uint16_t* tri = idx + t * 3;
            float c[3] = {};
            bool valid = true;
            for (int k = 0; k < 3; k++) {
                if (tri[k] >= numVerts) valid = false;
                else for (int a = 0; a < 3; a++) c[a] += pos(tri[k])[a] / 3;
            }
            bool isHead = valid && c[2] > topZ && c[0] < backX;
            (isHead ? head : keep).insert((isHead ? head : keep).end(), tri, tri + 3);
            if (isHead)
                for (int k = 0; k < 3; k++, headVerts++)
                    for (int a = 0; a < 3; a++) sum[a] += pos(tri[k])[a];
        }
        if (head.empty()) continue;
        keep.insert(keep.end(), head.begin(), head.end());
        void* p = nullptr;
        if (FAILED(ib->Lock(first * 2, tris * 6, &p, 0)) || !p) {
            Log("cockpit: %s: index buffer lock failed", name);
            return d;
        }
        memcpy(p, keep.data(), tris * 6);
        ib->Unlock();
        memcpy(idx, keep.data(), tris * 6);
        int kept = tris - (int)head.size() / 3;
        d->kept[i] = kept;
        At<int>(&d->trimmed[i * kEntrySize], kEntryTris) = kept;
        At<int>(&d->trimmed[i * kEntrySize], kEntryNumIndices) = kept * 3;
    }
    if (headVerts) {
        for (int a = 0; a < 3; a++) d->head[a] = sum[a] / headVerts;
        d->ok = true;
    }
    Log("cockpit: %s: head %.2f %.2f %.2f, triangles kept per entry:%s", name, d->head[0], d->head[1], d->head[2],
        [&] {
            static char buf[128];
            buf[0] = 0;
            for (int k : d->kept) sprintf(buf + strlen(buf), " %d", k);
            return buf;
        }());
    return d;
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

Driver* GetDriver(uint8_t* solid) {
    SolidInfo now;
    ReadSolid(solid, now);
    auto it = s_solids.find(solid);
    if (it != s_solids.end()) {
        const SolidInfo& was = it->second;
        bool same = !memcmp(was.name, now.name, sizeof(now.name)) && was.plat == now.plat &&
                    was.entries == now.entries && was.ib == now.ib && was.indices == now.indices;
        // A driver reloaded in place has its original triangle order back.
        if (same && was.driver && was.driver->ok) same = CurrentHash(now) == was.indexHash;
        if (same) return was.driver;
        if (was.driver) {
            Log("cockpit: %.64s changed or reloaded, preparing it again", was.name);
            s_retired.push_back(was.driver);
        }
    }
    char name[65] = {};
    memcpy(name, now.name, 64);
    now.driver = strstr(name, "_DRIVER") ? Prepare(solid, name) : nullptr;
    now.indexHash = CurrentHash(now);
    s_solids[solid] = now;
    return now.driver;
}

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

// Returns the entry to submit (the trimmed copy for a head next to the eyes), or null for none.
uint8_t* __cdecl OnSubmit(uint8_t* model, const float* m, uint8_t* entry) {
    uint8_t* solid = model ? At<uint8_t*>(model, kModelSolid) : nullptr;
    Driver* d = solid ? GetDriver(solid) : nullptr;
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

    if (!g_cfg.hideHead) return entry;
    void* cam = At<void*>((void*)kPlayerView, kViewCamera);
    if (!cam || Dist(world, &At<float>(cam, kCamPosition)) > kHeadNear) return entry;
    uint8_t* entries = At<uint8_t*>(At<uint8_t*>(solid, kSolidPlat), kPlatEntries);
    int i = (int)(entry - entries) / kEntrySize;
    if (entry < entries || i >= (int)d->kept.size()) return entry;
    if (d->kept[i] <= 0) return nullptr;
    // Copied from the live entry each time, so its vertex buffer and data are the current ones.
    uint8_t* trimmed = &d->trimmed[i * kEntrySize];
    memcpy(trimmed, entry, kEntrySize);
    At<int>(trimmed, kEntryTris) = d->kept[i];
    At<int>(trimmed, kEntryNumIndices) = d->kept[i] * 3;
    return trimmed;
}

__declspec(naked) void hkSubmitEntry() {
    __asm {
        push dword ptr [esp + 4]        // entry
        push dword ptr [ebp + 0Ch]      // local-to-world matrix (eViewPlatInterface::Render's 2nd arg)
        push dword ptr [ebp + 8]        // model
        call OnSubmit
        add esp, 12
        test eax, eax
        jz skip
        mov [esp + 4], eax
        mov eax, kSubmitEntry
        jmp eax
    skip:
        ret
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
    if (Held(g_cfg.keySeatReset)) {
        s_offset[0] = s_offset[1] = s_offset[2] = 0;
        s_offsetDirty = true;
    }
    const int keys[3][2] = {{g_cfg.keySeatLeft, g_cfg.keySeatRight},
                            {g_cfg.keySeatDown, g_cfg.keySeatUp},
                            {g_cfg.keySeatBack, g_cfg.keySeatForward}};
    bool moving = false;
    for (int a = 0; a < 3; a++) {
        int dir = (Held(keys[a][1]) ? 1 : 0) - (Held(keys[a][0]) ? 1 : 0);
        if (!dir) continue;
        s_offset[a] = std::max(-kSeatLimit, std::min(kSeatLimit, s_offset[a] + dir * kSeatSpeed * dt));
        moving = s_offsetDirty = true;
    }
    if (s_offsetDirty && !moving && !Held(g_cfg.keySeatReset)) SaveOffset();
}
