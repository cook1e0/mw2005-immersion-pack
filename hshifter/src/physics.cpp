// Physics sliders for the player's car: the game's driving aids, grip and gravity.
//
// The game models front, rear and all-wheel drive properly (SuspensionRacer::DoDriveForces
// splits the torque by the transmission's TORQUE_SPLIT), but several aids hide the difference:
// - traction control: DoDriveForces cuts torque to a spinning wheel while steering, scaled by
//   Ramp(forward slip, 1, Tweak_TractionFowardSlip);
// - yaw control: rear tires get extra friction while the car rotates, up to MaxYawBonus
//   (TuneWheelParams / YawFrictionBoost);
// - drift stabiliser: DoDrifting damps rotation by DriftStabilizerTable;
// - drift assist: DoDrifting lowers rear grip in a slide by DriftRearFrictionTable.
// These, the grip (Tweak_GlobalGripScale) and the torque split (the transmission's TORQUE_SPLIT,
// the front wheels' share) are tuning data read while a car's suspension simulates. SuspensionRacer::OnTaskSimulate is wrapped: for the
// player's car the slider values are swapped in for the duration of the call and the game's own
// values put back after it, so AI and traffic keep the stock physics.
//
// Gravity is rigidbodyspecs.GRAVITY, added to a body's force in RigidBody::OnBeginFrame; that
// is wrapped the same way, for the player's car or for every car.
//
// Per car: a [Car.<vehicle name>] section in the ini holds a car's own slider values; the
// [Physics] values apply to every car without one.
#include "common.h"
#include "physics.h"
#include "game.h"

namespace {

// --- speed.exe v1.3 -------------------------------------------------------------------------
constexpr uintptr_t kSuspSimulate = 0x6B1DD0;       // void SuspensionRacer::OnTaskSimulate(float)
constexpr uintptr_t kRBBeginFrame = 0x6A7050;       // void RigidBody::OnBeginFrame(float)
constexpr uintptr_t kGlobalGripScale = 0x8AAC94;    // float Tweak_GlobalGripScale (1.2)
constexpr uintptr_t kTractionFwdSlip = 0x8AAD94;    // float Tweak_TractionFowardSlip (20)
constexpr uintptr_t kMaxYawBonus = 0x8F8D3C;        // float MaxYawBonus (0.35)
constexpr uintptr_t kDriftRearTable = 0x8F8FB0;     // Table DriftRearFrictionTable: count +0, values* +0x10
constexpr uintptr_t kDriftStabTable = 0x938210;     // tGraph DriftStabilizerTable: {x, y}* +0, count +4
constexpr uintptr_t kISuspensionHandle = 0x404040;
constexpr uintptr_t kIRigidBodyHandle = 0x4039F0;

constexpr int kSR_Tranny = 0xDC;                    // SuspensionRacer: transmission layout (+0x70 TORQUE_SPLIT)
constexpr int kTR_TorqueSplit = 0x70;
constexpr int kRB_Specs = 0x84;                     // RigidBody: rigidbodyspecs layout (+0xF0 GRAVITY)

typedef void(__thiscall* Simulate_t)(void* obj, float dT);
Simulate_t s_origSusp, s_origRB;

template <class T> T& Field(uint8_t* p, int off) { return *(T*)(p + off); }

// The game's own values, captured once.
struct {
    bool ok = false;
    float rear[16];
    int rearN = 0;
    float stab[16];
    int stabN = 0;
} g;

bool CaptureTables() {
    int n = *(int*)kDriftRearTable;
    float* v = *(float**)(kDriftRearTable + 0x10);
    float* graph = *(float**)kDriftStabTable;
    int m = *(int*)(kDriftStabTable + 4);
    if (n <= 0 || n > 16 || !v || m <= 0 || m > 16 || !graph) {
        Log("physics: unexpected drift tables (%d at %p, %d at %p)", n, v, m, graph);
        return false;
    }
    g.rearN = n;
    memcpy(g.rear, v, n * sizeof(float));
    g.stabN = m;
    for (int i = 0; i < m; i++) g.stab[i] = graph[i * 2 + 1];
    g.ok = true;
    return true;
}

// The player's ISuspension / IRigidBody interfaces, to recognise their objects.
bool Owns(uint8_t* obj, void* iface) { return iface && (uint8_t*)iface >= obj && (uint8_t*)iface < obj + 0x300; }

// Saved values while the player's suspension simulates.
struct Saved {
    float grip, slip, yaw;
    float split;
    float* tranny;
};

void Apply(uint8_t* susp, Saved& sv) {
    const Config& c = g_cfg;
    sv.grip = *(float*)kGlobalGripScale;
    sv.slip = *(float*)kTractionFwdSlip;
    sv.yaw = *(float*)kMaxYawBonus;
    *(float*)kGlobalGripScale = sv.grip * c.grip;
    // DoDriveForces scales the cut by Ramp(slip, 1, limit) = (slip - 1) / (limit - 1).
    *(float*)kTractionFwdSlip = c.tractionControl > 0.01f ? 1.0f + (sv.slip - 1.0f) / c.tractionControl : 1.0e9f;
    *(float*)kMaxYawBonus = sv.yaw * c.stability;
    float* graph = *(float**)kDriftStabTable;
    for (int i = 0; i < g.stabN; i++) graph[i * 2 + 1] = g.stab[i] * c.stability;
    float* rear = *(float**)(kDriftRearTable + 0x10);
    for (int i = 0; i < g.rearN; i++) rear[i] = 1.0f + (g.rear[i] - 1.0f) * c.driftAssist;
    uint8_t* tranny = Field<uint8_t*>(susp, kSR_Tranny);
    sv.tranny = tranny ? (float*)(tranny + kTR_TorqueSplit) : nullptr;
    if (sv.tranny) {
        sv.split = *sv.tranny;
        *sv.tranny = Clamp(sv.split * c.torqueSplit, 0.0f, 1.0f);
    }
}

void Restore(const Saved& sv) {
    *(float*)kGlobalGripScale = sv.grip;
    *(float*)kTractionFwdSlip = sv.slip;
    *(float*)kMaxYawBonus = sv.yaw;
    float* graph = *(float**)kDriftStabTable;
    for (int i = 0; i < g.stabN; i++) graph[i * 2 + 1] = g.stab[i];
    float* rear = *(float**)(kDriftRearTable + 0x10);
    memcpy(rear, g.rear, g.rearN * sizeof(float));
    if (sv.tranny) *sv.tranny = sv.split;
}

bool Stock() {
    const Config& c = g_cfg;
    return c.tractionControl >= 1.0f && c.stability >= 1.0f && c.driftAssist >= 1.0f && c.grip == 1.0f &&
           c.torqueSplit == 1.0f;
}

void __fastcall Hook_SuspSimulate(uint8_t* susp, void*, float dT) {
    if (!g.ok || !g_cfg.physics || Stock() || !Owns(susp, Game_PlayerInterface((void*)kISuspensionHandle))) {
        s_origSusp(susp, dT);
        return;
    }
    static uint8_t* logged;
    if (logged != susp) {
        logged = susp;
        uint8_t* tranny = Field<uint8_t*>(susp, kSR_Tranny);
        Log("physics: player suspension %p (torque split %.2f front)", susp, tranny ? Field<float>(tranny, kTR_TorqueSplit) : -1.0f);
    }
    Saved sv;
    Apply(susp, sv);
    s_origSusp(susp, dT);
    Restore(sv);
}

void __fastcall Hook_RBBeginFrame(uint8_t* rb, void*, float dT) {
    float k = g_cfg.gravity;
    uint8_t* specs = Field<uint8_t*>(rb, kRB_Specs);
    if (!g_cfg.physics || k == 1.0f || !specs ||
        !(g_cfg.gravityAll ? Game_PlayerInterface(nullptr) != nullptr   // in game, any car
                           : Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle)))) {
        s_origRB(rb, dT);
        return;
    }
    float& gravity = Field<float>(specs, 0xF0);
    float saved = gravity;
    gravity = saved * k;
    s_origRB(rb, dT);
    gravity = saved;
}

bool Expect(uintptr_t at, std::initializer_list<uint8_t> bytes) {
    size_t i = 0;
    for (uint8_t b : bytes) {
        if (((uint8_t*)at)[i] != b) {
            Log("physics: unexpected code at 0x%06X+%u", (unsigned)at, (unsigned)i);
            return false;
        }
        i++;
    }
    return true;
}

// Jumps from `at` to `hook`; returns a trampoline running the `stolen` bytes and jumping back.
void* Detour(uintptr_t at, size_t stolen, void* hook) {
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, (void*)at, stolen);
    tramp[stolen] = 0xE9;
    *(int32_t*)(tramp + stolen + 1) = (int32_t)((at + stolen) - ((uintptr_t)tramp + stolen + 5));
    DWORD old;
    VirtualProtect((void*)at, stolen, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)hook - (at + 5));
    for (size_t i = 5; i < stolen; i++) p[i] = 0x90;
    VirtualProtect((void*)at, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, stolen);
    return tramp;
}

// --- per car ---------------------------------------------------------------------------------
struct PerCar {
    float Config::*field;
    const char* key;
    float lo, hi;
};
const PerCar kPerCar[] = {
    {&Config::tractionControl, "TractionControl", 0.0f, 1.0f},
    {&Config::stability, "Stability", 0.0f, 1.0f},
    {&Config::driftAssist, "DriftAssist", 0.0f, 1.0f},
    {&Config::grip, "Grip", 0.3f, 2.0f},
    {&Config::torqueSplit, "TorqueSplit", 0.0f, 3.0f},
    {&Config::gravity, "Gravity", 0.1f, 3.0f},
};
constexpr int kNumPerCar = sizeof(kPerCar) / sizeof(kPerCar[0]);

struct {
    std::string car;            // the player's vehicle name, empty = none yet
    bool own = false;           // it has its own [Car.<name>] section
    float global[kNumPerCar];   // the [Physics] values while a car has its own
} pc;

void StoreGlobal() {
    for (int i = 0; i < kNumPerCar; i++) pc.global[i] = g_cfg.*kPerCar[i].field;
}

void LoadGlobal() {
    for (int i = 0; i < kNumPerCar; i++) g_cfg.*kPerCar[i].field = pc.global[i];
}

}  // namespace

std::string Physics_Section() { return pc.own ? "Car." + pc.car : "Physics"; }
const char* Physics_Car() { return pc.car.c_str(); }
bool Physics_OwnSettings() { return pc.own; }

bool Physics_IsPerCar(const void* value) {
    for (const PerCar& p : kPerCar)
        if (value == &(g_cfg.*p.field)) return true;
    return false;
}

void Physics_SetCar(const char* name) {
    std::string car = name ? name : "";
    if (car == pc.car) return;
    if (!pc.own) StoreGlobal();
    pc.car = car;
    std::string section = "Car." + car;
    char buf[64];
    pc.own = !car.empty() && GetPrivateProfileSectionA(section.c_str(), buf, sizeof(buf), IniPath()) > 0;
    LoadGlobal();
    if (pc.own) {
        for (const PerCar& p : kPerCar)
            if (GetPrivateProfileStringA(section.c_str(), p.key, "", buf, sizeof(buf), IniPath()) > 0)
                g_cfg.*p.field = Clamp((float)atof(buf), p.lo, p.hi);
    }
    Log("physics: car %s (%s)", car.empty() ? "?" : car.c_str(), pc.own ? "own settings" : "[Physics] settings");
}

void Physics_SetOwnSettings(bool on) {
    if (on == pc.own || pc.car.empty()) return;
    if (on) {
        // Starts from the values in use; written to [Car.<name>] when the menu saves.
        StoreGlobal();
        pc.own = true;
    } else {
        WritePrivateProfileStringA(("Car." + pc.car).c_str(), nullptr, nullptr, IniPath());
        pc.own = false;
        LoadGlobal();
    }
    Log("physics: %s %s", pc.car.c_str(), on ? "has its own settings" : "back to [Physics] settings");
}

void Physics_Install() {
    if (!Expect(kSuspSimulate, {0x81, 0xEC, 0xFC, 0x00, 0x00, 0x00, 0x56, 0x8B, 0xF1}) ||
        !Expect(kRBBeginFrame, {0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x78}) ||
        !Expect(kRBBeginFrame + 0x9C, {0x8B, 0x86, 0x84, 0x00, 0x00, 0x00, 0xD9, 0x80, 0xF0, 0x00, 0x00, 0x00}) ||
        *(float*)kTractionFwdSlip <= 1.0f || *(float*)kGlobalGripScale <= 0.0f) {
        Log("physics: sliders unavailable");
        return;
    }
    // The two Tweak_ values live in .rdata.
    for (uintptr_t at : {kGlobalGripScale, kTractionFwdSlip}) {
        DWORD old;
        if (!VirtualProtect((void*)at, 4, PAGE_READWRITE, &old)) {
            Log("physics: cannot unprotect 0x%06X", (unsigned)at);
            return;
        }
    }
    s_origSusp = (Simulate_t)Detour(kSuspSimulate, 6, (void*)&Hook_SuspSimulate);
    s_origRB = (Simulate_t)Detour(kRBBeginFrame, 7, (void*)&Hook_RBBeginFrame);
    Log("physics: %s (traction control %.0f%%, stability %.0f%%, drift assist %.0f%%, grip %.2f, torque split x%.2f, "
        "gravity %.2f%s)", g_cfg.physics ? "on" : "off", g_cfg.tractionControl * 100, g_cfg.stability * 100,
        g_cfg.driftAssist * 100, g_cfg.grip, g_cfg.torqueSplit, g_cfg.gravity, g_cfg.gravityAll ? " all cars" : "");
}

void Physics_Frame() {
    // The drift tables are built by static constructors; read them once the game runs.
    if (!g.ok) {
        static bool tried;
        if (!tried) {
            tried = true;
            CaptureTables();
        }
    }
}
