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
#include <array>
#include <map>
#include <set>
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
constexpr uintptr_t kRvmViewPlat = 0x919730;         // eViews[EVIEW_PLAYER1_RVM]: its eViewPlatInfo*

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
    // The in-car dashboard: where its panel goes (the gauge cluster behind the wheel), facing the
    // driver: centre, normal toward the head, the panel's right and up (model space).
    bool dashOk = false;
    V3 dash, dashN, dashRight, dashUp;
    std::vector<uint64_t> gauges;                   // the car's own gauges (TriId): drawn black with the dashboard
    std::vector<int> kept, turned;                  // per entry: triangles before the wheel, wheel triangles
    std::vector<uint8_t> parts;                     // per entry: the entry without the wheel, the wheel alone
};
// Side mirror glass that shares a draw entry with the rest of the body (BMW M3 GTR): in each such
// entry its triangles are moved to the end, the other triangles first, then the left glass, then
// the right, and the entry is submitted as up to three parts so the glass has draws of its own.
struct GlassSplit {
    struct Entry { int index; int count[3]; };   // other, left glass, right glass triangles
    std::vector<Entry> entries;
    std::vector<uint8_t> parts;                  // 3 entries per split entry
};
struct HeadPos { float v[3]; };
std::unordered_map<std::string, HeadPos> s_carHead; // driver's head per car (model space)

// A triangle by its three vertex indices, sorted: the same whatever order the index buffer has it
// in (the cockpit reorders draw entries) and whichever way round it is wound.
uint64_t TriId(uint16_t a, uint16_t b, uint16_t c) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | ((uint64_t)b << 16) | c;
}
struct TriRef {
    V3 p[3];                                        // model space
    uint16_t v[3];                                  // as wound in the index buffer
    int entry;                                      // draw entry
    uint64_t id;
    bool hidden = false;                            // made zero-area in the index buffer (still pickable)
};

// The triangle editor's changes to one solid (by name, e.g. IS300_KIT00_INTERIOR_A), saved in
// nfsmw_vr_edits.ini: hidden (made degenerate in the index buffer), drawn black, added to or taken
// off the steering wheel, and kept out of the automatic gauge blackout.
struct EditSet {
    std::map<uint64_t, std::array<uint16_t, 3>> hide;   // with the original winding, to put it back
    std::set<uint64_t> black, wheel, noWheel, noAuto, dash;   // dash: added to the dashboard blackout
    bool loaded = false;
};
std::unordered_map<std::string, EditSet> s_edits;

const char* EditsPath() {
    static char path[MAX_PATH];
    if (!path[0]) ConfigFile("nfsmw_vr_edits.ini", path, false);
    return path;
}

// "a.b.c,a.b.c,..." <-> triangles (vertex indices, as wound).
std::vector<std::array<uint16_t, 3>> ParseTris(const char* s) {
    std::vector<std::array<uint16_t, 3>> out;
    unsigned a, b, c;
    int n;
    while (*s && sscanf(s, "%u.%u.%u%n", &a, &b, &c, &n) == 3) {
        out.push_back({(uint16_t)a, (uint16_t)b, (uint16_t)c});
        s += n;
        while (*s == ',' || *s == ' ') s++;
    }
    return out;
}

std::string TrisText(const std::vector<std::array<uint16_t, 3>>& tris) {
    std::string s;
    char buf[32];
    for (const auto& t : tris) {
        snprintf(buf, sizeof(buf), "%s%u.%u.%u", s.empty() ? "" : ",", t[0], t[1], t[2]);
        s += buf;
    }
    return s;
}

std::array<uint16_t, 3> IdTri(uint64_t id) { return {(uint16_t)(id >> 32), (uint16_t)(id >> 16), (uint16_t)id}; }

EditSet& EditsFor(const std::string& solidName) {
    // The file is read once, whole: every model the game streams in (scenery too) asks here.
    static bool known;
    static std::set<std::string> sections;
    if (!known) {
        known = true;
        std::vector<char> names(1 << 16);
        DWORD n = GetPrivateProfileSectionNamesA(names.data(), (DWORD)names.size(), EditsPath());
        for (const char* p = names.data(); p < names.data() + n && *p; p += strlen(p) + 1) sections.insert(p);
    }
    EditSet& e = s_edits[solidName];
    if (e.loaded) return e;
    e.loaded = true;
    if (!sections.count(solidName)) return e;
    static std::vector<char> buf(1 << 17);
    auto read = [&](const char* key) {
        buf[0] = 0;
        GetPrivateProfileStringA(solidName.c_str(), key, "", buf.data(), (DWORD)buf.size(), EditsPath());
        return ParseTris(buf.data());
    };
    for (auto& t : read("Hide")) e.hide[TriId(t[0], t[1], t[2])] = t;
    std::set<uint64_t>* sets[5] = {&e.black, &e.wheel, &e.noWheel, &e.noAuto, &e.dash};
    const char* keys[5] = {"Black", "Wheel", "NoWheel", "NoAuto", "Dash"};
    for (int k = 0; k < 5; k++)
        for (auto& t : read(keys[k])) sets[k]->insert(TriId(t[0], t[1], t[2]));
    size_t n = e.hide.size() + e.black.size() + e.wheel.size() + e.noWheel.size() + e.noAuto.size() + e.dash.size();
    if (n) Log("editor: %s: %u edited triangles loaded", solidName.c_str(), (unsigned)n);
    return e;
}

void SaveEdits(const std::string& solidName) {
    const EditSet& e = EditsFor(solidName);
    std::vector<std::array<uint16_t, 3>> hide;
    for (auto& h : e.hide) hide.push_back(h.second);
    const std::set<uint64_t>* sets[5] = {&e.black, &e.wheel, &e.noWheel, &e.noAuto, &e.dash};
    const char* keys[5] = {"Black", "Wheel", "NoWheel", "NoAuto", "Dash"};
    WritePrivateProfileStringA(solidName.c_str(), "Hide", hide.empty() ? nullptr : TrisText(hide).c_str(), EditsPath());
    for (int k = 0; k < 5; k++) {
        std::vector<std::array<uint16_t, 3>> v;
        for (uint64_t id : *sets[k]) v.push_back(IdTri(id));
        WritePrivateProfileStringA(solidName.c_str(), keys[k], v.empty() ? nullptr : TrisText(v).c_str(), EditsPath());
    }
}

// Solids are streamed: a traffic car's or a reloaded car's model can be freed and another loaded
// at the same address. Each cache entry remembers what the solid looked like when it was seen, and
// is prepared again when that changed. Drawing a freed model's buffers through a stale entry made
// garbage polygons in the cockpit and corrupted d3d9 until it crashed releasing a buffer.
struct SolidInfo {
    Driver* driver = nullptr;                       // nullptr = not a driver
    Wheel* wheel = nullptr;                         // interiors, once the car's driver head is known
    GlassSplit* split = nullptr;                    // bodies whose mirror glass shares draw entries
    bool interior = false;
    int mirrorKind = -1;                            // -1 not looked at, 0 none, 1 side mirror solid, 2 body, 3 interior
    MirrorGlass glass[3];                           // left, right, rear view (model space)
    void* glassIb[2] = {};                          // the side glass's registered draw entries (first indices)
    std::vector<int> glassFirsts[2];
    char name[64] = {};
    uint8_t* plat = nullptr;
    uint8_t* entries = nullptr;
    void* ib = nullptr;
    uint16_t* indices = nullptr;
    uint32_t indexHash = 0;                         // of the game's index copy, after reordering
    // Triangle editor: the solid's triangles (built when needed, again after its indices changed),
    // the hidden ones as applied to the index buffer (id -> winding), and what is drawn black.
    std::vector<TriRef> tris;
    bool trisDirty = true;
    std::map<uint64_t, std::array<uint16_t, 3>> hidden;
    int hiddenDash = -1;                            // the dashboard's state (on / off) the hides were applied for
    std::vector<V3> black;                          // model space, 3 points per triangle
    bool blackDirty = true;
};
std::unordered_map<uint8_t*, SolidInfo> s_solids;
std::vector<Driver*> s_retired;                     // replaced this frame; queued draws may still use them
std::vector<Wheel*> s_retiredWheels;
std::vector<GlassSplit*> s_retiredSplits;

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

// The HUD panel and the in-car dashboard per car (HudPerCar), like the seat: entering a car loads
// its own values if it has them (else the last used stay); changes made in a car are saved for it
// once they have settled for a second. [Hud]: size, distance, height, right; [Dash]: right, up, zoom.
struct PerCar {
    const char* section;
    const char* what;
    int n;
    float* v[20];
    float lo[20], hi[20];                           // accepted when loading
    int least;                                      // values a line needs (older lines lack the last ones)
    bool nanOk;                                     // NaN accepted (automatic placement)
    bool always = false;                            // per car whatever HudPerCar says (like the seat)
    std::string car;
    float seen[20] = {};
    bool dirty = false;
    double changed = 0;
};
PerCar s_hudCar = {"Hud", "HUD", 4, {&g_cfg.hudWidth, &g_cfg.hudDistance, &g_cfg.hudHeight, &g_cfg.hudRight},
                   {0.1f, 0.1f, -5, -5}, {10, 10, 5, 5}, 3, false};
PerCar s_dashCar = {"Dash", "dashboard", 7,
                    {&g_cfg.dashRight, &g_cfg.dashUp, &g_cfg.dashZoom, &g_cfg.dashCover, &g_cfg.dashDepth,
                     &g_cfg.dashScreenW, &g_cfg.dashScreenH},
                    {-1, -1, 0.2f, 1, -0.2f, 0.02f, 0.02f}, {1, 1, 4, 4, 0.3f, 1, 1}, 3, false, true};
// Each dashboard element's x, y and size (Config::dashEl), in that order per element.
PerCar s_dashLayoutCar = {"DashLayout", "dashboard layout", 18,
                          {&g_cfg.dashEl[0][0], &g_cfg.dashEl[0][1], &g_cfg.dashEl[0][2], &g_cfg.dashEl[1][0],
                           &g_cfg.dashEl[1][1], &g_cfg.dashEl[1][2], &g_cfg.dashEl[2][0], &g_cfg.dashEl[2][1],
                           &g_cfg.dashEl[2][2], &g_cfg.dashEl[3][0], &g_cfg.dashEl[3][1], &g_cfg.dashEl[3][2],
                           &g_cfg.dashEl[4][0], &g_cfg.dashEl[4][1], &g_cfg.dashEl[4][2], &g_cfg.dashEl[5][0],
                           &g_cfg.dashEl[5][1], &g_cfg.dashEl[5][2]},
                          {-1, -1, 0, -1, -1, 0, -1, -1, 0, -1, -1, 0, -1, -1, 0, -1, -1, 0},
                          {1, 1, 5, 1, 1, 5, 1, 1, 5, 1, 1, 5, 1, 1, 5, 1, 1, 5}, 15, true, true};

PerCar s_mirrorCar = {"MirrorOffset", "rear view mirror offset", 1, {&g_cfg.mirrorOffset}, {-0.1f}, {0.1f}, 1, false, true};
// The steering wheel's turning centre moved (right, up; Config::wheelPivot).
PerCar s_pivotCar = {"WheelPivot", "steering wheel turning centre", 2, {&g_cfg.wheelPivot[0], &g_cfg.wheelPivot[1]},
                     {-0.1f, -0.1f}, {0.1f, 0.1f}, 2, false, true};

void SavePerCar(PerCar& p) {
    char buf[512];
    int len = 0;
    for (int i = 0; i < p.n && len < (int)sizeof(buf) - 16; i++)
        len += snprintf(buf + len, sizeof(buf) - len, i ? ",%.4f" : "%.4f", *p.v[i]);
    WritePrivateProfileStringA(p.section, p.car.c_str(), buf, IniPath());
    p.dirty = false;
    Log("cockpit: %s for %s saved: %s", p.what, p.car.c_str(), buf);
}

void UpdatePerCar(PerCar& p, const std::string& car) {
    if ((g_cfg.hudPerCar < 0.5f && !p.always) || car.empty()) {
        if (p.dirty && !p.car.empty()) SavePerCar(p);
        p.car.clear();
        return;
    }
    if (car != p.car) {
        if (p.dirty && !p.car.empty()) SavePerCar(p);
        p.car = car;
        char buf[512];
        GetPrivateProfileStringA(p.section, car.c_str(), "", buf, sizeof(buf), IniPath());
        // Lines saved before a value existed lack it (older [Hud]: no "right", 0; [Dash]: no cover,
        // the current one stays).
        float x[20] = {};
        int got = 0;
        for (char* s = buf; *s && got < p.n;) {
            char* end;
            x[got] = strtof(s, &end);
            if (end == s) break;
            got++;
            s = end;
            while (*s == ',' || *s == ' ') s++;
        }
        for (int i = got; i < p.n; i++) x[i] = p.section[0] == 'H' && p.section[1] == 'u' ? 0.0f : *p.v[i];
        bool ok = got >= p.least;
        for (int i = 0; i < p.n && ok; i++) ok = (p.nanOk && x[i] != x[i]) || (x[i] >= p.lo[i] && x[i] <= p.hi[i]);
        if (ok) {
            for (int i = 0; i < p.n; i++) *p.v[i] = x[i];
            Log("cockpit: %s for %s: %s", p.what, car.c_str(), buf);
        } else {
            Log("cockpit: %s for %s: none saved yet, the last used stays", p.what, car.c_str());
        }
        for (int i = 0; i < p.n; i++) p.seen[i] = *p.v[i];
        return;
    }
    bool moved = false;
    for (int i = 0; i < p.n; i++) {
        float a = *p.v[i], b = p.seen[i];
        if ((a == a) != (b == b) || (a == a && fabsf(a - b) > 1e-4f)) moved = true;
        p.seen[i] = a;
    }
    if (moved) {
        p.dirty = true;
        p.changed = Now();
    } else if (p.dirty && Now() - p.changed > 1.0) {
        SavePerCar(p);
    }
}

void UpdateHudCar(const std::string& car) {
    UpdatePerCar(s_hudCar, car);
    UpdatePerCar(s_dashCar, car);
    UpdatePerCar(s_dashLayoutCar, car);
    UpdatePerCar(s_mirrorCar, car);
    if (car != s_pivotCar.car && !car.empty()) {   // a car with none saved: none
        if (s_pivotCar.dirty && !s_pivotCar.car.empty()) SavePerCar(s_pivotCar);
        g_cfg.wheelPivot[0] = g_cfg.wheelPivot[1] = 0;
    }
    UpdatePerCar(s_pivotCar, car);
}

// Arms and hands: in front of the head, above the lap, and out to the side (the chest is in the
// middle). Checked against every car's driver model offline.
constexpr float kArmsForward = 0.10f, kArmsBelowHead = 0.48f, kArmsSideways = 0.06f;   // 0.48: hands low on the wheel in low cars (Gallardo)

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
        // Some drivers split the hands into a draw entry of their own (a second DRIVER_PLAYER
        // material): an entry that is mostly arms is all arms, so no stray hand pieces stay.
        if (!arms.empty() && arms.size() * 4 >= (body.size() + arms.size()) * 3) {
            arms.insert(arms.begin(), body.begin(), body.end());
            body.clear();
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

// The in-car dashboard's place: from the driver's head over the top half of the wheel (its centre
// plus 0.55 of the radius, up in the wheel's plane) to the first surface of the interior past the
// wheel (the gauge cluster), facing the driver. Without a hit, a point behind the top of the wheel.
// The panel's plane stands 3 cm off that surface toward the driver, clear of the cluster's curves;
// the wheel is 15 cm or more in front of it.
// The car's own gauges, drawn black with the dashboard: the triangles of the draw entry the line
// hit, around the hit (22 cm to each side, 10 down, 12 up, 6 cm in front to 10 behind), facing the
// driver. The triangle editor corrects them (NoAuto, Black).
constexpr float kDashLift = 0.55f, kDashReach = 0.6f, kDashStandoff = 0.03f;
using mirror::Sub;
using mirror::Cross;
using mirror::Dot;
using mirror::Norm;

bool RayTriangle(V3 o, V3 d, V3 a, V3 b, V3 c, float* t) {
    V3 e1 = Sub(b, a), e2 = Sub(c, a), p = Cross(d, e2);
    float det = Dot(e1, p);
    if (fabsf(det) < 1e-9f) return false;
    float inv = 1.0f / det;
    V3 s = Sub(o, a);
    float u = Dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    V3 q = Cross(s, e1);
    float v = Dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    *t = Dot(e2, q) * inv;
    return true;
}

void FindDash(Wheel& w, const std::vector<TriRef>& tris, const float headPos[3], const char* name) {
    V3 head = {headPos[0], headPos[1], headPos[2]}, c = {w.centre[0], w.centre[1], w.centre[2]};
    V3 k = Norm({w.axis[0], w.axis[1], w.axis[2]}), z = {0, 0, 1};
    V3 up = Norm(Sub(z, k * Dot(z, k)));
    V3 target = c + up * (w.radius * kDashLift);
    V3 to = Sub(target, head);
    float past = sqrtf(Dot(to, to)) + 0.03f;
    V3 d = Norm(to);
    float best = 1e9f;
    int bestTri = -1;
    for (size_t i = 0; i < tris.size(); i++) {
        float t;
        if (RayTriangle(head, d, tris[i].p[0], tris[i].p[1], tris[i].p[2], &t) && t > past && t < past + kDashReach && t < best) {
            best = t;
            bestTri = (int)i;
        }
    }
    V3 hit = bestTri >= 0 ? head + d * best : c + k * 0.12f + up * (w.radius * 0.7f);
    V3 n = d * -1.0f;
    V3 pu = Norm(Sub(z, n * Dot(z, n)));
    V3 right = Norm(Cross(d, pu));
    w.dashN = n;
    w.dashUp = pu;
    w.dashRight = right;
    w.dash = hit - d * kDashStandoff;
    w.dashOk = true;
    w.gauges.clear();
    if (bestTri >= 0) {
        int entry = tris[bestTri].entry;
        for (const TriRef& t : tris) {
            if (t.entry != entry) continue;
            V3 centre = (t.p[0] + t.p[1] + t.p[2]) * (1.0f / 3);
            V3 rel = Sub(centre, hit);
            float u = Dot(rel, right), v = Dot(rel, pu), depth = Dot(rel, d);
            if (fabsf(u) > 0.22f || v < -0.10f || v > 0.12f || depth < -0.06f || depth > 0.10f) continue;
            V3 normal = Norm(Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0])));
            if (fabsf(Dot(normal, d)) < 0.3f) continue;
            w.gauges.push_back(t.id);
        }
    }
    Log("cockpit: %s: dashboard panel at %.2f %.2f %.2f (%s, %.2f m past the wheel), %u gauge triangles (draw entry %d)",
        name, w.dash.x, w.dash.y, w.dash.z, bestTri >= 0 ? "gauge cluster" : "no surface found, behind the wheel",
        bestTri >= 0 ? best - past + 0.03f : 0.0f, (unsigned)w.gauges.size(), bestTri >= 0 ? tris[bestTri].entry : -1);
}

// The panel's plane in the world: origin (with the offsets from F4, dashRight / dashUp / dashDepth,
// per car with HudPerCar), right, up and toward the driver (unit).
void DashBasis(const Wheel& w, const float* m, float origin[3], float right[3], float up[3], float toEye[3]) {
    V3 o = w.dash + w.dashRight * g_cfg.dashRight + w.dashUp * g_cfg.dashUp + w.dashN * g_cfg.dashDepth;
    for (int a = 0; a < 3; a++) origin[a] = o.x * m[a] + o.y * m[4 + a] + o.z * m[8 + a] + m[12 + a];
    auto dir = [&](V3 v, float* out) {
        V3 r = {v.x * m[0] + v.y * m[4] + v.z * m[8], v.x * m[1] + v.y * m[5] + v.z * m[9], v.x * m[2] + v.y * m[6] + v.z * m[10]};
        r = Norm(r);
        out[0] = r.x; out[1] = r.y; out[2] = r.z;
    };
    dir(w.dashRight, right);
    dir(w.dashUp, up);
    dir(w.dashN, toEye);
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
    std::vector<TriRef> rest;                       // the interior's other triangles
    const EditSet& edits = EditsFor(name);          // triangles put on or taken off the wheel by hand
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
            bool valid = tri[0] < numVerts && tri[1] < numVerts && tri[2] < numVerts;
            bool on = valid;
            for (int k = 0; k < 3 && on; k++) on = OnWheel(*w, pos(tri[k]));
            uint64_t id = valid ? TriId(tri[0], tri[1], tri[2]) : 0;
            if (valid && edits.wheel.count(id)) on = true;
            else if (valid && edits.noWheel.count(id)) on = false;
            (on ? wheel : keep).insert((on ? wheel : keep).end(), tri, tri + 3);
            if (!on && valid && tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]) rest.push_back({{pts[tri[0]], pts[tri[1]], pts[tri[2]]}, {tri[0], tri[1], tri[2]}, i, id});
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
    FindDash(*w, rest, head, name);
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
    // About the found centre moved by the car's pivot offset (right = -y, up in the wheel's plane).
    float upv[3] = {-k[2], 0, k[0]};
    float ul = sqrtf(upv[0] * upv[0] + upv[2] * upv[2]);
    if (ul > 1e-4f) upv[0] /= ul, upv[2] /= ul;
    float pivot[3] = {w.centre[0] + upv[0] * g_cfg.wheelPivot[1], w.centre[1] - g_cfg.wheelPivot[0],
                      w.centre[2] + upv[2] * g_cfg.wheelPivot[1]};
    for (int col = 0; col < 3; col++) {
        float cr = 0;
        for (int r = 0; r < 3; r++) cr += pivot[r] * T[r][col];
        T[3][col] = pivot[col] - cr;
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

void ApplyHides(uint8_t* solid, SolidInfo& info);
void RestoreAll(uint8_t* solid, SolidInfo& info);

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
        if (same && ((was.driver && was.driver->ok) || (was.wheel && was.wheel->ok) || was.split))
            same = CurrentHash(now) == was.indexHash;
        if (same) {
            // An interior seen before its car's driver: look for the wheel once the head is known.
            if (was.interior && !was.wheel && g_cfg.steeringWheel) {
                if (const HeadPos* head = CarHead(name)) {
                    RestoreAll(solid, was);
                    was.wheel = PrepareWheel(solid, name, head->v);
                    was.indexHash = CurrentHash(now);
                    ApplyHides(solid, was);
                }
            }
            return &was;
        }
        bool sameModel = !memcmp(was.name, now.name, sizeof(now.name)) && was.indices == now.indices && was.plat == now.plat;
        if (was.driver || was.wheel)
            Log("cockpit: %.64s changed or reloaded (%s), preparing it again", was.name,
                sameModel ? "its index order changed" : "another model at its address");
        if (was.driver) s_retired.push_back(was.driver);
        if (was.wheel) s_retiredWheels.push_back(was.wheel);
        if (was.split) s_retiredSplits.push_back(was.split);
        // Our zero-area triangles are put back first: the wheel and the gauges are found from whole
        // triangles (prepared over them, the gauges were lost: missing, and not pickable).
        if (sameModel && !was.hidden.empty()) {
            now.hidden = was.hidden;
            RestoreAll(solid, now);
        }
    }
    now.driver = strstr(name, "_DRIVER") ? Prepare(solid, name) : nullptr;
    now.interior = strstr(name, "_INTERIOR") != nullptr;
    if (now.interior && g_cfg.steeringWheel)
        if (const HeadPos* head = CarHead(name)) now.wheel = PrepareWheel(solid, name, head->v);
    now.indexHash = CurrentHash(now);
    SolidInfo& stored = s_solids[solid];
    stored = now;
    ApplyHides(solid, stored);
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

// CarRenderInfo::Render skips a car in the player view when the camera is within 4 m of it and,
// lifted 0.5 m, inside its bounding box (bBoundingBoxIsInside(min, max, eye + 0.5 up, 0)): the
// game's way of not drawing your own car around a camera inside it. From the seat the eyes sit at
// the driver's head, so looking down or leaning brings them low enough for the lifted point to
// fall under the roof, and the whole car vanished (IS300, low roof). Then the driver wasn't drawn
// either and the seat dropped to the game camera. While seated the car is always drawn.
constexpr uintptr_t kCallCarCameraInside = 0x74E68C;
constexpr uintptr_t kBoxIsInside = 0x45FDE0;        // int bBoundingBoxIsInside(const bVector3* min, max, point, float), cdecl

int __cdecl hkCarCameraInside(const float* mn, const float* mx, const float* point, float extra) {
    if (s_seatedConn) {
        static bool logged;
        if (!logged) {
            logged = true;
            Log("cockpit: car kept drawn with the eyes inside it (seated)");
        }
        return 0;
    }
    return ((int(__cdecl*)(const float*, const float*, const float*, float))kBoxIsInside)(mn, mx, point, extra);
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

// A car listed in [VR] GameMirrorsSkip (model names such as BMWM3GTRE46, any case): its side
// mirrors stay the game's.
bool SkipGameMirrors(const std::string& car) {
    char list[sizeof(g_cfg.gameMirrorsSkip)];
    strcpy(list, g_cfg.gameMirrorsSkip);
    for (char* tok = strtok(list, ", ;"); tok; tok = strtok(nullptr, ", ;"))
        if (_stricmp(tok, car.c_str()) == 0) return true;
    return false;
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
        // The glass's own coordinates, 0..1 across and down: what part of the rear view it shows
        // is worked out each eye (NoteMirrors), as a mirror would show it.
        const float basis[14] = {g.centre.x, g.centre.y, g.centre.z, g.u.x, g.u.y, g.u.z, g.v.x, g.v.y, g.v.z,
                                 g.halfU, g.halfV, 0.0f, 1.0f, 0};
        info.glassIb[side] = ib;
        info.glassFirsts[side].push_back(first);
        std::vector<uint16_t> used(indices + first, indices + first + tris * 3);
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        D3D_RegisterMirrorGlass(ib, first, tris, basis, used.data(), (int)used.size());
        Log("mirrors: %s: %s glass is draw entry %d (%d triangles)", name, side ? "right" : "left", i, tris);
        // Every entry on the glass: some cars lay a second one (the old glass texture) over it.
    }
    if (info.glassFirsts[side].empty()) Log("mirrors: %s: %s glass has no draw entry of its own", name, side ? "right" : "left");
}

// Both side mirrors' glass in one draw entry of their own (one material, as on the BMW M3 GTR's
// body): that entry gets both, each glass's vertices that side's slice of the rear view
// (SideMirrorWidth) baked in. Without one, every entry touching the glass is logged.
bool RegisterSharedGlass(uint8_t* solid, SolidInfo& info) {
    char name[65] = {};
    memcpy(name, info.name, 64);
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    int n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    uint8_t* entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    uint16_t* indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    int numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    void* ib = plat ? At<void*>(plat, kPlatIndexBuffer) : nullptr;
    if (n <= 0 || n > 64 || !entries || !indices || !ib) return false;
    uint8_t* verts = nullptr;
    int stride = 0;
    for (int i = 0; i < n && !verts; i++) {
        uint8_t* e = entries + i * kEntrySize;
        verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        stride = fmt ? At<int>(fmt, 8) : 0;
    }
    if (!verts || stride < 12) return false;
    auto onGlass = [&](int side, const uint16_t* tri) {
        const MirrorGlass& g = info.glass[side];
        float d = mirror::Dot(g.centre, g.normal), cu = mirror::Dot(g.centre, g.u), cv = mirror::Dot(g.centre, g.v);
        for (int k = 0; k < 3; k++) {
            const float* p = (const float*)(verts + tri[k] * stride);
            V3 q = {p[0], p[1], p[2]};
            if (!(fabsf(mirror::Dot(q, g.normal) - d) < 0.012f && fabsf(mirror::Dot(q, g.u) - cu) < g.halfU + 0.01f &&
                  fabsf(mirror::Dot(q, g.v) - cv) < g.halfV + 0.01f))
                return false;
        }
        return true;
    };
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        if (tris < 2 || first < 0 || first + tris * 3 > numIndices) continue;
        std::vector<uint16_t> used[2];
        int on[2] = {0, 0};
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = indices + first + t * 3;
            for (int side = 0; side < 2; side++)
                if (onGlass(side, tri)) {
                    on[side]++;
                    used[side].insert(used[side].end(), tri, tri + 3);
                    break;
                }
        }
        if (on[0] + on[1] > 0)
            Log("mirrors: %s: draw entry %d (%d triangles) has %d left and %d right glass triangles", name, i, tris, on[0], on[1]);
        if (!on[0] || !on[1] || (on[0] + on[1]) * 10 < tris * 8) continue;
        float w = g_cfg.sideMirrorWidth;
        for (int side = 0; side < 2; side++) {
            const MirrorGlass& g = info.glass[side];
            float u0 = side == 0 ? 0.0f : 1.0f - w, u1 = u0 + w;
            if (g_cfg.mirrorFlip) {
                float a = 1.0f - u0, c = 1.0f - u1;
                u0 = a;
                u1 = c;
            }
            const float basis[14] = {g.centre.x, g.centre.y, g.centre.z, g.u.x, g.u.y, g.u.z, g.v.x, g.v.y, g.v.z,
                                     g.halfU, g.halfV, u0, u1, 0};
            std::sort(used[side].begin(), used[side].end());
            used[side].erase(std::unique(used[side].begin(), used[side].end()), used[side].end());
            if (side == 0) D3D_RegisterMirrorGlass(ib, first, tris, basis, used[side].data(), (int)used[side].size());
            else D3D_AddMirrorGlassPart(ib, first, basis, used[side].data(), (int)used[side].size());
        }
        Log("mirrors: %s: both side mirrors' glass is draw entry %d (%d triangles)", name, i, tris);
        found = true;
    }
    return found;
}

// Glass without an entry of its own: split out of the entries it shares (GlassSplit).
void SplitSharedGlass(uint8_t* solid, SolidInfo& info, const bool need[2]) {
    char name[65] = {};
    memcpy(name, info.name, 64);
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    int n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    uint8_t* entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    uint16_t* indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    int numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    auto* ib = plat ? At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) : nullptr;
    if (n <= 0 || n > 64 || !entries || !indices || !ib || info.split) return;
    uint8_t* verts = nullptr;
    int stride = 0;
    for (int i = 0; i < n && !verts; i++) {
        uint8_t* e = entries + i * kEntrySize;
        verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        stride = fmt ? At<int>(fmt, 8) : 0;
    }
    D3DINDEXBUFFER_DESC desc;
    if (!verts || stride < 12 || FAILED(ib->GetDesc(&desc)) || desc.Format != D3DFMT_INDEX16) return;
    auto onGlass = [&](int side, const uint16_t* tri) {
        const MirrorGlass& g = info.glass[side];
        float d = mirror::Dot(g.centre, g.normal), cu = mirror::Dot(g.centre, g.u), cv = mirror::Dot(g.centre, g.v);
        for (int k = 0; k < 3; k++) {
            const float* p = (const float*)(verts + tri[k] * stride);
            V3 q = {p[0], p[1], p[2]};
            if (!(fabsf(mirror::Dot(q, g.normal) - d) < 0.012f && fabsf(mirror::Dot(q, g.u) - cu) < g.halfU + 0.01f &&
                  fabsf(mirror::Dot(q, g.v) - cv) < g.halfV + 0.01f))
                return false;
        }
        return true;
    };
    GlassSplit* split = new GlassSplit;
    for (int i = 0; i < n; i++) {
        uint8_t* e = entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        if (tris < 2 || first < 0 || first + tris * 3 > numIndices) continue;
        std::vector<uint16_t> part[3];
        const uint16_t* idx = indices + first;
        for (int t = 0; t < tris; t++) {
            const uint16_t* tri = idx + t * 3;
            int cls = 0;
            for (int side = 0; side < 2 && !cls; side++)
                if (need[side] && info.glass[side].ok && onGlass(side, tri)) cls = side + 1;
            part[cls].insert(part[cls].end(), tri, tri + 3);
        }
        // A few stray triangles are not a mirror's glass.
        for (int side = 1; side < 3; side++)
            if (part[side].size() < 6) {
                part[0].insert(part[0].end(), part[side].begin(), part[side].end());
                part[side].clear();
            }
        if (part[1].empty() && part[2].empty()) continue;
        std::vector<uint16_t> order = part[0];
        order.insert(order.end(), part[1].begin(), part[1].end());
        order.insert(order.end(), part[2].begin(), part[2].end());
        void* p = nullptr;
        if (FAILED(ib->Lock(first * 2, tris * 6, &p, 0)) || !p) {
            Log("mirrors: %s: index buffer lock failed", name);
            continue;
        }
        memcpy(p, order.data(), tris * 6);
        ib->Unlock();
        memcpy(indices + first, order.data(), tris * 6);
        GlassSplit::Entry se = {i, {(int)part[0].size() / 3, (int)part[1].size() / 3, (int)part[2].size() / 3}};
        split->entries.push_back(se);
        // Vertices the glass shares with the rest get the glass's coordinates too (noted).
        std::vector<uint16_t> rest(part[0]);
        std::sort(rest.begin(), rest.end());
        int offset = se.count[0];
        for (int side = 0; side < 2; side++) {
            const std::vector<uint16_t>& tri = part[side + 1];
            if (tri.empty()) continue;
            std::vector<uint16_t> used(tri);
            std::sort(used.begin(), used.end());
            used.erase(std::unique(used.begin(), used.end()), used.end());
            int shared = 0;
            for (uint16_t v : used) shared += std::binary_search(rest.begin(), rest.end(), v);
            const MirrorGlass& g = info.glass[side];
            const float basis[14] = {g.centre.x, g.centre.y, g.centre.z, g.u.x, g.u.y, g.u.z, g.v.x, g.v.y, g.v.z,
                                     g.halfU, g.halfV, 0.0f, 1.0f, 0};
            D3D_RegisterMirrorGlass(ib, first + offset * 3, (int)tri.size() / 3, basis, used.data(), (int)used.size());
            info.glassIb[side] = ib;
            info.glassFirsts[side].push_back(first + offset * 3);
            Log("mirrors: %s: %s glass split out of draw entry %d (%d of %d triangles, %d vertices shared)", name,
                side ? "right" : "left", i, (int)tri.size() / 3, tris, shared);
            offset += (int)tri.size() / 3;
        }
    }
    if (split->entries.empty()) {
        delete split;
        return;
    }
    split->parts.assign(split->entries.size() * 3 * kEntrySize, 0);
    info.split = split;
    info.indexHash = CurrentHash(info);
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
        // Mirrors built into the body sit beside the driver: from 0.2 m behind to 1 m ahead of the
        // head. Further ahead are hood vents (the Viper's KIT04 body). Looked at once the head is known.
        const HeadPos* head = CarHead(name);
        if (!head) {
            info.mirrorKind = -1;
            return;
        }
        float hx = head->v[0];
        for (int side = 0; side < 2; side++) {
            std::vector<V3> tris;
            float s = side == 0 ? 1.0f : -1.0f;
            SolidTriangles(solid, tris, [s, hx](const V3* p) {
                if (!mirror::InBodyMirrorRegion(p, s)) return false;
                for (int k = 0; k < 3; k++)
                    if (p[k].x < hx - 0.2f || p[k].x > hx + 1.0f) return false;
                return true;
            });
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
        if (info.glass[side].ok && g_cfg.gameMirrors && !SkipGameMirrors(CarOf(name))) RegisterGlassEntry(solid, info, side);
    if (g_cfg.gameMirrors && !SkipGameMirrors(CarOf(name)) && info.glass[0].ok && info.glass[1].ok &&
        info.glassFirsts[0].empty() && info.glassFirsts[1].empty())
        if (!RegisterSharedGlass(solid, info)) {
            // Mixed into the body's entries: split out of them.
            const bool need[2] = {true, true};
            SplitSharedGlass(solid, info, need);
        }
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

// A side mirror: a fixed slice of the rear view, [VR] SideMirrorWidth of its width from that side,
// its full height (the left mirror shows the left part).
void SideMirrorView(const SolidInfo& info, int side, const float*) {
    float w = g_cfg.sideMirrorWidth;
    float u0 = side == 0 ? 0.0f : 1.0f - w, u1 = u0 + w;
    if (g_cfg.mirrorFlip) {
        float a = 1.0f - u1, c = 1.0f - u0;
        u0 = a;
        u1 = c;
        std::swap(u0, u1);
    }
    const float uv[8] = {u0, 0.0f, u1, 0.0f, u0, 1.0f, u1, 1.0f};
    for (int first : info.glassFirsts[side]) D3D_SetGlassCorners(info.glassIb[side], first, uv);
}

void NoteMirrors(uint8_t* solid, SolidInfo& info, const float* m) {
    if (info.mirrorKind < 0) FindMirrors(solid, info);
    if (info.mirrorKind <= 0) return;
    for (int side = 0; side < 2; side++)
        if (!info.glassFirsts[side].empty() && info.glass[side].ok) SideMirrorView(info, side, m);
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
        // MirrorOffset (per car, F4 > Menu and testing) moves it toward the driver or back into the glass.
        GlassVertices(d.glass, d.m, frame, 0.006f + g_cfg.mirrorOffset, 1.0f);
        GlassVertices(d.glass, d.m, verts, 0.010f + g_cfg.mirrorOffset, inner);
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

// --- triangle editor -------------------------------------------------------------------------------
// F4 > VR > Triangle editor. Picking ("Pick triangles": the menu hides): every triangle of the player
// car on the line from between the eyes, straight ahead, is a candidate; the menu's up / down step
// through them from the nearest, left / right change the selection's size (one triangle, within
// 2 / 5 / 10 cm, the connected piece, the whole draw entry). A reticle marks the point and the
// selection is shown over everything (also when behind something); the menu key confirms. The
// page's commands then hide the selection, draw it black, put it on or take it off the steering
// wheel (prepared again), add it to or take it off the dashboard blackout, restore it, or undo.
// Hidden and black triangles are made zero-area in the index buffer, (a, b, b); black ones are
// drawn again in black in their place, so the game's own (whose gauge layer ignores the depth
// test) can't show through. Saved per solid in nfsmw_vr_edits.ini.

V3 s_gazePos, s_gazeDir;
bool s_haveGaze = false;
struct Pick {
    uint8_t* solid = nullptr;
    std::string name;
    uint64_t id = 0;
    V3 hit{};                                       // model space
    V3 hitWorld{};
    float dist = 1e9f;
};
std::vector<Pick> s_hits;                           // this frame's candidates along the gaze
Pick s_pick;                                        // the one in use
std::vector<uint64_t> s_brush;                      // s_pick's selection
uint64_t s_brushFor = 0;
int s_brushMode = -1;
struct Undo { std::string name; EditSet set; };
std::vector<Undo> s_undo;
int s_eyeIndex = 0;                                 // eye being rendered (Cockpit_BeginEye counts them)
std::set<uint8_t*> s_eyeDone, s_frameDone;         // solids handled this eye / picked this frame
std::vector<float> s_overlay;                       // this eye's selection, world (drawn over the eye)

bool EditorLive() { return g_cfg.editorOn > 0.5f || g_cfg.editPicking > 0.5f; }

std::string SolidName(const SolidInfo& info) {
    char name[65] = {};
    memcpy(name, info.name, 64);
    return name;
}

// The solid's vertices (first entry with vertex data) and index copy.
struct SolidMesh {
    uint8_t* verts = nullptr;
    int stride = 0, numVerts = 0, numIndices = 0, n = 0;
    uint8_t* entries = nullptr;
    uint16_t* indices = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    const float* pos(int v) const { return (const float*)(verts + v * stride); }
    V3 at(int v) const { const float* p = pos(v); return {p[0], p[1], p[2]}; }
};

bool ReadMesh(uint8_t* solid, SolidMesh& s) {
    uint8_t* plat = At<uint8_t*>(solid, kSolidPlat);
    s.n = plat ? At<int>(plat, kPlatNumEntries) : 0;
    s.entries = plat ? At<uint8_t*>(plat, kPlatEntries) : nullptr;
    s.indices = plat ? At<uint16_t*>(plat, kPlatIndices) : nullptr;
    s.ib = plat ? At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) : nullptr;
    s.numIndices = plat ? At<int>(plat, kPlatNumIndices) : 0;
    if (s.n <= 0 || s.n > 64 || !s.entries || !s.indices || !s.ib || s.numIndices <= 0) return false;
    for (int i = 0; i < s.n && !s.verts; i++) {
        uint8_t* e = s.entries + i * kEntrySize;
        s.verts = At<uint8_t*>(e, kEntryVertices);
        uint8_t* fmt = At<uint8_t*>(e, kEntryFormat);
        s.stride = fmt ? At<int>(fmt, 8) : 0;
        s.numVerts = s.stride > 0 ? At<int>(e, kEntryVertexBytes) / s.stride : 0;
    }
    return s.verts && s.stride >= 12 && s.stride <= 128 && s.numVerts > 0;
}

// The solid's triangles, the hidden ones included (where they were), for picking.
void BuildTris(uint8_t* solid, SolidInfo& info) {
    info.tris.clear();
    info.trisDirty = false;
    info.blackDirty = true;
    SolidMesh s;
    if (!ReadMesh(solid, s)) return;
    for (int i = 0; i < s.n; i++) {
        uint8_t* e = s.entries + i * kEntrySize;
        int tris = At<int>(e, kEntryTris), first = At<int>(e, kEntryFirstIndex);
        if (tris <= 0 || first < 0 || first + tris * 3 > s.numIndices) continue;
        for (int t = 0; t < tris; t++) {
            int p = first + t * 3;
            const uint16_t* tri = s.indices + p;
            if (tri[0] >= s.numVerts || tri[1] >= s.numVerts || tri[2] >= s.numVerts) continue;
            bool empty = tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2];
            uint16_t v[3] = {tri[0], tri[1], tri[2]};
            if (empty) {
                // One of ours, (a, b, b): the triangle it was.
                bool found = false;
                for (auto& h : info.hidden)
                    if (h.second[0] == tri[0] && h.second[1] == tri[1] && tri[2] == tri[1]) {
                        memcpy(v, h.second.data(), 6);
                        found = true;
                        break;
                    }
                if (!found || v[2] >= s.numVerts) continue;
            }
            TriRef r;
            for (int k = 0; k < 3; k++) {
                r.p[k] = s.at(v[k]);
                r.v[k] = v[k];
            }
            r.entry = i;
            r.id = TriId(v[0], v[1], v[2]);
            r.hidden = empty;
            info.tris.push_back(r);
        }
    }
}

bool WriteIndices(const SolidMesh& s, int at, const uint16_t v[3]) {
    void* p = nullptr;
    if (FAILED(s.ib->Lock(at * 2, 6, &p, 0)) || !p) return false;
    memcpy(p, v, 6);
    s.ib->Unlock();
    memcpy(s.indices + at, v, 6);
    return true;
}

bool DashShown() { return g_cfg.dash > 0.5f; }

// The car's own gauges drawn black with the dashboard: found automatically, plus those added by
// hand, less those taken off, drawn black or hidden by hand.
// None any more: the dashboard draws nothing over the car's own triangles (they stay as the game has
// them; the triangle editor's Draw black does that by hand). The old automatic gauges
// (Wheel::gauges) and the [Dash] lists in nfsmw_vr_edits.ini are left unused.
std::set<uint64_t> Gauges(const SolidInfo&, const EditSet&) { return {}; }

// What should be zero-area in the index buffer now: the hidden and the black triangles (black ones
// are drawn again in black in their place: drawn over the game's own, they fought with them, whose
// gauge layer doesn't keep to the depth test); applied by difference (newly hidden made (a, b, b),
// others put back). A model prepared again gets them back first (GetSolid, RestoreAll).
void ApplyHides(uint8_t* solid, SolidInfo& info) {
    EditSet& e = EditsFor(SolidName(info));
    bool dash = DashShown();
    std::set<uint64_t> want;
    for (auto& h : e.hide) want.insert(h.first);
    want.insert(e.black.begin(), e.black.end());
    info.hiddenDash = dash;
    info.blackDirty = true;
    if (want.empty() && info.hidden.empty()) return;
    SolidMesh s;
    if (!ReadMesh(solid, s)) return;
    bool changed = false;
    for (auto it = info.hidden.begin(); it != info.hidden.end();) {
        if (want.count(it->first)) {
            ++it;
            continue;
        }
        const auto& w = it->second;
        for (int p = 0; p + 2 < s.numIndices; p += 3)
            if (s.indices[p] == w[0] && s.indices[p + 1] == w[1] && s.indices[p + 2] == w[1]) {
                changed |= WriteIndices(s, p, w.data());
                break;
            }
        it = info.hidden.erase(it);
    }
    for (uint64_t id : want) {
        if (info.hidden.count(id)) continue;
        for (int p = 0; p + 2 < s.numIndices; p += 3) {
            const uint16_t* t = s.indices + p;
            if (t[0] == t[1] || t[1] == t[2] || TriId(t[0], t[1], t[2]) != id) continue;
            std::array<uint16_t, 3> wound = {t[0], t[1], t[2]};
            uint16_t gone[3] = {t[0], t[1], t[1]};
            if (WriteIndices(s, p, gone)) {
                info.hidden[id] = wound;
                changed = true;
            }
            break;
        }
    }
    if (changed) {
        info.indexHash = CurrentHash(info);
        info.trisDirty = true;
    }
}

// The black triangles, model space, in place of the hidden originals: the editor's, and the gauges
// while the dashboard shows.
void UpdateBlack(uint8_t* solid, SolidInfo& info) {
    if (!info.blackDirty) return;
    info.blackDirty = false;
    info.black.clear();
    const EditSet& e = EditsFor(SolidName(info));
    std::set<uint64_t> ids(e.black.begin(), e.black.end());
    if (DashShown()) {
        std::set<uint64_t> g = Gauges(info, e);
        ids.insert(g.begin(), g.end());
    }
    if (ids.empty()) return;
    SolidMesh s;
    if (!ReadMesh(solid, s)) return;
    for (uint64_t id : ids) {
        std::array<uint16_t, 3> v = IdTri(id);
        if (v[0] >= s.numVerts || v[1] >= s.numVerts || v[2] >= s.numVerts) continue;
        for (int k = 0; k < 3; k++) info.black.push_back(s.at(v[k]));
    }
}

V3 ToWorld(const V3& p, const float* m) {
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

// Model-space triangles to the world for the current eye, each vertex lifted `lift` m toward the
// eyes (last frame's gaze position: the game camera's own is not where the eyes are).
void AddColor(void* ib, const std::vector<V3>& tris, const float* m, float lift, uint32_t argb) {
    if (tris.empty()) return;
    V3 eye = s_gazePos;
    if (!s_haveGaze) {
        void* cam = At<void*>((void*)kPlayerView, kViewCamera);
        if (!cam) return;
        const float* c = &At<float>(cam, kCamPosition);
        eye = {c[0], c[1], c[2]};
    }
    std::vector<float> xyz(tris.size() * 3);
    for (size_t i = 0; i < tris.size(); i++) {
        V3 w = ToWorld(tris[i], m);
        V3 d = Sub(eye, w);
        float len = sqrtf(Dot(d, d));
        float k = len > 1e-4f ? std::min(lift, len * 0.5f) / len : 0.0f;
        xyz[i * 3] = w.x + d.x * k;
        xyz[i * 3 + 1] = w.y + d.y * k;
        xyz[i * 3 + 2] = w.z + d.z * k;
    }
    D3D_AddColorTris(ib, xyz.data(), (int)tris.size() / 3, argb);
}

// The gaze against the solid's triangles (drawn with `m`): every hit is a candidate.
void PickIn(uint8_t* solid, SolidInfo& info, const float* m) {
    if (info.trisDirty) BuildTris(solid, info);
    V3 ax[3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    V3 rel = Sub(s_gazePos, {m[12], m[13], m[14]});
    V3 o, d;
    float* oc[3] = {&o.x, &o.y, &o.z};
    float* dc[3] = {&d.x, &d.y, &d.z};
    for (int i = 0; i < 3; i++) {
        float l2 = Dot(ax[i], ax[i]);
        if (l2 < 1e-8f) return;
        *oc[i] = Dot(rel, ax[i]) / l2;
        *dc[i] = Dot(s_gazeDir, ax[i]) / l2;
    }
    std::string name = SolidName(info);
    for (const TriRef& t : info.tris) {
        float hit;
        if (!RayTriangle(o, d, t.p[0], t.p[1], t.p[2], &hit) || hit < 0.02f || hit > 6.0f) continue;
        Pick p;
        p.solid = solid;
        p.name = name;
        p.id = t.id;
        p.hit = o + d * hit;
        p.hitWorld = s_gazePos + s_gazeDir * hit;   // the map from model to world is affine: t in metres
        p.dist = hit;
        s_hits.push_back(p);
    }
}

// s_pick's selection for the size chosen (Config::editMode).
void UpdateBrush(SolidInfo& info) {
    int mode = std::max(0, std::min(5, (int)lrintf(g_cfg.editMode)));
    if (s_brushFor == s_pick.id && s_brushMode == mode && !info.trisDirty) return;
    s_brushFor = s_pick.id;
    s_brushMode = mode;
    s_brush.assign(1, s_pick.id);
    const TriRef* picked = nullptr;
    for (const TriRef& t : info.tris)
        if (t.id == s_pick.id) picked = &t;
    if (!picked || mode == 0) return;
    if (mode <= 3) {
        const float radius[4] = {0, 0.02f, 0.05f, 0.10f};
        float r = radius[mode];
        for (const TriRef& t : info.tris) {
            V3 dd = Sub((t.p[0] + t.p[1] + t.p[2]) * (1.0f / 3), s_pick.hit);
            if (t.id != s_pick.id && Dot(dd, dd) < r * r) s_brush.push_back(t.id);
        }
    } else if (mode == 4) {
        // The connected piece: triangles sharing vertices, spreading from the one picked.
        std::unordered_map<uint16_t, std::vector<int>> byVertex;
        for (int i = 0; i < (int)info.tris.size(); i++)
            for (int k = 0; k < 3; k++) byVertex[info.tris[i].v[k]].push_back(i);
        std::vector<char> seen(info.tris.size(), 0);
        std::vector<int> todo(1, (int)(picked - info.tris.data()));
        seen[todo[0]] = 1;
        while (!todo.empty()) {
            int i = todo.back();
            todo.pop_back();
            if (info.tris[i].id != s_pick.id) s_brush.push_back(info.tris[i].id);
            for (int k = 0; k < 3; k++)
                for (int j : byVertex[info.tris[i].v[k]])
                    if (!seen[j]) {
                        seen[j] = 1;
                        todo.push_back(j);
                    }
        }
    } else {
        for (const TriRef& t : info.tris)
            if (t.entry == picked->entry && t.id != s_pick.id) s_brush.push_back(t.id);
    }
}

// Every triangle made zero-area put back (the wheel and the gauges are found from whole triangles).
void RestoreAll(uint8_t* solid, SolidInfo& info) {
    SolidMesh s;
    if (info.hidden.empty() || !ReadMesh(solid, s)) return;
    for (auto& h : info.hidden)
        for (int p = 0; p + 2 < s.numIndices; p += 3)
            if (s.indices[p] == h.second[0] && s.indices[p + 1] == h.second[1] && s.indices[p + 2] == h.second[1]) {
                WriteIndices(s, p, h.second.data());
                break;
            }
    info.hidden.clear();
    info.indexHash = CurrentHash(info);
    info.trisDirty = info.blackDirty = true;
}

// Prepares the steering wheel of every loaded solid with that name again (after wheel edits).
void RePrepareWheels(const std::string& name) {
    for (auto& kv : s_solids) {
        SolidInfo& info = kv.second;
        if (!info.interior || SolidName(info) != name) continue;
        const HeadPos* head = CarHead(name.c_str());
        if (!head) continue;
        RestoreAll(kv.first, info);
        if (info.wheel) s_retiredWheels.push_back(info.wheel);
        info.wheel = PrepareWheel(kv.first, name.c_str(), head->v);
        info.indexHash = CurrentHash(info);
        info.trisDirty = info.blackDirty = true;
    }
}

void ApplyEditsTo(const std::string& name, bool wheel) {
    if (wheel) RePrepareWheels(name);
    for (auto& kv : s_solids) {
        if (SolidName(kv.second) != name) continue;
        ApplyHides(kv.first, kv.second);
        kv.second.blackDirty = true;
    }
    s_brushFor = 0;
}

// The current car's steering wheel turning centre fitted to the wheel's triangles as they are now
// (those put on it by hand too), as the pivot offset (Config::wheelPivot, saved per car); which
// triangles are on the wheel is left as it is. The outermost wheel vertex in each of 36 directions
// round the column, a circle through them (least squares; points over 3 cm off the median radius
// dropped), three times from the new centre.
void CentreWheelPivot() {
    std::string car = s_head.valid ? s_head.car : std::string();
    for (auto& kv : s_solids) {
        SolidInfo& info = kv.second;
        std::string name = SolidName(info);
        if (car.empty() || !info.interior || !info.wheel || !info.wheel->ok || name.compare(0, car.size() + 1, car + "_") != 0)
            continue;
        const Wheel& w = *info.wheel;
        SolidMesh s;
        if (!ReadMesh(kv.first, s)) continue;
        std::vector<V3> pts;
        for (int i = 0; i < s.n && i < (int)w.kept.size(); i++) {
            if (!w.turned[i]) continue;
            int first = At<int>(s.entries + i * kEntrySize, kEntryFirstIndex);
            int a = first + w.kept[i] * 3, b = std::min(s.numIndices, first + (w.kept[i] + w.turned[i]) * 3);
            for (int k = std::max(0, a); k < b; k++)
                if (s.indices[k] < s.numVerts) pts.push_back(s.at(s.indices[k]));
        }
        V3 k = Norm({w.axis[0], w.axis[1], w.axis[2]}), e1 = {0, 1, 0};   // the column is in the x-z plane
        V3 e2 = Norm({-k.z, 0, k.x});                                      // up in the wheel's plane
        V3 c0 = {w.centre[0], w.centre[1], w.centre[2]}, c = c0;
        bool fitted = false;
        for (int pass = 0; pass < 3; pass++) {
            float rmax[36], sa[36] = {}, sb[36] = {};
            for (float& r : rmax) r = -1;
            for (const V3& p : pts) {
                V3 rel = Sub(p, c);
                if (fabsf(Dot(rel, k)) > 0.08f) continue;
                float a = Dot(rel, e1), b = Dot(rel, e2), r = sqrtf(a * a + b * b);
                if (r < 0.06f || r > 0.30f) continue;
                int sec = (int)((atan2f(b, a) + 3.14159265f) / (2 * 3.14159265f) * 36) % 36;
                if (r > rmax[sec]) {
                    rmax[sec] = r;
                    sa[sec] = a;
                    sb[sec] = b;
                }
            }
            std::vector<float> radii;
            for (float r : rmax)
                if (r > 0) radii.push_back(r);
            if (radii.size() < 18) break;
            std::nth_element(radii.begin(), radii.begin() + radii.size() / 2, radii.end());
            float med = radii[radii.size() / 2];
            // Circle a^2 + b^2 + D a + E b + F = 0 through the points, least squares (Cramer's rule).
            double m[3][3] = {}, v[3] = {};
            int used = 0;
            for (int i = 0; i < 36; i++) {
                if (rmax[i] < 0 || fabsf(rmax[i] - med) > 0.03f) continue;
                double row[3] = {sa[i], sb[i], 1}, rhs = -(sa[i] * (double)sa[i] + sb[i] * (double)sb[i]);
                for (int r = 0; r < 3; r++) {
                    for (int q = 0; q < 3; q++) m[r][q] += row[r] * row[q];
                    v[r] += row[r] * rhs;
                }
                used++;
            }
            if (used < 12) break;
            auto det = [](double a[3][3]) {
                return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
            };
            double d = det(m);
            if (fabs(d) < 1e-12) break;
            double sol[2];
            for (int col = 0; col < 2; col++) {
                double t[3][3];
                memcpy(t, m, sizeof(t));
                for (int r = 0; r < 3; r++) t[r][col] = v[r];
                sol[col] = det(t) / d;
            }
            c = c + e1 * (float)(-sol[0] / 2) + e2 * (float)(-sol[1] / 2);
            fitted = true;
        }
        V3 moved = Sub(c, c0);
        float right = -Dot(moved, e1), up = Dot(moved, e2);
        if (!fitted || fabsf(right) > 0.1f || fabsf(up) > 0.1f) {
            Log("cockpit: %s: steering wheel turning centre: no fit (%u wheel vertices)", name.c_str(), (unsigned)pts.size());
            return;
        }
        g_cfg.wheelPivot[0] = right;
        g_cfg.wheelPivot[1] = up;
        Log("cockpit: %s: steering wheel turns about a centre %.1f mm right, %.1f mm up of the found one (%u wheel vertices)",
            name.c_str(), right * 1000, up * 1000, (unsigned)pts.size());
        return;
    }
    Log("cockpit: steering wheel turning centre: no steering wheel for %s", car.c_str());
}

// The F4 page's command for the selection (see Config::editCommand).
void RunEditCommand(int cmd) {
    if (cmd == 6) {
        if (s_undo.empty()) {
            Log("editor: nothing to undo");
            return;
        }
        Undo u = s_undo.back();
        s_undo.pop_back();
        EditSet& cur = EditsFor(u.name);
        bool wheel = cur.wheel != u.set.wheel || cur.noWheel != u.set.noWheel;
        cur = u.set;
        cur.loaded = true;
        SaveEdits(u.name);
        ApplyEditsTo(u.name, wheel);
        Log("editor: %s: undone", u.name.c_str());
        return;
    }
    auto it = s_solids.find(s_pick.solid);
    if (!s_pick.solid || it == s_solids.end() || SolidName(it->second) != s_pick.name || s_brush.empty()) {
        Log("editor: nothing picked");
        return;
    }
    SolidInfo& info = it->second;
    EditSet& e = EditsFor(s_pick.name);
    s_undo.push_back({s_pick.name, e});
    if (s_undo.size() > 100) s_undo.erase(s_undo.begin());
    std::map<uint64_t, std::array<uint16_t, 3>> wound;
    for (const TriRef& t : info.tris) wound[t.id] = {t.v[0], t.v[1], t.v[2]};
    bool wheel = false;
    for (uint64_t id : s_brush) {
        switch (cmd) {
            case 1:
                if (wound.count(id)) e.hide[id] = wound[id];
                e.black.erase(id);
                break;
            case 2:
                e.black.insert(id);
                e.hide.erase(id);
                break;
            case 3:
                e.wheel.insert(id);
                e.noWheel.erase(id);
                wheel = true;
                break;
            case 4:
                e.noWheel.insert(id);
                e.wheel.erase(id);
                wheel = true;
                break;
            case 5:
                wheel |= e.wheel.erase(id) + e.noWheel.erase(id) > 0;
                e.hide.erase(id);
                e.black.erase(id);
                e.dash.erase(id);
                e.noAuto.insert(id);
                break;
            case 7:
                e.dash.insert(id);
                e.noAuto.erase(id);
                e.hide.erase(id);
                e.black.erase(id);
                break;
            case 8:
                e.dash.erase(id);
                e.noAuto.insert(id);
                break;
        }
    }
    static const char* kWhat[] = {"", "hidden", "black", "on the steering wheel", "off the steering wheel", "restored", "",
                                  "added to the dashboard blackout", "taken off the dashboard blackout"};
    Log("editor: %s: %u triangles %s", s_pick.name.c_str(), (unsigned)s_brush.size(), kWhat[cmd]);
    SaveEdits(s_pick.name);
    ApplyEditsTo(s_pick.name, wheel);
}

// For each player car solid as it is submitted (m: its matrix): its black triangles, picking and the
// selection. Once per solid per eye.
void NoteEditor(uint8_t* solid, SolidInfo& info, const float* m) {
    if (!s_eyeDone.insert(solid).second) return;
    void* ib = At<void*>(At<uint8_t*>(solid, kSolidPlat), kPlatIndexBuffer);
    if (info.hiddenDash != (int)DashShown()) ApplyHides(solid, info);
    UpdateBlack(solid, info);
    // In place of the originals (zero-area in the index buffer), 2 mm toward the eyes: clear of the
    // gauges' thin decal layers (needles, numbers) a fraction of a millimetre in front, which fought
    // with a copy just above the surface when not picked too.
    AddColor(ib, info.black, m, 0.002f, D3DCOLOR_ARGB(255, 0, 0, 0));
    if (!EditorLive() || !s_haveGaze) return;
    if (g_cfg.editPicking > 0.5f && s_eyeIndex == 0 && s_frameDone.insert(solid).second) PickIn(solid, info, m);
    if (solid != s_pick.solid || SolidName(info) != s_pick.name) return;
    if (info.trisDirty) BuildTris(solid, info);
    UpdateBrush(info);
    g_cfg.editPicked = (float)s_brush.size();
    std::set<uint64_t> brush(s_brush.begin(), s_brush.end());
    for (const TriRef& t : info.tris)
        if (brush.count(t.id))
            for (int k = 0; k < 3; k++) {
                V3 w = ToWorld(t.p[k], m);
                s_overlay.push_back(w.x);
                s_overlay.push_back(w.y);
                s_overlay.push_back(w.z);
            }
}

// After an eye's scene: the selection over everything, and the reticle where the gaze hits (or
// a metre ahead), a small cross facing the eyes.
void DrawEditorOverlay() {
    if (!EditorLive()) {
        s_overlay.clear();
        return;
    }
    if (!s_overlay.empty()) D3D_DrawOverlay(s_overlay.data(), (int)s_overlay.size() / 9, D3DCOLOR_ARGB(110, 255, 200, 0));
    s_overlay.clear();
    if (g_cfg.editPicking < 0.5f || !s_haveGaze) return;
    bool hit = s_pick.solid != nullptr;
    V3 c = hit ? s_pick.hitWorld : s_gazePos + s_gazeDir;
    float dist = sqrtf(Dot(Sub(c, s_gazePos), Sub(c, s_gazePos)));
    float r = std::max(0.002f, dist * 0.008f), t = r * 0.18f;
    V3 up = {0, 0, 1};
    V3 right = Cross(s_gazeDir, up);
    if (Dot(right, right) < 1e-6f) right = {1, 0, 0};
    right = Norm(right);
    up = Norm(Cross(right, s_gazeDir));
    std::vector<float> xyz;
    auto quad = [&](V3 a, V3 b, V3 cc, V3 d) {
        for (const V3& p : {a, b, cc, a, cc, d}) {
            xyz.push_back(p.x);
            xyz.push_back(p.y);
            xyz.push_back(p.z);
        }
    };
    quad(c - right * r + up * t, c + right * r + up * t, c + right * r - up * t, c - right * r - up * t);
    quad(c - right * t + up * r, c + right * t + up * r, c + right * t - up * r, c - right * t - up * r);
    D3D_DrawOverlay(xyz.data(), (int)xyz.size() / 9, hit ? D3DCOLOR_ARGB(255, 255, 60, 0) : D3DCOLOR_ARGB(255, 120, 120, 120));
}

// The seated driver's ranges set by OnSubmit: s_driverParts entries from s_driverEntry.
uint8_t* s_driverEntry;
int s_driverParts;

// Returns the entry to submit (the first of the seated driver's ranges), or null for none.
uint8_t* __cdecl OnSubmit(uint8_t* model, const float* m, uint8_t* entry) {
    uint8_t* solid = model ? At<uint8_t*>(model, kModelSolid) : nullptr;
    SolidInfo* info = solid ? GetSolid(solid) : nullptr;
    if (info && m && g_cfg.mirrors && DrawingPlayerCar()) NoteMirrors(solid, *info, m);
    if (info && m && info->wheel && info->wheel->dashOk && g_cfg.dash > 0.5f && DrawingPlayerCar()) {
        float origin[3], right[3], up[3], toEye[3];
        DashBasis(*info->wheel, m, origin, right, up, toEye);
        D3D_AddDash(At<void*>(At<uint8_t*>(solid, kSolidPlat), kPlatIndexBuffer), origin, right, up, toEye);
    }
    if (info && m && DrawingPlayerCar()) NoteEditor(solid, *info, m);
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
    bool wheelOverHud = g_cfg.wheelOverHud > 0.5f;
    if (w && w->ok && matrix && (fabsf(angle) > 0.05f || wheelOverHud) && DrawingPlayerCar()) {
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
            if (wheelOverHud)
                D3D_RegisterWheelDraw(At<void*>(At<uint8_t*>(solid, kSolidPlat), kPlatIndexBuffer), At<int>(wheel, kEntryFirstIndex));
            submit(wheel, a2, a3, a4, a5, turned, a7, a8, a9);
            return;
        }
    }
    GlassSplit* split = it != s_solids.end() ? it->second.split : nullptr;
    if (split) {
        uint8_t* entries = At<uint8_t*>(At<uint8_t*>(solid, kSolidPlat), kPlatEntries);
        int i = (int)(entry - entries) / kEntrySize;
        for (size_t k = 0; k < split->entries.size(); k++) {
            const GlassSplit::Entry& se = split->entries[k];
            if (se.index != i) continue;
            int first = At<int>(entry, kEntryFirstIndex), offset = 0;
            for (int part = 0; part < 3; part++) {
                int count = se.count[part];
                if (!count) continue;
                uint8_t* pe = &split->parts[(k * 3 + part) * kEntrySize];
                memcpy(pe, entry, kEntrySize);
                At<int>(pe, kEntryFirstIndex) = first + offset * 3;
                At<int>(pe, kEntryTris) = count;
                At<int>(pe, kEntryNumIndices) = count * 3;
                submit(pe, a2, a3, a4, a5, matrix, a7, a8, a9);
                offset += count;
            }
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
    PatchCall(kCallCarCameraInside, kBoxIsInside, (void*)&hkCarCameraInside);
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
    // Triangle editor: while picking, the candidate along the gaze chosen with up / down (layer,
    // nearest first) is the pick; otherwise the last one stays (locked) for the page's commands.
    if (g_cfg.editPicking > 0.5f) {
        std::sort(s_hits.begin(), s_hits.end(), [](const Pick& a, const Pick& b) { return a.dist < b.dist; });
        int n = (int)s_hits.size();
        int layer = std::max(0, std::min(n - 1, (int)lrintf(g_cfg.editLayer)));
        if (n && g_cfg.editLayer > (float)(n - 1)) g_cfg.editLayer = (float)(n - 1);
        Pick next = n ? s_hits[layer] : Pick();
        if (next.solid != s_pick.solid || next.id != s_pick.id) s_brushFor = 0;
        s_pick = next;
        if (!n) g_cfg.editPicked = 0;
    } else if (!EditorLive()) {
        s_pick = Pick();
    }
    s_hits.clear();
    s_frameDone.clear();
    s_eyeIndex = -1;
    int cmd = (int)lrintf(g_cfg.editCommand);
    if (cmd) {
        g_cfg.editCommand = 0;
        if (cmd >= 1 && cmd <= 8) RunEditCommand(cmd);
        if (cmd == 10) CentreWheelPivot();                             // F4 > Menu and testing
        if (cmd == 11) g_cfg.wheelPivot[0] = g_cfg.wheelPivot[1] = 0;
        if (cmd == 9) {   // picking cancelled: no selection
            s_pick = Pick();
            s_brush.clear();
            s_brushFor = 0;
            g_cfg.editPicked = 0;
        }
    }
    for (Driver* d : s_retired) delete d;
    s_retired.clear();
    for (Wheel* w : s_retiredWheels) delete w;
    s_retiredWheels.clear();
    for (GlassSplit* g : s_retiredSplits) delete g;
    s_retiredSplits.clear();
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
    UpdateHudCar(s_head.valid ? s_head.car : std::string());
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

void Cockpit_BeginEye() {
    s_mirrorDraws.clear();
    s_eyeDone.clear();
    s_eyeIndex++;
}

void Cockpit_EndEye() { DrawEditorOverlay(); }

void Cockpit_SetGaze(const float pos[3], const float dir[3]) {
    s_gazePos = {pos[0], pos[1], pos[2]};
    s_gazeDir = Norm({dir[0], dir[1], dir[2]});
    s_haveGaze = Dot(s_gazeDir, s_gazeDir) > 0.5f;
}
