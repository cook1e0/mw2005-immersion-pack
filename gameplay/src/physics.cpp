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
// Drivetrain multiplication: in Tire::UpdateLoaded a driving tyre's forward force is multiplied by
// TireForceEllipseRatio (1.5) before the tyre's grip limit is applied (and divided back if under
// it), so a tyre putting power down runs out of side grip sooner: why rear drive cars step out
// under power and front drive cars push wide, and all-wheel drive does a bit of both by its
// torque split. The ratio is multiplied for the player's car, so the driven axle's character is
// exaggerated (2 = a rear drive car's power oversteer, a front drive car's understeer, doubled)
// or, below 1, softened. Both uses in UpdateLoaded are pointed at our own copies (the constants
// are shared with other code).
// These, the grip (Tweak_GlobalGripScale) and the torque split (the transmission's TORQUE_SPLIT,
// the front wheels' share) are tuning data read while a car's suspension simulates. SuspensionRacer::OnTaskSimulate is wrapped: for the
// player's car the slider values are swapped in for the duration of the call and the game's own
// values put back after it, so AI and traffic keep the stock physics.
//
// Spin limit: the game lets a body spin at up to 30 rad/s (RigidBody::DoIntegration). Once the
// player's car spins that fast (props launching it, a landing while rotating, with the aids turned
// down) each step turns it deep into walls or the ground, and the push-out adds more speed than the
// hit took: the car tumbles around the map gaining speed with nobody driving. The player's spin is
// limited to [Physics] MaxSpin, far above anything a car does on its wheels or in a real crash,
// before and after each integration step: the suspension step that comes next computes its forces
// from the spin it finds (tyre side forces are limited by the wheels' point speed, which a fast
// spin makes huge), so limiting it only before integration let those forces pin it at the game's
// cap again (30 rad/s on each axis, 52 in all) in the same step.
//
// Gravity is rigidbodyspecs.GRAVITY, added to a body's force in RigidBody::OnBeginFrame; that
// is wrapped the same way, for the player's car or for every car.
//
// Popped tires: SuspensionRacer::TuneWheelParams gives a blown tire (Chassis::State blowOuts, from
// ISpikeable::GetTireDamage) full brake, no handbrake and 0.3 of its traction, so a car that
// runs over a spike strip can barely move. That branch is replaced by a call: for the player's
// car (with [Tires] PoppedTires) the tire keeps its brakes and loses part of its traction, with a
// light brake drag for the flat tire's rolling resistance, both scaled by Handicap. Other cars
// get the game's values.
//
// Per car: a [Car.<vehicle name>] section in the ini holds a car's own slider values; the
// [Physics] values apply to every car without one.
#include "common.h"
#include "physics.h"
#include "game.h"
#include <math.h>

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
constexpr uintptr_t kBlownTire = 0x6A9ECA;          // TuneWheelParams: blown tire branch (44 bytes, to 0x6A9EF6)
constexpr int kBlownTireLen = 0x2C;
constexpr uintptr_t kEllipseMul = 0x69DF61;         // UpdateLoaded: fmul [TireForceEllipseRatio]
constexpr uintptr_t kEllipseDiv = 0x69E03A;         // UpdateLoaded: fmul [1 / TireForceEllipseRatio]
constexpr uintptr_t kEllipseRatio = 0x8AAC78;       // 1.5
constexpr uintptr_t kEllipseRatioInv = 0x896D64;    // 0.6667

constexpr int kSR_Tranny = 0xDC;                    // SuspensionRacer: transmission layout (+0x70 TORQUE_SPLIT)
constexpr int kTR_TorqueSplit = 0x70;
constexpr int kRB_Specs = 0x84;                     // RigidBody: rigidbodyspecs layout (+0xF0 GRAVITY)
// RigidBody::mData (**(rb + 0x78)), RigidBody::Volatile, as OnBeginFrame (0x6A7050) and
// DoIntegration (0x6978F0) use it: position, linear and angular velocity (y up).
constexpr int kRB_Data = 0x78;
constexpr int kRV_Position = 0x10, kRV_LinearVel = 0x20, kRV_AngularVel = 0x30;
constexpr int kTire_Brake = 0xC8;                   // SuspensionRacer::Tire: brake 0..1
constexpr int kTire_EBrake = 0xCC;                  // ... handbrake 0..1
constexpr int kTire_Traction = 0xEC;                // ... traction scale for this step

// Popped tire at Handicap 1: traction lost, and brake applied as rolling resistance.
constexpr float kPoppedTraction = 0.45f;
constexpr float kPoppedDrag = 0.06f;

typedef void(__thiscall* Simulate_t)(void* obj, float dT);
Simulate_t s_origSusp, s_origRB;
bool s_playerSusp;      // the player's suspension is simulating

template <class T> T& Field(uint8_t* p, int off) { return *(T*)(p + off); }

// UpdateLoaded's ellipse ratio and its inverse, ours (the game's while not the player's car).
float s_ellipse = 1.5f, s_ellipseInv = 1.0f / 1.5f, s_ellipseGame = 1.5f;

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
    // Never below 1: the forward force is not made smaller than it is.
    s_ellipse = std::max(s_ellipseGame * c.drivetrain, 1.0f);
    s_ellipseInv = 1.0f / s_ellipse;
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
    s_ellipse = s_ellipseGame;
    s_ellipseInv = 1.0f / s_ellipseGame;
}

bool Stock() {
    const Config& c = g_cfg;
    return c.tractionControl >= 1.0f && c.stability >= 1.0f && c.driftAssist >= 1.0f && c.grip == 1.0f &&
           c.torqueSplit == 1.0f && c.drivetrain == 1.0f;
}

void __fastcall Hook_SuspSimulate(uint8_t* susp, void*, float dT) {
    bool player = Owns(susp, Game_PlayerInterface((void*)kISuspensionHandle));
    s_playerSusp = player;
    if (!player || !g.ok || !g_cfg.physics || Stock()) {
        s_origSusp(susp, dT);
        s_playerSusp = false;
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
    s_playerSusp = false;
}

// Called from TuneWheelParams for each blown tire, in place of the game's code.
void __cdecl Hook_BlownTire(uint8_t* tire) {
    float& brake = Field<float>(tire, kTire_Brake);
    float& traction = Field<float>(tire, kTire_Traction);
    if (!s_playerSusp || !g_cfg.poppedTires) {
        Field<float>(tire, kTire_EBrake) = 0.0f;
        brake = 1.0f;
        traction *= 0.3f;
        return;
    }
    float k = g_cfg.tireHandicap;
    brake = std::max(brake, std::min(kPoppedDrag * k, 1.0f));
    traction *= 1.0f - std::min(kPoppedTraction * k, 0.95f);
}

// Drop from the air (testing): requested by the menu, done in the player's next OnBeginFrame.
volatile double s_dropAt = -1;

void DropIfAsked(uint8_t* rb) {
    if (s_dropAt < 0) return;
    if (Now() - s_dropAt > 2.0) {   // not in a car: forget it
        s_dropAt = -1;
        return;
    }
    if (!Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle))) return;
    s_dropAt = -1;
    uint8_t** holder = Field<uint8_t**>(rb, kRB_Data);
    uint8_t* data = holder ? *holder : nullptr;
    if (!data) return;
    float* pos = (float*)(data + kRV_Position);
    float* vel = (float*)(data + kRV_LinearVel);
    float* av = (float*)(data + kRV_AngularVel);
    pos[1] += g_cfg.dropHeight;
    vel[1] = 0.0f;
    av[0] = av[1] = av[2] = 0.0f;
    Log("physics: car dropped from %.1f m (speed %.1f m/s)", g_cfg.dropHeight, sqrtf(vel[0] * vel[0] + vel[2] * vel[2]));
}

// The player's body: its spin limited to [Physics] MaxSpin.
void LimitSpin(uint8_t* rb) {
    float limit = g_cfg.maxSpin;
    if (limit <= 0.0f || !Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle))) return;
    uint8_t** holder = Field<uint8_t**>(rb, kRB_Data);
    uint8_t* data = holder ? *holder : nullptr;
    if (!data) return;
    float* av = (float*)(data + kRV_AngularVel);
    float w = sqrtf(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]);
    if (!(w > limit)) return;   // also skips NaN
    float k = limit / w;
    av[0] *= k;
    av[1] *= k;
    av[2] *= k;
    static double lastLog = -1e9;
    if (Now() - lastLog > 1.0) {
        lastLog = Now();
        const float* v = (const float*)(data + kRV_LinearVel);
        Log("physics: car spinning at %.1f rad/s, limited to %.1f (speed %.1f m/s, vertical %+.1f)", w, limit,
            sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), v[1]);
    }
}

void __fastcall Hook_RBBeginFrame(uint8_t* rb, void*, float dT) {
    DropIfAsked(rb);
    LimitSpin(rb);
    float k = g_cfg.gravity;
    uint8_t* specs = Field<uint8_t*>(rb, kRB_Specs);
    if (!g_cfg.physics || k == 1.0f || !specs ||
        !(g_cfg.gravityAll ? Game_PlayerInterface(nullptr) != nullptr   // in game, any car
                           : Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle)))) {
        s_origRB(rb, dT);
    } else {
        float& gravity = Field<float>(specs, 0xF0);
        float saved = gravity;
        gravity = saved * k;
        s_origRB(rb, dT);
        gravity = saved;
    }
    LimitSpin(rb);   // after integrating: the next suspension step starts from a sane spin
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

void InstallBlownTire() {
    // mov eax,[edx]; mov [eax+0xCC],0; mov eax,[edx]; mov [eax+0xC8],1.0; mov eax,[edx];
    // fld [eax+0xEC]; fmul [0.3]; fstp [eax+0xEC]
    if (!Expect(kBlownTire, {0x8B, 0x02, 0xC7, 0x80, 0xCC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x02,
                             0xC7, 0x80, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x8B, 0x02, 0xD9, 0x80,
                             0xEC, 0x00, 0x00, 0x00, 0xD8, 0x0D, 0x58, 0x4B, 0x89, 0x00, 0xD9, 0x98, 0xEC, 0x00,
                             0x00, 0x00})) {
        Log("physics: popped tires unavailable");
        g_cfg.poppedTires = false;
        return;
    }
    // ecx (wheel index) and edx (tire pointer slot) live across the call; the x87 stack is empty.
    uint8_t code[kBlownTireLen];
    memset(code, 0x90, sizeof(code));
    uint8_t* p = code;
    *p++ = 0x51;                                        // push ecx
    *p++ = 0x52;                                        // push edx
    *p++ = 0xFF; *p++ = 0x32;                           // push dword [edx]  (the Tire*)
    *p++ = 0xE8;                                        // call Hook_BlownTire
    *(int32_t*)p = (int32_t)((uintptr_t)&Hook_BlownTire - (kBlownTire + (p + 4 - code)));
    p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;              // add esp,4
    *p++ = 0x5A;                                        // pop edx
    *p++ = 0x59;                                        // pop ecx
    *p++ = 0xE9;                                        // jmp past the original code
    *(int32_t*)p = (int32_t)((kBlownTire + kBlownTireLen) - (kBlownTire + (p + 4 - code)));
    DWORD old;
    VirtualProtect((void*)kBlownTire, sizeof(code), PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)kBlownTire, code, sizeof(code));
    VirtualProtect((void*)kBlownTire, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kBlownTire, sizeof(code));
    Log("physics: popped tires %s (handicap %.2f)", g_cfg.poppedTires ? "drivable" : "as in the game", g_cfg.tireHandicap);
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
    {&Config::drivetrain, "Drivetrain", 0.7f, 3.0f},
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

void Physics_DropCar() { s_dropAt = Now(); }

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
    InstallBlownTire();
    // fmul dword ptr [abs32] at both uses of the ratio: point them at our copies.
    if (Expect(kEllipseMul, {0xD8, 0x0D, 0x78, 0xAC, 0x8A, 0x00}) && Expect(kEllipseDiv, {0xD8, 0x0D, 0x64, 0x6D, 0x89, 0x00}) &&
        *(float*)kEllipseRatio > 1.0f) {
        s_ellipseGame = s_ellipse = *(float*)kEllipseRatio;
        s_ellipseInv = 1.0f / s_ellipse;
        for (auto at : {std::make_pair(kEllipseMul, &s_ellipse), std::make_pair(kEllipseDiv, &s_ellipseInv)}) {
            DWORD old;
            VirtualProtect((void*)(at.first + 2), 4, PAGE_EXECUTE_READWRITE, &old);
            *(float**)(at.first + 2) = at.second;
            VirtualProtect((void*)(at.first + 2), 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)at.first, 6);
        }
        Log("physics: drivetrain multiplication ready (ellipse ratio %.2f)", s_ellipseGame);
    } else {
        Log("physics: unexpected tire code, drivetrain multiplication unavailable");
    }
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
