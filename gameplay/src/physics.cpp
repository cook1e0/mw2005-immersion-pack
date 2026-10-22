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
// These and the grip (Tweak_GlobalGripScale) are tuning data read while a car's suspension simulates. SuspensionRacer::OnTaskSimulate is wrapped: for the
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
// wrapped, and for the player's car the front tyres' boost is scaled by 1 + [Physics] GripBalance,
// the rear ones' by 1 - GripBalance (above 0 the rear lets go first, oversteer; the overall grip is
// Grip's), and any tyre on an off-road surface (its simsurface name:
// grass, dirt, mud, gravel, sand...) by [Physics] OffroadGrip, and every tyre in the rain by
// [Physics] RainGrip (with the player view's rain intensity: eViews[1].Precipitation, a Rain, at
// 0x9196B8, intensity at +0x4C). UpdateLoaded applies the boost to both the tyre's sliding friction
// and its grip limit.
// Slide grip: a tyre that has broken loose slides on its dynamic friction (UpdateLoaded,
// DYNAMIC_GRIP * traction boost * pilot factor at 0x69DD22), its grip limit is the static one. For
// the player's car that multiply also takes [Physics] SlideGrip: below 1 a sliding tyre has less
// grip than at its peak, so a rear that lets go slides the car instead of tightening the turn.
// Speed grip loss (for comparing with Aero, to go): after the grip limit, UpdateLoaded multiplies
// the tyre's side force by its surface's LATERAL_GRIP (0x69E098); for the player's tyres that also
// takes 1 - [Physics] SpeedGripLoss * (speed / 300 km/h)^2, so a car pulls away and brakes as
// before but corners on less grip the faster it goes.
// Power oversteer: UpdateLoaded weighs a driving wheel's drive force by TireForceEllipseRatio
// (1.5, at 0x8AAC78) against the tyre's grip budget (load * static grip), so throttle uses up more
// of the grip than its own force and leaves less for cornering; below the limit the drive force is
// divided back (0x896D64, 1/1.5). For the player's tyres both are swapped for that ratio times
// [Physics] PowerOversteer: above 1 the driven wheels lose more side grip under power, the more so
// the more torque the car has (rear drive: oversteer, front drive: understeer).
// Slide oversteer (the older way, to compare, behind [Physics] UsePowerOversteer=0): the rear
// tyres' side grip (the LATERAL_GRIP multiply below) takes [Physics] SlideOversteer with the game's
// throttle (lifting off in a front drive car), coming back as the rear's slip angle (Tire +0x118,
// in turns) grows from 20 to 45 degrees.
// Aero: Chassis::DoAerodynamics (called from SuspensionRacer::OnTaskSimulate) pushes the car down
// with the game's own downforce: speed * AERO_COEFFICIENT * 2000 (Physics::Info::
// AerodynamicDownforce), times the Aerodynamics tuning, at a point between the axles set by
// AERO_CG, only while it is positive. That call is wrapped, and for the player's car a force of
// [Physics] Aero times the car's weight at 300 km/h, growing with the square of the speed, is added
// the same way at the same point (Chassis::State: matrix +0, linear_vel +0x50, ground_effect +0x6C,
// cog +0x70, mass +0xAC, speed +0xBC; IRigidBody::ResolveForce(force, point), vtable +0x88): above
// 0 more downforce, below 0 lift. It loads or unloads the tyres like the game's own, so the grip,
// the suspension and the steering weight all follow.
//
// ABS and traction control use each tyre's own wheel speed: UpdateLoaded keeps the radius (+0xC4),
// the angular speed (+0xD0, rad/s) and the slip speed (+0xE8 = +0xD0 * +0xC4 - the road speed under
// the tyre, set at 0x69F209): strongly negative when a wheel locks under braking, positive when it
// spins. Both run in the DoDrifting hook, after TuneWheelParams has set each tyre's brake (+0xC8)
// for the step and before the tyres are updated, from the slip of the step before.
// - ABS ([Physics] ABS): a wheel braked while turning more than 12% slower than the road (the
//   lock) has its brake eased off (to a quarter at most) and reapplied as it rolls again, a few
//   times a second: real ABS pulsing, the car stops straight. The handbrake (+0xCC) is untouched.
// - Traction control ([Physics] TC): the game's own traction control at 100% for the step (it cuts
//   a spinning wheel's torque while steering), and the throttle this mod sends (controls.cpp) is
//   trimmed while the driven wheels (by the torque split) spin past 12% slip; launch control: from a
//   standstill (under 4 m/s) the limit is 6%, so a launch hooks up without wheelspin.
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
#include "input.h"
#include "inisave.h"
#include "patch.h"
#include <math.h>
#include <vector>

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
constexpr int kTire_Radius = 0xC4, kTire_AngVel = 0xD0, kTire_SlipSpeed = 0xE8;
// Tire: the ground's speed under the wheel along it at the last step (m/s), stored by
// Tire::UpdateLoaded (0x69F065) from its argument whatever the wheel does; the slip speed at +0xE8
// is forced to 0 in some states (0x69F1EB), so a locked wheel can't be told from it.
constexpr int kTire_RoadSpeed = 0xF4;
constexpr uintptr_t kCallDoDrifting = 0x6AA006;     // TuneWheelParams: call SuspensionRacer::DoDrifting(state)
constexpr uintptr_t kDoDrifting = 0x6A9760;         // thiscall, 1 arg
constexpr uintptr_t kPlayerRain = 0x9196B8;         // Rain* of the player view (eViews[1] + 0x68)
constexpr int kRain_Intensity = 0x4C;
constexpr uintptr_t kSlideFriction = 0x69DD22;      // UpdateLoaded: fmul [esi+0xEC] for the sliding friction
constexpr uintptr_t kEllipse = 0x69DF61;            // UpdateLoaded: fmul [TireForceEllipseRatio] (esi = tyre)
constexpr uintptr_t kEllipseUndo = 0x69E03A;        // UpdateLoaded: fmul [1 / 1.5] below the grip limit
constexpr uintptr_t kEllipseRatio = 0x8AAC78, kEllipseInv = 0x896D64;
constexpr int kTire_SlipAngle = 0x118;
constexpr int kState_Gas = 0x4C;                    // Chassis::State: gas_input 0..1
constexpr uintptr_t kLateralGrip = 0x69E098;        // UpdateLoaded: fld [ecx+0xF0] (surface LATERAL_GRIP; esi = tyre)
constexpr uintptr_t kCallDoAero = 0x6B206E;         // SuspensionRacer::OnTaskSimulate: call Chassis::DoAerodynamics
constexpr uintptr_t kDoAerodynamics = 0x68D8E0;     // thiscall (state, drag_pct, aero_pct, front_z, rear_z, tunings)
constexpr uintptr_t kAeroDropOff = 0x8F89D0, kAeroDropOffMin = 0x8F89D4;
constexpr int kCh_Owner = 0x34;                     // Chassis: ISimable* (vtable +0x54: GetRigidBody)
constexpr int kCh_Layout = 0x78;                    // Chassis: chassis attributes' layout
constexpr int kChL_AeroCg = 0x5C, kChL_AeroCoef = 0x68;
constexpr int kSt_LinearVel = 0x50, kSt_Ground = 0x6C, kSt_Cog = 0x70, kSt_Mass = 0xAC, kSt_Speed = 0xBC;
constexpr int kTunings_Aero = 0x10;                 // Physics::Tunings: AERODYNAMICS
constexpr float kAeroRefSpeed = 300.0f / 3.6f;      // Aero and SpeedGripLoss are their full value here

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
float s_speedMul = 1.0f;    // the speed grip loss part of s_latMul this step
// The tyre force ellipse for the player's tyres (s_latTires): the game's ratio times PowerOversteer.
extern "C" float s_ellipse, s_ellipseInv;
float s_ellipse = 1.5f, s_ellipseInv = 1.0f / 1.5f;

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
};

void Apply(uint8_t* susp, Saved& sv) {
    const Config& c = g_cfg;
    sv.grip = *(float*)kGlobalGripScale;
    sv.slip = *(float*)kTractionFwdSlip;
    sv.yaw = *(float*)kMaxYawBonus;
    *(float*)kGlobalGripScale = sv.grip * c.grip;
    // DoDriveForces scales the cut by Ramp(slip, 1, limit) = (slip - 1) / (limit - 1).
    float tcStrength = c.tc ? 1.0f : c.tractionControl;   // TC on: the game's own at full strength
    *(float*)kTractionFwdSlip = tcStrength > 0.01f ? 1.0f + (sv.slip - 1.0f) / tcStrength : 1.0e9f;
    *(float*)kMaxYawBonus = sv.yaw * c.stability;
    float* graph = *(float**)kDriftStabTable;
    for (int i = 0; i < g.stabN; i++) graph[i * 2 + 1] = g.stab[i] * c.stability;
    float* rear = *(float**)(kDriftRearTable + 0x10);
    for (int i = 0; i < g.rearN; i++) rear[i] = 1.0f + (g.rear[i] - 1.0f) * c.driftAssist;
}

void Restore(const Saved& sv) {
    *(float*)kGlobalGripScale = sv.grip;
    *(float*)kTractionFwdSlip = sv.slip;
    *(float*)kMaxYawBonus = sv.yaw;
    float* graph = *(float**)kDriftStabTable;
    for (int i = 0; i < g.stabN; i++) graph[i * 2 + 1] = g.stab[i];
    float* rear = *(float**)(kDriftRearTable + 0x10);
    memcpy(rear, g.rear, g.rearN * sizeof(float));
}

bool Stock() {
    const Config& c = g_cfg;
    return (c.tractionControl >= 1.0f || c.tc) && c.stability >= 1.0f && c.driftAssist >= 1.0f && c.grip == 1.0f &&
           c.slideGrip == 1.0f && (c.useAero || c.speedGripLoss == 0.0f) &&
           (c.usePowerOversteer ? c.powerOversteer == 1.0f : c.slideOversteer == 1.0f);
}

// Side grip left at `speed` (m/s) for SpeedGripLoss `loss`: 1 - loss * (speed / 300 km/h)^2, at
// least a tenth.
float SpeedGrip(float loss, float speed) {
    float r = speed / kAeroRefSpeed;
    return Clamp(1.0f - loss * r * r, 0.1f, 1.0f);
}

float s_suspDT = 0.0f;                  // the player's last suspension step (ABS and TC time their pulses by it)
float s_playerSpeed = 0.0f;             // m/s, from the player's rigid body at its last step

void __fastcall Hook_SuspSimulate(uint8_t* susp, void*, float dT) {
    uint8_t* iface = (uint8_t*)Game_PlayerInterface((void*)kISuspensionHandle);
    bool player = Owns(susp, iface);
    s_playerSusp = player;
    if (player) s_suspDT = dT;
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
    // The player's tyres, for the side grip and ellipse stubs (slide oversteer's rear part is set in
    // the DoDrifting hook: it needs the throttle).
    s_speedMul = !g_cfg.useAero && g_cfg.speedGripLoss > 0.0f ? SpeedGrip(g_cfg.speedGripLoss, s_playerSpeed) : 1.0f;
    float ratio = *(const float*)kEllipseRatio * (g_cfg.usePowerOversteer ? g_cfg.powerOversteer : 1.0f);
    s_ellipse = ratio;
    s_ellipseInv = 1.0f / ratio;
    for (int i = 0; i < 4; i++) {
        s_latTires[i] = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        s_latMul[i] = s_speedMul;
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

// In place of UpdateLoaded's fmul [TireForceEllipseRatio] and its fmul [1/1.5] undo (6 bytes
// each): the player's tyres (s_latTires) take s_ellipse / s_ellipseInv, the others the game's.
__asm__(
    ".text\n"
    "_EllipseStub:\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    fmuls 0x8AAC78\n"
    "    ret\n"
    "2:  popl %eax\n"
    "    fmuls _s_ellipse\n"
    "    ret\n"
    "_EllipseUndoStub:\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    fmuls 0x896D64\n"
    "    ret\n"
    "2:  popl %eax\n"
    "    fmuls _s_ellipseInv\n"
    "    ret\n");
extern "C" void EllipseStub();
extern "C" void EllipseUndoStub();

bool Plausible(uintptr_t p);

typedef void(__thiscall* DoAerodynamics_t)(void* chassis, const uint8_t* state, float drag, float aero, float frontZ,
                                           float rearZ, const uint8_t* tunings);
typedef void*(__thiscall* GetRigidBody_t)(void* simable);
typedef void(__thiscall* ResolveForce_t)(void* rb, const float* force, const float* point);

// The game's own aerodynamics, then for the player's car [Physics] Aero's lift or downforce.
void __fastcall Hook_DoAerodynamics(uint8_t* chassis, void*, const uint8_t* state, float drag, float aero, float frontZ,
                                    float rearZ, const uint8_t* tunings) {
    ((DoAerodynamics_t)kDoAerodynamics)(chassis, state, drag, aero, frontZ, rearZ, tunings);
    if (!s_playerSusp || !g_cfg.physics || !state) return;
    const float* m = (const float*)state;   // rows: right, up, forward, position (4 floats each)
    uint8_t* layout = Field<uint8_t*>(chassis, kCh_Layout);
    float mass = *(const float*)(state + kSt_Mass), speed = *(const float*)(state + kSt_Speed);
    float tuning = tunings ? *(const float*)(tunings + kTunings_Aero) : 0.0f;

    // The car's own aero, logged once per car: its downforce at 300 km/h as a share of its weight.
    static uint8_t* logged;
    if (logged != chassis && Plausible((uintptr_t)layout) && mass > 1.0f) {
        logged = chassis;
        float game = kAeroRefSpeed * Field<float>(layout, kChL_AeroCoef) * 2000.0f * (1.0f + 0.25f * tuning);
        Log("physics: aero: the game's downforce at 300 km/h is %.0f%% of the car's weight (AERO_COEFFICIENT %.4f, "
            "AERO_CG %.0f%%, tuning %+.0f, mass %.0f kg), linear in speed; Aero %+.2f adds %.0f%% at 300 km/h, "
            "with the square of the speed",
            game / (mass * 9.81f) * 100.0f, Field<float>(layout, kChL_AeroCoef), Field<float>(layout, kChL_AeroCg), tuning,
            mass, g_cfg.aero, g_cfg.aero * 100.0f);
    }
    if (!g_cfg.useAero || g_cfg.aero == 0.0f || aero <= 0.0f || mass <= 1.0f || speed < 1.0f) return;
    float ground = *(const float*)(state + kSt_Ground);
    if (ground == 0.0f) return;   // in the air: no lift or downforce to speak of
    // As the game's own: less when tilted with fewer than two wheels down, none going backwards.
    float upness = ground >= 0.5f ? 1.0f : std::max(m[5], 0.0f);
    const float* vel = (const float*)(state + kSt_LinearVel);
    float forwardness = std::max((vel[0] * m[8] + vel[1] * m[9] + vel[2] * m[10]) / speed, 0.0f);
    forwardness = std::max(*(const float*)kAeroDropOffMin, powf(forwardness, *(const float*)kAeroDropOff));
    float r = speed / kAeroRefSpeed;
    float f = g_cfg.aero * mass * 9.81f * r * r * upness * forwardness;   // + down, - up
    // At the game's aero point: the car's centre of gravity, moved along it by AERO_CG between the axles.
    const float* cog = (const float*)(state + kSt_Cog);
    float z = Plausible((uintptr_t)layout) ? (frontZ - rearZ) * (Field<float>(layout, kChL_AeroCg) * 0.01f) + rearZ : cog[2];
    float point[3], force[3];
    for (int a = 0; a < 3; a++) {
        point[a] = m[a] * cog[0] + m[4 + a] * cog[1] + m[8 + a] * z + m[12 + a];
        force[a] = -f * m[4 + a];
    }
    void* simable = Field<void*>(chassis, kCh_Owner);
    if (!Plausible((uintptr_t)simable)) return;
    void* rb = ((GetRigidBody_t)(*(uintptr_t**)simable)[0x54 / 4])(simable);
    if (!Plausible((uintptr_t)rb)) return;
    ((ResolveForce_t)(*(uintptr_t**)rb)[0x88 / 4])(rb, force, point);
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

// A pointer into the game's heap (not null, not a small number or garbage).
bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x7FFF0000; }

// The rigid body's RigidBody::Volatile (position, velocities, torque), or null.
uint8_t* RBData(uint8_t* rb) {
    uint8_t** holder = Field<uint8_t**>(rb, kRB_Data);
    return holder ? *holder : nullptr;
}

float Length(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Drop from the air (testing): requested by the menu, done in the player's next OnBeginFrame.
volatile double s_dropAt = -1;

void DropIfAsked(uint8_t* data) {
    if (s_dropAt < 0) return;
    if (Now() - s_dropAt > 2.0) {   // not in a car: forget it
        s_dropAt = -1;
        return;
    }
    if (!data) return;
    s_dropAt = -1;
    float* pos = (float*)(data + kRV_Position);
    float* vel = (float*)(data + kRV_LinearVel);
    float* av = (float*)(data + kRV_AngularVel);
    pos[1] += g_cfg.dropHeight;
    vel[1] = 0.0f;
    av[0] = av[1] = av[2] = 0.0f;
    Log("physics: car dropped from %.1f m (speed %.1f m/s)", g_cfg.dropHeight, sqrtf(vel[0] * vel[0] + vel[2] * vel[2]));
}

// The player's body: its spin limited to [Physics] MaxSpin.
void LimitSpin(uint8_t* data) {
    float limit = g_cfg.maxSpin;
    if (limit <= 0.0f || !data) return;
    float* av = (float*)(data + kRV_AngularVel);
    float w = Length(av);
    if (!(w > limit)) return;   // also skips NaN
    float k = limit / w;
    av[0] *= k;
    av[1] *= k;
    av[2] *= k;
    static double lastLog = -1e9;
    if (Now() - lastLog > 1.0) {
        lastLog = Now();
        const float* v = (const float*)(data + kRV_LinearVel);
        Log("physics: car spinning at %.1f rad/s, limited to %.1f (speed %.1f m/s, vertical %+.1f)", w, limit, Length(v), v[1]);
    }
}

// Before the player's body integrates: the torque scaled so the spin it adds stays within MaxSpin
// and one step never changes the spin by more than kMaxSpinAccel * dT.
void LimitTorque(uint8_t* rb, uint8_t* data, float dT) {
    float limit = g_cfg.maxSpin;
    if (limit <= 0.0f || !(dT > 0.0f) || !data || data[0x5D] != 0) return;   // +0x5D set: asleep, no integration
    float* T = (float*)(data + kRV_Torque);
    const float* av = (const float*)(data + kRV_AngularVel);
    const float* M = (const float*)(rb + kRB_InvWorldTensor);
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = (T[0] * M[i] + T[1] * M[4 + i] + T[2] * M[8 + i]) * dT;
    float s = 1.0f;
    float dn = Length(d);
    const char* why = nullptr;
    if (dn > kMaxSpinAccel * dT) {
        s = kMaxSpinAccel * dT / dn;
        why = "spike";
    }
    float ws[3] = {av[0] + d[0] * s, av[1] + d[1] * s, av[2] + d[2] * s};
    if (Length(ws) > limit) {
        why = "spin";
        // The largest share s of the torque with |av + s d| <= limit (|av| <= limit already).
        float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        float wd = av[0] * d[0] + av[1] * d[1] + av[2] * d[2];
        float ww = av[0] * av[0] + av[1] * av[1] + av[2] * av[2];
        float disc = wd * wd - dd * (ww - limit * limit);
        s = std::min(s, dd > 1e-12f && disc >= 0.0f ? std::max(0.0f, std::min(1.0f, (-wd + sqrtf(disc)) / dd)) : 0.0f);
    }
    if (!why) return;
    static double lastLog = -1e9;
    if (Now() - lastLog > 1.0) {
        lastLog = Now();
        float w1[3] = {av[0] + d[0], av[1] + d[1], av[2] + d[2]};
        Log("physics: torque %.0f N m (%s) would change the spin by %.1f to %.1f rad/s in one step: scaled to %.4f (step %.5f s)",
            Length(T), why, dn, Length(w1), s, dT);
    }
    for (int i = 0; i < 3; i++) T[i] *= s;
}

// A tyre on an off-road surface, by the surface's own name.
bool OffroadTire(uint8_t* tire) {
    uintptr_t layout = *(uintptr_t*)(tire + kTire_Surface + 8);
    if (!Plausible(layout)) return false;
    const char* name = *(const char**)(layout + kSurf_Name);
    if (!Plausible((uintptr_t)name) || IsBadStringPtrA(name, 32)) return false;
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
    if (!Plausible(rain)) return 0.0f;
    float v = *(float*)(rain + kRain_Intensity);
    float r = v >= 0.0f && v <= 10.0f ? v : 0.0f;
    static float logged = -1.0f;
    if (fabsf(r - logged) > 0.1f || (r > 0.0f) != (logged > 0.0f)) {
        logged = r;
        Log("physics: rain intensity %.2f", r);
    }
    return r;
}

// ABS and traction control (see the header).
struct {
    float absMul[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool absRelease[4] = {};         // ABS letting a wheel's brake off until the wheel turns with the road again
    bool absActive = false;         // ABS releasing a wheel now (the HUD icon flashes)
    float tcGas = 1.0f;              // the throttle share traction control lets through
    bool tcActive = false;
    float absLock = 0.0f, tcSpin = 0.0f;   // for the log
    double absSince = 0, tcSince = 0;     // last time each was active (the icon keeps flashing briefly)
} s_aid;

void Aids(uint8_t* susp, uint8_t* iface, float dT) {
    if (!(dT > 0.0f && dT < 0.1f)) dT = 0.01f;
    uint8_t* tranny = Field<uint8_t*>(susp, kSR_Tranny);
    float front = tranny ? Clamp(Field<float>(tranny, kTR_TorqueSplit), 0.0f, 1.0f) : 0.0f;
    float speed = s_playerSpeed;
    bool absNow = false;
    float spin = 0.0f, lockMax = 0.0f;
    for (int i = 0; i < 4; i++) {
        uint8_t* tire = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        if ((uintptr_t)tire < 0x10000 || (uintptr_t)tire >= 0x7FFF0000) continue;
        float r = Field<float>(tire, kTire_Radius), w = Field<float>(tire, kTire_AngVel), road = Field<float>(tire, kTire_RoadSpeed);
        if (!(r > 0.1f && r < 1.0f) || !(fabsf(w) < 2000.0f) || !(fabsf(road) < 200.0f)) continue;
        float surface = w * r;
        // ABS: how much slower than the road the wheel turns (1 = locked). Past 15% the brake is let
        // off quickly until the wheel is back under 5%, then it comes back on: the pulsing of a real
        // ABS, ~10 times a second, and the wheel keeps turning, so the car still steers.
        float& m = s_aid.absMul[i];
        bool& release = s_aid.absRelease[i];
        float& brake = Field<float>(tire, kTire_Brake);
        if (g_cfg.abs && brake > 0.05f && fabsf(road) > 3.0f) {
            float lock = surface * road <= 0.0f ? 1.0f : Clamp(1.0f - surface / road, 0.0f, 1.0f);
            lockMax = std::max(lockMax, lock);
            if (lock > 0.15f) release = true;
            else if (lock < 0.05f) release = false;
            m = release ? std::max(0.0f, m - 25.0f * dT) : std::min(1.0f, m + 6.0f * dT);
            brake *= m;
            if (m < 0.97f) absNow = true;
        } else {
            m = 1.0f;
            release = false;
        }
        // TC: how much faster than the road the driven wheels turn, in m/s.
        float share = i < 2 ? front : 1.0f - front;
        if (share > 0.2f && road * surface >= 0.0f) spin = std::max(spin, fabsf(surface) - fabsf(road));
    }
    if (absNow) s_aid.absSince = Now();
    s_aid.absActive = absNow;
    s_aid.absLock = lockMax;
    s_aid.tcSpin = spin;
    if (g_cfg.tc) {
        // The wheels may turn 10% faster than the road, and at least 1 m/s faster: launch control
        // from a standstill keeps that bit of slip, which is what pulls hardest, instead of none
        // (an exact 0 at a standstill held the throttle shut). Over it the throttle is taken back
        // in proportion; it never goes under 30% at a launch, so the car always pulls away.
        float allowed = std::max(1.0f, 0.10f * speed);
        float floor = speed < 5.0f ? 0.3f : 0.15f;
        if (spin > allowed) s_aid.tcGas = std::max(floor, s_aid.tcGas - std::min((spin - allowed) * 1.5f, 8.0f) * dT);
        else if (spin < 0.7f * allowed) s_aid.tcGas = std::min(1.0f, s_aid.tcGas + 1.5f * dT);
    } else {
        s_aid.tcGas = 1.0f;
    }
    s_aid.tcActive = s_aid.tcGas < 0.97f;
    if (s_aid.tcActive) s_aid.tcSince = Now();
    static double lastLog;
    if (g_cfg.logInputs && (absNow || s_aid.tcActive) && Now() - lastLog > 0.5) {
        lastLog = Now();
        Log("physics: aids: ABS %s (lock %.2f, brake x%.2f %.2f %.2f %.2f) TC %s (spin %.1f m/s, throttle x%.2f) speed %.1f",
            absNow ? "active" : "-", lockMax, s_aid.absMul[0], s_aid.absMul[1], s_aid.absMul[2], s_aid.absMul[3],
            s_aid.tcActive ? "active" : "-", spin, s_aid.tcGas, speed);
    }
}

// The ABS / TC bindings, from the input thread's press counts.
void AidToggles() {
    InputSnapshot in = Input_Get();
    static uint32_t abs = in.absToggles, tc = in.tcToggles;
    if (in.absToggles != abs) {
        abs = in.absToggles;
        g_cfg.abs = !g_cfg.abs;
        IniSave_Queue({{IniPath(), "Physics", "ABS", g_cfg.abs ? "1" : "0"}});
        Log("physics: ABS %s (binding)", g_cfg.abs ? "on" : "off");
    }
    if (in.tcToggles != tc) {
        tc = in.tcToggles;
        g_cfg.tc = !g_cfg.tc;
        IniSave_Queue({{IniPath(), "Physics", "TC", g_cfg.tc ? "1" : "0"}});
        Log("physics: traction control %s (binding)", g_cfg.tc ? "on (with launch control)" : "off");
    }
}

typedef void(__thiscall* DoDrifting_t)(void* susp, void* state);

// Slide oversteer's rear side grip factor for this step: with the game's throttle (rear or all-wheel
// drive) or lifting off (front drive), less as the rear's slip angle grows past 20 degrees, none
// from 45. On top of the speed grip loss part.
void SlideOversteer(uint8_t* susp, const uint8_t* state) {
    if (!s_latTires[2] || !s_latTires[3]) return;
    float gas = Clamp(*(const float*)(state + kState_Gas), 0.0f, 1.0f);
    uint8_t* tranny = Field<uint8_t*>(susp, kSR_Tranny);
    bool frontDrive = tranny && Field<float>(tranny, kTR_TorqueSplit) >= 0.99f;
    float power = frontDrive ? 1.0f - gas : gas;
    float angle = 0.0f;
    for (int i = 2; i < 4; i++) angle = std::max(angle, fabsf(Field<float>(s_latTires[i], kTire_SlipAngle)) * 360.0f);
    float fade = 1.0f - Smoothstep(20.0f, 45.0f, angle);
    s_latMul[2] = s_latMul[3] = s_speedMul * (1.0f - (1.0f - g_cfg.slideOversteer) * power * fade);
}

void __fastcall Hook_DoDrifting(uint8_t* susp, void*, void* state) {
    ((DoDrifting_t)kDoDrifting)(susp, state);
    if (!s_playerSusp) return;
    if (state && g_cfg.physics && !g_cfg.usePowerOversteer && g_cfg.slideOversteer != 1.0f)
        SlideOversteer(susp, (const uint8_t*)state);
    if (uint8_t* aidIface = (uint8_t*)Game_PlayerInterface((void*)kISuspensionHandle)) Aids(susp, aidIface, s_suspDT);
    if (!g_cfg.physics) return;
    float front = 1.0f + g_cfg.gripBalance, rear = 1.0f - g_cfg.gripBalance, off = g_cfg.offroadGrip;
    // Full RainGrip from a light rain on (intensity 0.3).
    float wet = 1.0f - (1.0f - g_cfg.rainGrip) * std::min(RainIntensity() / 0.3f, 1.0f);
    if (front == 1.0f && rear == 1.0f && off == 1.0f && wet == 1.0f) return;
    uint8_t* iface = (uint8_t*)Game_PlayerInterface((void*)kISuspensionHandle);
    if (!iface) return;
    for (int i = 0; i < 4; i++) {
        uint8_t* tire = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        if (!Plausible((uintptr_t)tire)) continue;
        float k = (i >= 2 ? rear : front) * wet;
        if (off != 1.0f && OffroadTire(tire)) k *= off;
        Field<float>(tire, kTire_Traction) *= k;
    }
}

void __fastcall Hook_RBBeginFrame(uint8_t* rb, void*, float dT) {
    bool player = Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle));
    uint8_t* data = player ? RBData(rb) : nullptr;
    if (data) {
        DropIfAsked(data);
        s_playerSpeed = Length((const float*)(data + kRV_LinearVel));
        LimitSpin(data);
        LimitTorque(rb, data, dT);
    }
    float k = g_cfg.gravity;
    uint8_t* specs = Field<uint8_t*>(rb, kRB_Specs);
    if (!g_cfg.physics || k == 1.0f || !specs || !(g_cfg.gravityAll ? Game_PlayerInterface(nullptr) != nullptr : player)) {
        s_origRB(rb, dT);
    } else {
        float& gravity = Field<float>(specs, 0xF0);
        float saved = gravity;
        gravity = saved * k;
        s_origRB(rb, dT);
        gravity = saved;
    }
    LimitSpin(data);   // after integrating: the next suspension step starts from a sane spin
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
    Write(kBlownTire, code, sizeof(code));
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
    {&Config::gravity, "Gravity", 0.1f, 3.0f},
    {&Config::gripBalance, "GripBalance", -0.5f, 0.5f},
    {&Config::offroadGrip, "OffroadGrip", 0.2f, 1.0f},
    {&Config::slideGrip, "SlideGrip", 0.4f, 1.0f},
    {&Config::rainGrip, "RainGrip", 0.5f, 1.0f},
    {&Config::maxSpin, "MaxSpin", 0.0f, 30.0f},
    {&Config::speedGripLoss, "SpeedGripLoss", 0.0f, 0.9f},
    {&Config::aero, "Aero", -0.9f, 0.9f},
    {&Config::powerOversteer, "PowerOversteer", 0.5f, 3.0f},
    {&Config::slideOversteer, "SlideOversteer", 0.4f, 1.0f},
};
constexpr int kNumPerCar = sizeof(kPerCar) / sizeof(kPerCar[0]);
// Keys of settings that are gone or were replaced (FrontGrip and RearGrip by GripBalance): taken
// out of a section when it is written.
const char* const kOldPerCarKeys[] = {"FrontGrip", "RearGrip", "SmokeBack"};

int PerCarIndex(float Config::*field) {
    for (int i = 0; i < kNumPerCar; i++)
        if (kPerCar[i].field == field) return i;
    return -1;
}
// The driving aids' switches (ABS, TC) are the same for every car: always [Physics], never a car's
// own section or a preset (ABS / TC keys left in [Car.<name>] or a preset are ignored).

struct {
    std::string car;            // the player's vehicle name, empty = none yet
    bool own = false;           // it has its own [Car.<name>] section
    std::string preset;         // the preset it was given (its section's Preset key), empty = none
    float global[kNumPerCar];   // the [Physics] values while a car has its own
} pc;

void StoreGlobal() {
    for (int i = 0; i < kNumPerCar; i++) pc.global[i] = g_cfg.*kPerCar[i].field;
}

void LoadGlobal() {
    for (int i = 0; i < kNumPerCar; i++) g_cfg.*kPerCar[i].field = pc.global[i];
}

// Presets: named sets of the per-car values (the sliders), one [<name>] section each
// in nfsmw_physics_presets.ini next to the gameplay ini. Read once, kept here, written through
// inisave's thread like the menu's settings.
struct Preset {
    std::string name;
    float value[kNumPerCar];
};
std::vector<Preset> s_presets;
bool s_presetsRead = false;

const char* PresetPath() {
    static char path[MAX_PATH];
    if (!path[0]) ConfigFile("nfsmw_physics_presets.ini", path, false);
    return path;
}

void ReadPresets() {
    if (s_presetsRead) return;
    s_presetsRead = true;
    std::vector<char> names(32768);
    DWORD n = GetPrivateProfileSectionNamesA(names.data(), (DWORD)names.size(), PresetPath());
    const Config def;
    for (const char* s = names.data(); s < names.data() + n && *s; s += strlen(s) + 1) {
        Preset p;
        p.name = s;
        char buf[64];
        for (int i = 0; i < kNumPerCar; i++) {
            float v = def.*kPerCar[i].field;
            if (GetPrivateProfileStringA(s, kPerCar[i].key, "", buf, sizeof(buf), PresetPath()) > 0) v = (float)atof(buf);
            p.value[i] = Clamp(v, kPerCar[i].lo, kPerCar[i].hi);
        }
        LegacyGripBalance(s, PresetPath(), &p.value[PerCarIndex(&Config::grip)], &p.value[PerCarIndex(&Config::gripBalance)]);
        s_presets.push_back(p);
    }
    Log("physics: %u presets in %s", (unsigned)s_presets.size(), PresetPath());
}

void WritePreset(const Preset& p) {
    std::vector<IniWrite> w;
    char buf[32];
    for (int i = 0; i < kNumPerCar; i++) {
        snprintf(buf, sizeof(buf), "%g", p.value[i]);
        w.push_back({PresetPath(), p.name, kPerCar[i].key, buf});
    }
    for (const char* key : kOldPerCarKeys) w.push_back(IniErase(PresetPath(), p.name, key));
    IniSave_Queue(w);
}

// The presets as the file has them now (it may have been edited by hand or by the menu's
// Overwrite since it was read). Pending writes go first.
void RereadPresets() {
    IniSave_Flush();
    s_presets.clear();
    s_presetsRead = false;
    ReadPresets();
}

const Preset* FindPreset(const std::string& name) {
    for (const Preset& p : s_presets)
        if (_stricmp(p.name.c_str(), name.c_str()) == 0) return &p;
    return nullptr;
}

}  // namespace

std::string Physics_Section() { return pc.own ? "Car." + pc.car : "Physics"; }
const char* Physics_Car() { return pc.car.c_str(); }
const char* Physics_CarPreset() { return pc.own ? pc.preset.c_str() : ""; }
bool Physics_OwnSettings() { return pc.own; }

void Physics_EraseOldKeys(std::vector<IniWrite>* writes) {
    for (const char* key : kOldPerCarKeys) writes->push_back(IniErase(IniPath(), Physics_Section(), key));
}

float Physics_EngineSmokeBack() {
    // The game's car names (its CARS folders). The 911s and the Clio V6 have it at the back, the
    // others in the middle, behind the seats.
    static const struct { const char* car; float back; } kBehind[] = {
        {"911gt2", 3.0f}, {"911turbo", 3.0f}, {"997s", 3.0f}, {"carreragt", 2.5f}, {"caymans", 2.5f},
        {"clio", 2.5f}, {"elise", 2.5f}, {"fordgt", 2.5f}, {"gallardo", 2.5f}, {"murcielago", 2.5f},
    };
    for (const auto& b : kBehind)
        if (_stricmp(pc.car.c_str(), b.car) == 0) return b.back;
    return 0.0f;
}

bool Physics_IsPerCar(const void* value) {
    for (const PerCar& p : kPerCar)
        if (value == &(g_cfg.*p.field)) return true;
    return false;
}

int Physics_PresetCount() {
    ReadPresets();
    return (int)s_presets.size();
}

const char* Physics_PresetName(int i) {
    ReadPresets();
    return i >= 0 && i < (int)s_presets.size() ? s_presets[i].name.c_str() : "";
}

int Physics_SavePreset(int i) {
    ReadPresets();
    if (i < 0 || i >= (int)s_presets.size()) {
        // A new one, named "Preset <n>" with the first free n (rename it in the presets file).
        Preset p;
        for (int n = 1;; n++) {
            p.name = "Preset " + std::to_string(n);
            bool used = false;
            for (const Preset& q : s_presets) used |= _stricmp(q.name.c_str(), p.name.c_str()) == 0;
            if (!used) break;
        }
        s_presets.push_back(p);
        i = (int)s_presets.size() - 1;
    }
    Preset& p = s_presets[i];
    for (int k = 0; k < kNumPerCar; k++) p.value[k] = g_cfg.*kPerCar[k].field;
    WritePreset(p);
    Log("physics: preset %s saved", p.name.c_str());
    return i;
}

bool Physics_LoadPreset(int i) {
    ReadPresets();
    if (i < 0 || i >= (int)s_presets.size()) return false;
    // On a known car the preset becomes that car's own settings; the shared ones are left alone.
    if (!pc.car.empty() && !pc.own) Physics_SetOwnSettings(true);
    const Preset& p = s_presets[i];
    for (int k = 0; k < kNumPerCar; k++) g_cfg.*kPerCar[k].field = p.value[k];
    // The car keeps the preset's name and takes its values afresh every time it is driven, so a
    // preset changed later reaches every car that uses it without loading it on each again.
    if (pc.own) {
        pc.preset = p.name;
        IniSave_Queue({{IniPath(), "Car." + pc.car, "Preset", p.name}});
    }
    Log("physics: preset %s loaded (%s)", p.name.c_str(), pc.own ? pc.car.c_str() : "shared settings");
    return true;
}

const char* Physics_RenamePreset(int i, const std::string& name) {
    ReadPresets();
    if (i < 0 || i >= (int)s_presets.size()) return "No preset picked";
    size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
    std::string n = a == std::string::npos ? "" : name.substr(a, b - a + 1);
    if (n.empty()) return "A name needs at least one letter";
    if (n.find_first_of("[]=;") != std::string::npos) return "Names can't have [ ] = or ;";
    for (int k = 0; k < (int)s_presets.size(); k++)
        if (k != i && _stricmp(s_presets[k].name.c_str(), n.c_str()) == 0) return "Another preset has that name";
    Preset& p = s_presets[i];
    if (p.name == n) return nullptr;
    IniSave_Queue({{PresetPath(), p.name, "", ""}});   // the old section goes, then the new one is written
    Log("physics: preset %s renamed to %s", p.name.c_str(), n.c_str());
    // The cars that use it follow the new name.
    IniSave_Flush();
    std::vector<char> names(65536);
    DWORD len = GetPrivateProfileSectionNamesA(names.data(), (DWORD)names.size(), IniPath());
    std::vector<IniWrite> links;
    char buf[256];
    for (const char* sec = names.data(); sec < names.data() + len && *sec; sec += strlen(sec) + 1)
        if (_strnicmp(sec, "Car.", 4) == 0 && GetPrivateProfileStringA(sec, "Preset", "", buf, sizeof(buf), IniPath()) > 0 &&
            _stricmp(buf, p.name.c_str()) == 0)
            links.push_back({IniPath(), sec, "Preset", n});
    if (!links.empty()) IniSave_Queue(links);
    if (_stricmp(pc.preset.c_str(), p.name.c_str()) == 0) pc.preset = n;
    p.name = n;
    WritePreset(p);
    return nullptr;
}

void Physics_DeletePreset(int i) {
    ReadPresets();
    if (i < 0 || i >= (int)s_presets.size()) return;
    IniSave_Queue({{PresetPath(), s_presets[i].name, "", ""}});
    Log("physics: preset %s deleted", s_presets[i].name.c_str());
    s_presets.erase(s_presets.begin() + i);
}

void Physics_SetCar(const char* name) {
    std::string car = name ? name : "";
    if (car == pc.car) return;
    IniSave_Flush();   // the last car's section written before this one's is read
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
        LegacyGripBalance(section.c_str(), IniPath(), &g_cfg.grip, &g_cfg.gripBalance);
    }
    pc.preset.clear();
    if (pc.own && GetPrivateProfileStringA(section.c_str(), "Preset", "", buf, sizeof(buf), IniPath()) > 0) {
        pc.preset = buf;
        RereadPresets();
        if (const Preset* p = FindPreset(pc.preset)) {
            for (int k = 0; k < kNumPerCar; k++) g_cfg.*kPerCar[k].field = p->value[k];
            Log("physics: car %s: preset %s applied", car.c_str(), p->name.c_str());
        } else {
            Log("physics: car %s: preset %s not found, its own values kept", car.c_str(), pc.preset.c_str());
        }
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
        // Queued behind any save of the car's values, so none can bring the section back.
        IniSave_Queue({{IniPath(), "Car." + pc.car, "", ""}});
        pc.own = false;
        pc.preset.clear();
        LoadGlobal();
    }
    Log("physics: %s %s", pc.car.c_str(), on ? "has its own settings" : "back to [Physics] settings");
}

float Physics_PlayerSpeed() { return s_playerSpeed; }

float Physics_TcThrottle() { return g_cfg.tc ? s_aid.tcGas : 1.0f; }

void Physics_Aids(bool* absActive, bool* tcActive) {
    *absActive = g_cfg.abs && Now() - s_aid.absSince < 0.15;
    *tcActive = g_cfg.tc && Now() - s_aid.tcSince < 0.15;
}

void Physics_DropCar() { s_dropAt = Now(); }

// The perfect launch: at the race start PVehicle keeps a launch amount (IVehicle+0xB8, from how
// well the revs were held at GO), and its slot at 0x8AA84C (0x6700E0, thiscall) turns a positive
// one into a 4.5 s boost (IVehicle+0xB4 = 4.5) and fires EPerfectLaunch; only for the player's
// car (driver class 0 at +0x94; others get it cleared). With the physics mod on the amount is
// cleared first, so no launch boost: the start is down to the car, the throttle and TC.
constexpr uintptr_t kSlotPerfectLaunch = 0x8AA84C;
constexpr uintptr_t kPerfectLaunch = 0x6700E0;
constexpr int kIV_LaunchAmount = 0xB8;

void __fastcall Hook_PerfectLaunch(uint8_t* vehicle, void* edx) {
    float& amount = Field<float>(vehicle, kIV_LaunchAmount);
    if (g_cfg.physics && amount > 0.0f) {
        Log("physics: perfect launch (%.2f) dropped", amount);
        amount = 0.0f;
    }
    ((void(__fastcall*)(uint8_t*, void*))kPerfectLaunch)(vehicle, edx);
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
    InstallBlownTire();
    if (Expect(kSlideFriction, {0xD8, 0x8E, 0xEC, 0x00, 0x00, 0x00})) {
        WriteCallNops(kSlideFriction, 6, (void*)&SlideFrictionStub);
        Log("physics: slide grip ready (%.2f)", g_cfg.slideGrip);
        if (Expect(kLateralGrip, {0xD9, 0x81, 0xF0, 0x00, 0x00, 0x00})) {
            WriteCallNops(kLateralGrip, 6, (void*)&LateralGripStub);
            Log("physics: speed grip loss ready (%.2f, %s)", g_cfg.speedGripLoss, g_cfg.useAero ? "off: Aero in use" : "in use");
            Log("physics: slide oversteer ready (%.2f, %s)", g_cfg.slideOversteer,
                g_cfg.usePowerOversteer ? "off: power oversteer in use" : "in use");
        }
    } else {
        Log("physics: slide grip unavailable");
    }
    if (Expect(kPerfectLaunch + 0x41, {0xD9, 0x86, 0xB8, 0x00, 0x00, 0x00}) &&
        SetSlot(kSlotPerfectLaunch, kPerfectLaunch, (void*)&Hook_PerfectLaunch)) {
        Log("physics: perfect launch %s", g_cfg.physics ? "off" : "as the game's (physics mod off)");
    } else {
        Log("physics: unexpected vehicle code, the perfect launch stays");
    }
    if (Expect(kEllipse, {0xD8, 0x0D, 0x78, 0xAC, 0x8A, 0x00}) && Expect(kEllipseUndo, {0xD8, 0x0D, 0x64, 0x6D, 0x89, 0x00})) {
        WriteCallNops(kEllipse, 6, (void*)&EllipseStub);
        WriteCallNops(kEllipseUndo, 6, (void*)&EllipseUndoStub);
        Log("physics: power oversteer ready (%.2f x the game's tyre force ellipse %.2f, %s)", g_cfg.powerOversteer,
            *(const float*)kEllipseRatio, g_cfg.usePowerOversteer ? "in use" : "off: slide oversteer in use");
    } else {
        Log("physics: unexpected tyre code, no power oversteer");
    }
    if (RetargetCall(kCallDoAero, kDoAerodynamics, (void*)&Hook_DoAerodynamics))
        Log("physics: aero ready (%+.2f)", g_cfg.aero);
    else
        Log("physics: unexpected suspension code, no aero setting");
    if (RetargetCall(kCallDoDrifting, kDoDrifting, (void*)&Hook_DoDrifting)) {
        Log("physics: grip balance ready (balance %+.2f, off-road grip %.2f)", g_cfg.gripBalance, g_cfg.offroadGrip);
    } else {
        Log("physics: grip balance unavailable");
    }
    Log("physics: %s (traction control %.0f%%, stability %.0f%%, drift assist %.0f%%, grip %.2f, "
        "gravity %.2f%s)", g_cfg.physics ? "on" : "off", g_cfg.tractionControl * 100, g_cfg.stability * 100,
        g_cfg.driftAssist * 100, g_cfg.grip, g_cfg.gravity, g_cfg.gravityAll ? " all cars" : "");
}

void Physics_Frame() {
    AidToggles();
    // The drift tables are built by static constructors; read them once the game runs.
    if (!g.ok) {
        static bool tried;
        if (!tried) {
            tried = true;
            CaptureTables();
        }
    }
}
