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
// The seat: the head of the driver nearest the game camera is measured in that camera's space
// each frame. In the bumper and hood cameras (the camera is rigid to the car and close to the
// head) the eyes are moved there, plus a per-car offset adjusted with the seat keys and saved to
// [Seat] in nfsmw_vr.ini.
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
std::unordered_map<uint8_t*, Driver*> s_solids;     // nullptr = not a driver

// The game camera (before the seat and the eyes) this frame, as camera -> world.
M4 s_base;
bool s_haveBase = false;

// Nearest driver head to the game camera seen this frame, and the one in use.
struct Seen {
    float dist = 1e9f;
    float cam[3];
    std::string car;
} s_frame;
struct {
    bool valid = false;
    float cam[3];                                   // head in game camera space (right, down, forward)
    std::string car;
    double time = 0;
} s_head;

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
    if (!path[0]) snprintf(path, sizeof(path), "%snfsmw_vr.ini", GameDir());
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

Driver* GetDriver(uint8_t* solid) {
    auto it = s_solids.find(solid);
    if (it != s_solids.end()) return it->second;
    char name[65] = {};
    memcpy(name, solid + kSolidName, 64);
    Driver* d = strstr(name, "_DRIVER") ? Prepare(solid, name) : nullptr;
    s_solids[solid] = d;
    return d;
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
    if (s_haveBase) {
        float rel[3], cam[3];
        for (int a = 0; a < 3; a++) rel[a] = world[a] - s_base.m[3][a];
        for (int r = 0; r < 3; r++) cam[r] = rel[0] * s_base.m[r][0] + rel[1] * s_base.m[r][1] + rel[2] * s_base.m[r][2];
        float dist = sqrtf(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
        if (dist < s_frame.dist) {
            s_frame.dist = dist;
            memcpy(s_frame.cam, cam, sizeof(cam));
            if (s_frame.car != d->car) s_frame.car = d->car;
        }
    }

    if (!g_cfg.hideHead) return entry;
    void* cam = At<void*>((void*)kPlayerView, kViewCamera);
    if (!cam || Dist(world, &At<float>(cam, kCamPosition)) > kHeadNear) return entry;
    uint8_t* entries = At<uint8_t*>(At<uint8_t*>(solid, kSolidPlat), kPlatEntries);
    int i = (int)(entry - entries) / kEntrySize;
    if (entry < entries || i >= (int)d->kept.size()) return entry;
    return d->kept[i] > 0 ? &d->trimmed[i * kEntrySize] : nullptr;
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
    if (PatchCall(kCallSubmitEntry, kSubmitEntry, (void*)&hkSubmitEntry))
        Log("cockpit: installed (seat %s, hide head %s)", g_cfg.seat ? "on" : "off", g_cfg.hideHead ? "on" : "off");
}

bool Cockpit_ApplySeat(M4& view) {
    s_base = MInvRigid(view);
    s_haveBase = true;
    if (!g_cfg.seat || !s_head.valid || Now() - s_head.time > 0.5) return false;
    float d = sqrtf(s_head.cam[0] * s_head.cam[0] + s_head.cam[1] * s_head.cam[1] + s_head.cam[2] * s_head.cam[2]);
    if (d > kSeatRange) return false;
    if (s_head.car != s_offsetCar) {
        if (s_offsetDirty) SaveOffset();
        LoadOffset(s_head.car);
    }
    // Camera space: +x right, +y down, +z forward.
    float c[3] = {s_head.cam[0] + s_offset[0], s_head.cam[1] - s_offset[1], s_head.cam[2] + s_offset[2]};
    M4 w = s_base;
    for (int a = 0; a < 3; a++) w.m[3][a] += c[0] * s_base.m[0][a] + c[1] * s_base.m[1][a] + c[2] * s_base.m[2][a];
    view = MInvRigid(w);
    return true;
}

void Cockpit_EndFrame() {
    if (s_frame.dist < 1e8f) {
        s_head.valid = true;
        memcpy(s_head.cam, s_frame.cam, sizeof(s_head.cam));
        s_head.car = s_frame.car;
        s_head.time = Now();
    }
    s_frame.dist = 1e9f;
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
