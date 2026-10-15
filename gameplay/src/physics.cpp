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
// The torque itself is limited too: in the pinball the game applied, every step and whatever the
// spin was, a torque that took the car from 8 rad/s to the cap in one step, rolling it about half
// a radian left, then right (the car "can't decide which way it's tilted"). Before integrating,
// the spin change the torque will make (DoIntegration's own formula) is worked out, and the torque
// scaled down so the spin after the step stays within MaxSpin, and so one step's torque never
// changes the spin by more than kMaxSpinAccel * dT (the broken torques, up to billions of N m,
// left a twitch even when under the limit).
//
// Gravity is rigidbodyspecs.GRAVITY, added to a body's force in RigidBody::OnBeginFrame; that
// is wrapped the same way, for the player's car or for every car.
//
// Grip balance and off-road grip: SuspensionRacer::TuneWheelParams sets each tyre's traction boost
// for the step (Tire::BeginFrame resets it every step) and ends with DoDrifting; that call is
// wrapped, and for the player's car the rear tyres' boost is scaled by [Physics] RearGrip (below 1:
// the rear lets go first, oversteer) and any tyre on an off-road surface (its simsurface name:
// grass, dirt, mud, gravel, sand...) by [Physics] OffroadGrip, and every tyre in the rain by
// [Physics] RainGrip (with the player view's rain intensity: eViews[1].Precipitation, a Rain, at
// 0x9196B8, intensity at +0x4C). UpdateLoaded applies the boost to both the tyre's sliding friction
// and its grip limit.
// Slide grip: a tyre that has broken loose slides on its dynamic friction (UpdateLoaded,
// DYNAMIC_GRIP * traction boost * pilot factor at 0x69DD22), its grip limit is the static one. For
// the player's car that multiply also takes [Physics] SlideGrip: below 1 a sliding tyre has less
// grip than at its peak, so a rear that lets go slides the car instead of tightening the turn.
// Slide oversteer: after the grip limit, UpdateLoaded multiplies the tyre's side force by its
// surface's LATERAL_GRIP and its drive force by DRIVE_GRIP (0x69E098); grass takes more side grip
// than drive grip, which is why a car on grass keeps driving and slides sideways. For the player's
// rear tyres that side grip also takes [Physics] SlideOversteer, so the rear slides under power
// on the road too, while the front steers and the drive grip is untouched.
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
constexpr int kRV_Position = 0x10, kRV_LinearVel = 0x20, kRV_AngularVel = 0x30, kRV_Torque = 0x60;
// RigidBody::mInvWorldTensor: DoIntegration turns torque * dT into a spin change with
// UMath::Rotate (row vector times this 4x4, 0x401670) and adds it to the angular velocity.
constexpr int kRB_InvWorldTensor = 0x90;
// The most a step's torque may change the player's spin, per second (tyres, suspension and a hard
// one-wheel landing stay well under it; impacts change the velocities directly, not by torque).
constexpr float kMaxSpinAccel = 150.0f;
constexpr int kTire_Brake = 0xC8;                   // SuspensionRacer::Tire: brake 0..1
constexpr int kTire_EBrake = 0xCC;                  // ... handbrake 0..1
constexpr int kTire_Traction = 0xEC;                // ... traction scale for this step
constexpr int kTire_Surface = 0x9C;                 // ... road surface: Attrib::Instance (layout at +8)
constexpr int kSurf_Name = 0xDC;                    // simsurface layout: CollectionName (char*)
constexpr int kSuspIface_Tires = 0x168;             // ISuspension (SuspensionRacer's) -> Tire* [4], front first
constexpr uintptr_t kCallDoDrifting = 0x6AA006;     // TuneWheelParams: call SuspensionRacer::DoDrifting(state)
constexpr uintptr_t kDoDrifting = 0x6A9760;         // thiscall, 1 arg
constexpr uintptr_t kPlayerRain = 0x9196B8;         // Rain* of the player view (eViews[1] + 0x68)
constexpr int kRain_Intensity = 0x4C;
constexpr uintptr_t kSlideFriction = 0x69DD22;      // UpdateLoaded: fmul [esi+0xEC] for the sliding friction
constexpr uintptr_t kLateralGrip = 0x69E098;        // UpdateLoaded: fld [ecx+0xF0] (surface LATERAL_GRIP; esi = tyre)

// Popped tire at Handicap 1: traction lost, and brake applied as rolling resistance.
constexpr float kPoppedTraction = 0.45f;
constexpr float kPoppedDrag = 0.06f;

typedef void(__thiscall* Simulate_t)(void* obj, float dT);
Simulate_t s_origSusp, s_origRB;
bool s_playerSusp;      // the player's suspension is simulating
extern "C" float s_slideGrip;
float s_slideGrip = 1.0f;   // the sliding friction factor for the suspension simulating now
// The player's tyres while its suspension simulates (else null) and their side grip factors.
extern "C" uint8_t* s_latTires[4];
extern "C" float s_latMul[4];
uint8_t* s_latTires[4];
float s_latMul[4] = {1.0f, 1.0f, 1.0f, 1.0f};

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
           c.torqueSplit == 1.0f && c.drivetrain == 1.0f && c.slideGrip == 1.0f && c.slideOversteer == 1.0f;
}

// Diagnostics: the player's physics step lengths (the game's forces divide by them).
float s_suspDT = 0.0f;                  // the player's last suspension step
struct {
    float rbMin = 1e9f, rbMax = 0.0f, suspMin = 1e9f, suspMax = 0.0f;
    int steps = 0;
    double since = 0;
} s_dt;

void __fastcall Hook_SuspSimulate(uint8_t* susp, void*, float dT) {
    bool player = Owns(susp, Game_PlayerInterface((void*)kISuspensionHandle));
    s_playerSusp = player;
    if (player) {
        s_suspDT = dT;
        s_dt.suspMin = std::min(s_dt.suspMin, dT);
        s_dt.suspMax = std::max(s_dt.suspMax, dT);
    }
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
    s_slideGrip = g_cfg.slideGrip;
    if (g_cfg.slideOversteer != 1.0f) {
        uint8_t* iface = (uint8_t*)Game_PlayerInterface((void*)kISuspensionHandle);
        for (int i = 0; i < 4; i++) {
            s_latTires[i] = iface ? *(uint8_t**)(iface + kSuspIface_Tires + i * 4) : nullptr;
            s_latMul[i] = i >= 2 ? g_cfg.slideOversteer : 1.0f;
        }
    }
    s_origSusp(susp, dT);
    for (uint8_t*& t : s_latTires) t = nullptr;
    s_slideGrip = 1.0f;
    Restore(sv);
    s_playerSusp = false;
}

// In place of UpdateLoaded's fmul [esi+0xEC] (6 bytes: a call and a nop): the same multiply and
// the slide grip factor.
__asm__(
    ".text\n"
    "_SlideFrictionStub:\n"
    "    fmuls 0xEC(%esi)\n"
    "    fmuls _s_slideGrip\n"
    "    ret\n");
extern "C" void SlideFrictionStub();

// In place of UpdateLoaded's fld [ecx+0xF0] (6 bytes): the same load, times s_latMul for a tyre in
// s_latTires. eax is kept; the flags are free here (the next test sets them again).
__asm__(
    ".text\n"
    "_LateralGripStub:\n"
    "    flds 0xF0(%ecx)\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    ret\n"
    "2:  fmuls _s_latMul(,%eax,4)\n"
    "    popl %eax\n"
    "    ret\n");
extern "C" void LateralGripStub();

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

// Before the player's body integrates: the torque scaled so the spin it adds stays within MaxSpin.
// Returns the spin predicted after the step (for the check after it), or false if not limited.
bool LimitTorque(uint8_t* rb, float dT, float predicted[3]) {
    float limit = g_cfg.maxSpin;
    if (limit <= 0.0f || !(dT > 0.0f) || !Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle))) return false;
    uint8_t** holder = Field<uint8_t**>(rb, kRB_Data);
    uint8_t* data = holder ? *holder : nullptr;
    if (!data || data[0x5D] != 0) return false;            // not awake: no integration
    float* T = (float*)(data + kRV_Torque);
    const float* av = (const float*)(data + kRV_AngularVel);
    const float* M = (const float*)(rb + kRB_InvWorldTensor);
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = (T[0] * M[i] + T[1] * M[4 + i] + T[2] * M[8 + i]) * dT;
    float w1[3] = {av[0] + d[0], av[1] + d[1], av[2] + d[2]};
    float n1 = sqrtf(w1[0] * w1[0] + w1[1] * w1[1] + w1[2] * w1[2]);
    float s = 1.0f;
    float dn = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const char* why = nullptr;
    if (dn > kMaxSpinAccel * dT) {
        s = kMaxSpinAccel * dT / dn;
        why = "spike";
    }
    float ws[3] = {av[0] + d[0] * s, av[1] + d[1] * s, av[2] + d[2] * s};
    if (sqrtf(ws[0] * ws[0] + ws[1] * ws[1] + ws[2] * ws[2]) > limit) {
        why = "spin";
        // The largest share s of the torque with |av + s d| <= limit (|av| <= limit already).
        float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        float wd = av[0] * d[0] + av[1] * d[1] + av[2] * d[2];
        float ww = av[0] * av[0] + av[1] * av[1] + av[2] * av[2];
        float disc = wd * wd - dd * (ww - limit * limit);
        s = std::min(s, dd > 1e-12f && disc >= 0.0f ? std::max(0.0f, std::min(1.0f, (-wd + sqrtf(disc)) / dd)) : 0.0f);
    }
    if (why) {
        static double lastLog = -1e9;
        if (Now() - lastLog > 1.0) {
            lastLog = Now();
            float tn = sqrtf(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
            Log("physics: torque %.0f N m (%s) would change the spin by %.1f to %.1f rad/s in one step: scaled to %.4f "
                "(step %.5f s, suspension step %.5f s)", tn, why, dn, n1, s, dT, s_suspDT);
        }
        for (int i = 0; i < 3; i++) T[i] *= s;
    } else if (!(n1 >= 0.0f)) {
        return false;                                       // NaN: leave it to the game
    }
    for (int i = 0; i < 3; i++) predicted[i] = av[i] + d[i] * s;
    return true;
}

// After the step: how well LimitTorque predicted the spin (a few times, to confirm the layout).
void CheckPrediction(uint8_t* rb, const float predicted[3], const float before[3]) {
    static int checked = 0;
    if (checked >= 3) return;
    uint8_t** holder = Field<uint8_t**>(rb, kRB_Data);
    uint8_t* data = holder ? *holder : nullptr;
    if (!data) return;
    const float* av = (const float*)(data + kRV_AngularVel);
    float change = 0, err = 0;
    for (int i = 0; i < 3; i++) {
        change += (predicted[i] - before[i]) * (predicted[i] - before[i]);
        err += (av[i] - predicted[i]) * (av[i] - predicted[i]);
    }
    if (sqrtf(change) < 0.05f) return;                      // too small to tell
    checked++;
    Log("physics: spin change predicted %.3f rad/s, off by %.4f", sqrtf(change), sqrtf(err));
}

// A tyre on an off-road surface, by the surface's own name.
bool OffroadTire(uint8_t* tire) {
    uintptr_t layout = *(uintptr_t*)(tire + kTire_Surface + 8);
    if (layout < 0x10000 || layout >= 0x7FFF0000) return false;
    const char* name = *(const char**)(layout + kSurf_Name);
    if ((uintptr_t)name < 0x10000 || (uintptr_t)name >= 0x7FFF0000 || IsBadStringPtrA(name, 32)) return false;
    static const char* const kWords[] = {"grass", "dirt", "mud", "gravel", "sand", "soil", "earth", "field",
                                         "offroad", "off_road", "forest", "bush", "shrub", "snow", "rock"};
    char low[48] = {};
    for (int k = 0; k < 47 && name[k]; k++) low[k] = (char)tolower((unsigned char)name[k]);
    for (const char* w : kWords)
        if (strstr(low, w)) return true;
    return false;
}

// Rain intensity in the player's view (0 = dry), checked against nonsense.
float RainIntensity() {
    uintptr_t rain = *(uintptr_t*)kPlayerRain;
    if (rain < 0x10000 || rain >= 0x7FFF0000) return 0.0f;
    float v = *(float*)(rain + kRain_Intensity);
    float r = v >= 0.0f && v <= 10.0f ? v : 0.0f;
    static float logged = -1.0f;
    if (fabsf(r - logged) > 0.1f || (r > 0.0f) != (logged > 0.0f)) {
        logged = r;
        Log("physics: rain intensity %.2f", r);
    }
    return r;
}

typedef void(__thiscall* DoDrifting_t)(void* susp, void* state);

void __fastcall Hook_DoDrifting(uint8_t* susp, void*, void* state) {
    ((DoDrifting_t)kDoDrifting)(susp, state);
    if (!s_playerSusp || !g_cfg.physics) return;
    float rear = g_cfg.rearGrip, off = g_cfg.offroadGrip;
    // Full RainGrip from a light rain on (intensity 0.3).
    float wet = 1.0f - (1.0f - g_cfg.rainGrip) * std::min(RainIntensity() / 0.3f, 1.0f);
    if (rear == 1.0f && off == 1.0f && wet == 1.0f) return;
    uint8_t* iface = (uint8_t*)Game_PlayerInterface((void*)kISuspensionHandle);
    if (!iface) return;
    for (int i = 0; i < 4; i++) {
        uint8_t* tire = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        if ((uintptr_t)tire < 0x10000 || (uintptr_t)tire >= 0x7FFF0000) continue;
        float k = (i >= 2 ? rear : 1.0f) * wet;
        if (off != 1.0f && OffroadTire(tire)) k *= off;
        Field<float>(tire, kTire_Traction) *= k;
    }
}

void __fastcall Hook_RBBeginFrame(uint8_t* rb, void*, float dT) {
    DropIfAsked(rb);
    if (Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle))) {
        s_dt.rbMin = std::min(s_dt.rbMin, dT);
        s_dt.rbMax = std::max(s_dt.rbMax, dT);
        s_dt.steps++;
        if (Now() - s_dt.since > 10.0) {
            if (s_dt.since > 0)
                Log("physics: %d steps in %.0f s, step %.5f..%.5f s, suspension step %.5f..%.5f s", s_dt.steps,
                    Now() - s_dt.since, s_dt.rbMin, s_dt.rbMax, s_dt.suspMin, s_dt.suspMax);
            s_dt = {};
            s_dt.since = Now();
        }
    }
    LimitSpin(rb);
    float predicted[3], before[3] = {0, 0, 0};
    bool limited = LimitTorque(rb, dT, predicted);
    if (limited) {
        const float* av = (const float*)(*Field<uint8_t**>(rb, kRB_Data) + kRV_AngularVel);
        for (int i = 0; i < 3; i++) before[i] = av[i];
    }
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
    if (limited) CheckPrediction(rb, predicted, before);
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
    if (Expect(kSlideFriction, {0xD8, 0x8E, 0xEC, 0x00, 0x00, 0x00})) {
        uint8_t code[6] = {0xE8, 0, 0, 0, 0, 0x90};
        int32_t rel = (int32_t)((uintptr_t)&SlideFrictionStub - (kSlideFriction + 5));
        memcpy(code + 1, &rel, 4);
        DWORD old;
        VirtualProtect((void*)kSlideFriction, sizeof(code), PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)kSlideFriction, code, sizeof(code));
        VirtualProtect((void*)kSlideFriction, sizeof(code), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kSlideFriction, sizeof(code));
        Log("physics: slide grip ready (%.2f)", g_cfg.slideGrip);
        if (Expect(kLateralGrip, {0xD9, 0x81, 0xF0, 0x00, 0x00, 0x00})) {
            uint8_t lat[6] = {0xE8, 0, 0, 0, 0, 0x90};
            int32_t r = (int32_t)((uintptr_t)&LateralGripStub - (kLateralGrip + 5));
            memcpy(lat + 1, &r, 4);
            VirtualProtect((void*)kLateralGrip, sizeof(lat), PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)kLateralGrip, lat, sizeof(lat));
            VirtualProtect((void*)kLateralGrip, sizeof(lat), old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)kLateralGrip, sizeof(lat));
            Log("physics: slide oversteer ready (%.2f)", g_cfg.slideOversteer);
        }
    } else {
        Log("physics: slide grip unavailable");
    }
    if (Expect(kCallDoDrifting, {0xE8}) && kCallDoDrifting + 5 + *(int32_t*)(kCallDoDrifting + 1) == kDoDrifting) {
        DWORD old;
        VirtualProtect((void*)kCallDoDrifting, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(kCallDoDrifting + 1) = (int32_t)((uintptr_t)&Hook_DoDrifting - (kCallDoDrifting + 5));
        VirtualProtect((void*)kCallDoDrifting, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kCallDoDrifting, 5);
        Log("physics: grip balance ready (rear grip %.2f, off-road grip %.2f)", g_cfg.rearGrip, g_cfg.offroadGrip);
    } else {
        Log("physics: grip balance unavailable");
    }
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
