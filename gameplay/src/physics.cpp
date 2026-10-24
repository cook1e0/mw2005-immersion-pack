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
// grip than at its peak, so a rear that lets go slides the car instead of tightening the turn. The
// sliding friction drives the wheel as well as holding it sideways, so it is weighted by the share of
// the tyre's skid that is sideways: a wheel spinning at a launch or locked under braking keeps the
// game's friction (with all of it, launches spun up and stayed spinning).
// Speed grip loss (for comparing with Aero, to go): after the grip limit, UpdateLoaded multiplies
// the tyre's side force by its surface's LATERAL_GRIP (0x69E098); for the player's tyres that also
// takes 1 - [Physics] SpeedGripLoss * (speed / 300 km/h)^2, so a car pulls away and brakes as
// before but corners on less grip the faster it goes.
// Friction circle: a tyre has one grip budget for driving and cornering together. The game only
// keeps to it at the limit, and only in its gripping model (UpdateLoaded weighs the drive force by
// TireForceEllipseRatio, 1.5, against the budget); its sliding model takes the forces from the
// dynamic friction along the skid, where the engine's torque doesn't count until it has spun the
// wheel up, so under power the side grip came and went with the model in use. For the player's
// tyres the side force (at the LATERAL_GRIP multiply below) is now held within
// budget * sqrt(1 - u^2) in both models, u being the drive torque the wheel gets this step (Tire
// +0xE0; the game's own drive force in its gripping model is (it + the brake torque) / radius,
// 0x69DD8B) over its radius, as a share of the budget: full throttle in a low gear (u near 1) leaves a driven tyre
// little side grip, at speed in a high gear little changes. [Physics] FrictionCircle blends it in
// (0 = off, 1 = the whole circle; above 1 the drive torque counts that many times over, to test). Rear drive steps out under power, front drive pushes wide.
// Aero: Chassis::DoAerodynamics (called from SuspensionRacer::OnTaskSimulate) pushes the car down
// with the game's own downforce: speed * AERO_COEFFICIENT * 2000 (Physics::Info::
// AerodynamicDownforce), times the Aerodynamics tuning, at a point between the axles set by
// AERO_CG, only while it is positive. It is large: the Mustang's is 2.4 times its weight at
// 300 km/h, which is most of the game's grip at speed (the grip limit is load * grip). That call is
// wrapped, and for the player's car the downforce becomes [Physics] Downforce times the game's own:
// the difference is added the same way at the same point (Chassis::State: matrix +0, linear_vel
// +0x50, ground_effect +0x6C, cog +0x70, mass +0xAC, speed +0xBC; IRigidBody::ResolveForce(force,
// point), vtable +0x88). Below 1 less, below 0 lift. It loads or unloads the tyres like the game's
// own, so the grip, the suspension and the steering weight all follow. The game's grows in a straight
// line with the speed (already 41% of the Mustang's weight at 50 km/h); [Physics] DownforceSquared
// also multiplies it by speed / DownforceMatchSpeed, so it grows with the square of the speed like
// real aero and equals Downforce times the game's at that speed.
//
// Load sensitivity: UpdateLoaded's grip limit (the budget the tyre's force is cut to) is its load
// times its grip, exactly in proportion (built at 0x69DFAE..0x69DFD2: +0x12C * traction boost *
// STATIC_GRIP * load (+0xD4) * pilot factor). For the player's tyres it is then multiplied by
// (load / static load)^-[Physics] LoadSensitivity, the static load being the tyre's share of the
// car's weight (mass, gravity, FRONT_WEIGHT_BIAS at chassis layout +0x6C): at rest nothing changes,
// a tyre loaded more gets less than proportionally more grip. So load moved across an axle in a
// corner costs that axle grip (anti-roll bars and springs set the balance), a heavier axle has less
// grip for its weight (rear-engined cars are looser at the rear), and downforce glues less.
// Past-peak grip: the game's cornering force rises with the slip angle (tables per 2 degrees and
// load, 0x8F8A70) and stays flat from 12 degrees on; a real tyre peaks and then slides on less.
// The player's tyres' side force (the LATERAL_GRIP multiply) takes 1 - (1 - [Physics] PastPeakGrip)
// * Smoothstep(8, 25 degrees, the slip angle), not on loose surfaces (grass, dirt...), where the
// force keeps rising as the tyre ploughs.
// Suspension firmness: DoWheelForces reads springs (+0x38), anti-roll bars (+0x48) and dampers
// (bump +0x30, rebound +0x28), front and rear, from the chassis layout at SuspensionRacer +0xC8.
// For the player's step they are scaled by [Physics] SuspensionFirmness (dampers by its square
// root, keeping the damping ratio), and put back after. [Physics] RollBalance moves the anti-roll
// bars' stiffness between the axles (front times 1 + it, rear times 1 - it): with load sensitivity,
// the stiffer end takes more of the load moved across in a corner and lets go first.
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
// Tire: the wheel's slip along it this step (m/s), wheel speed (angular velocity * radius) less the
// ground's, stored by UpdateLoaded (0x69DCB1) before the sliding friction.
constexpr int kTire_Slip = 0xF0;
// Tire: the share of its force it kept at the last step (1, or the grip limit / the force when that
// was over it, set at 0x69DFB4 / 0x69E004). Below 1, UpdateLoaded uses the sliding model this step.
constexpr int kTire_GripUsed = 0x110;
constexpr uintptr_t kCallDoDrifting = 0x6AA006;     // TuneWheelParams: call SuspensionRacer::DoDrifting(state)
constexpr uintptr_t kDoDrifting = 0x6A9760;         // thiscall, 1 arg
constexpr uintptr_t kPlayerRain = 0x9196B8;         // Rain* of the player view (eViews[1] + 0x68)
constexpr int kRain_Intensity = 0x4C;
constexpr uintptr_t kSlideFriction = 0x69DD22;      // UpdateLoaded: fmul [esi+0xEC] for the sliding friction
constexpr int kTire_SlipAngle = 0x118;
constexpr int kTire_Load = 0xD4;                    // ... load (N) for this step (0 off the ground)
constexpr int kTire_SideForce = 0xD8;               // ... side force this step (N), cut to the grip limit
// ... the drivetrain's torque on the wheel this step (N m): UpdateLoaded scales it by 1 - handbrake
// (0x69DBD5, the handbrake declutches); the brake torque is at +0xE4.
constexpr int kTire_DriveTorque = 0xE0;
constexpr uintptr_t kGripLimit = 0x69DFCE;          // UpdateLoaded: fmul [esp+0x1C]; fstp [esp+0x1C] (the grip limit)
constexpr int kSR_SuspLayout = 0xC8;                // SuspensionRacer: chassis layout DoWheelForces reads
// Chassis layout AxlePairs (front, rear): rebound and bump damping, springs, anti-roll bars.
constexpr int kChL_ShockExt = 0x28, kChL_Shock = 0x30, kChL_Spring = 0x38, kChL_Swaybar = 0x48;
constexpr int kChL_WeightBias = 0x6C;               // FRONT_WEIGHT_BIAS, percent
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
constexpr float kAeroRefSpeed = 300.0f / 3.6f;      // SpeedGripLoss is its full value here

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
// Each player tyre's grip budget at its last UpdateLoaded (the grip limit, after load sensitivity).
extern "C" float s_budget[4];
float s_budget[4];
float s_gas = 0.0f;         // the player's throttle at the last DoDrifting
float s_refLoad[2];         // a front / rear tyre's static load (N), for load sensitivity
float s_lsRatio[4], s_lsFactor[4], s_peakFactor[4], s_peakAngle[4];   // the last step's, for the log
// The player's suspension and aero steps this second, for the diagnostics line ([Physics] LogPhysics).
struct {
    int suspSteps = 0, suspCustom = 0, aeroSteps = 0;   // the player's suspension steps (own values), aero steps
    float gameForce = 0, extraForce = 0, weight = 0;    // N at the last aero step
} s_diag;
// The friction circle this second, per axle (front, rear), over the steps a tyre was driven: the
// drive torque's share of its grip (mean, highest), how many steps its side force was held to the
// circle, and the side force it kept (1 when not held), for [Physics] LogPhysics.
struct CircleStats {
    int steps = 0, held = 0;
    float use = 0, useMax = 0, keep = 0;
} s_circle[2];
// Slide grip this second, per axle (front, rear), over the steps a player's tyre used the sliding
// model: the factor it took (SlideGrip by the sideways share of the skid) and its wheel slip, for
// [Physics] LogPhysics.
struct SlideStats {
    int steps = 0;
    float factor = 0, min = 1, spin = 0;
} s_slide[2];


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
bool Plausible(uintptr_t p);

struct Saved {
    float grip, slip, yaw;
    uint8_t* susp = nullptr;    // the chassis layout whose springs, dampers and bars were scaled
    float suspVals[8];
};

const int kSuspOffsets[4] = {kChL_ShockExt, kChL_Shock, kChL_Spring, kChL_Swaybar};

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
    uint8_t* layout = Field<uint8_t*>(susp, kSR_SuspLayout);
    if ((c.suspFirmness != 1.0f || c.rollBalance != 0.0f) && Plausible((uintptr_t)layout)) {
        sv.susp = layout;
        float damp = sqrtf(c.suspFirmness);
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 2; a++) {
                float& v = Field<float>(layout, kSuspOffsets[k] + a * 4);
                sv.suspVals[k * 2 + a] = v;
                v *= k < 2 ? damp : c.suspFirmness;
                // Roll balance: the anti-roll bars' stiffness moved to the front (+) or the rear (-).
                if (kSuspOffsets[k] == kChL_Swaybar) v *= a == 0 ? 1.0f + c.rollBalance : 1.0f - c.rollBalance;
            }
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
    if (sv.susp)
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 2; a++) Field<float>(sv.susp, kSuspOffsets[k] + a * 4) = sv.suspVals[k * 2 + a];
}

bool Stock() {
    const Config& c = g_cfg;
    return (c.tractionControl >= 1.0f || c.tc) && c.stability >= 1.0f && c.driftAssist >= 1.0f && c.grip == 1.0f &&
           c.slideGrip == 1.0f && (c.useAero || c.speedGripLoss == 0.0f) &&
           c.frictionCircle == 0.0f && c.loadSensitivity == 0.0f &&
           c.pastPeakGrip >= 1.0f && c.suspFirmness == 1.0f && c.rollBalance == 0.0f;
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
    if (player) s_suspDT = dT, s_diag.suspSteps++;
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
        uint8_t* layout = Field<uint8_t*>(susp, kSR_SuspLayout);
        if (Plausible((uintptr_t)layout)) {
            auto ax = [&](int off, int a) { return Field<float>(layout, off + a * 4); };
            Log("physics: suspension front / rear: springs %.0f / %.0f lb/in, anti-roll bars %.0f / %.0f, damping bump %.0f / %.0f, "
                "rebound %.0f / %.0f; weight %.0f%% front; firmness %.2f, roll balance %+.2f", ax(kChL_Spring, 0),
                ax(kChL_Spring, 1), ax(kChL_Swaybar, 0), ax(kChL_Swaybar, 1), ax(kChL_Shock, 0), ax(kChL_Shock, 1),
                ax(kChL_ShockExt, 0), ax(kChL_ShockExt, 1), Field<float>(layout, kChL_WeightBias), g_cfg.suspFirmness,
                g_cfg.rollBalance);
        }
    }
    s_diag.suspCustom++;
    Saved sv;
    Apply(susp, sv);
    s_slideGrip = g_cfg.slideGrip;
    // The player's tyres, for the side grip stub.
    s_speedMul = !g_cfg.useAero && g_cfg.speedGripLoss > 0.0f ? SpeedGrip(g_cfg.speedGripLoss, s_playerSpeed) : 1.0f;
    for (int i = 0; i < 4; i++) {
        s_latTires[i] = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        s_latMul[i] = s_speedMul;
        s_budget[i] = 0.0f;
    }
    s_origSusp(susp, dT);
    for (uint8_t*& t : s_latTires) t = nullptr;
    s_slideGrip = 1.0f;
    Restore(sv);
    s_playerSusp = false;
}

// The slide grip factor for a sliding tyre: SlideGrip by the sideways share of its skid, so a tyre
// sliding sideways takes all of it and one spinning or locked in a straight line (a launch, a stop)
// keeps the game's friction. The skid speed is UpdateLoaded's sqrt(lat^2 + slip^2), the slip the
// tyre's own (+0xF0, stored just before); if they don't agree the stack isn't what it was read as,
// and the whole factor applies, as before.
extern "C" __attribute__((used)) float SlideFriction(uint8_t* tire, float lat, float skid) {
    int at = -1;
    for (int i = 0; i < 4; i++)
        if (s_latTires[i] == tire) at = i;
    if (at < 0) return 1.0f;
    bool sliding = Field<float>(tire, kTire_GripUsed) < 1.0f;
    float slip = Field<float>(tire, kTire_Slip);
    float want = sqrtf(lat * lat + slip * slip);
    if (!(fabsf(want - skid) <= 0.01f * want + 0.01f)) {
        static bool logged;
        if (!logged) {
            logged = true;
            Log("physics: slide grip: skid speed %.3f, expected %.3f (lateral %.3f, slip %.3f); not weighted by direction",
                skid, want, lat, slip);
        }
        return s_slideGrip;
    }
    float share = skid > 0.001f ? Clamp(fabsf(lat) / skid, 0.0f, 1.0f) : 0.0f;
    float f = 1.0f - (1.0f - s_slideGrip) * share;
    if (sliding) {
        SlideStats& s = s_slide[at >= 2];
        s.steps++, s.factor += f, s.spin += fabsf(slip);
        if (f < s.min) s.min = f;
    }
    return f;
}

// In place of UpdateLoaded's fmul [esi+0xEC] (6 bytes: a call and a nop): the same multiply, then
// with SlideGrip below 1 times SlideFriction(tyre, lateral speed, skid speed). Past UpdateLoaded's
// push ecx just before and the return address, its esp+X is this stub's esp+X+8: the lateral speed
// (its first argument) at 0x2C, the skid speed at 0x10. eax, ecx and edx are kept, and the product is
// held in memory over the call (the x87 stack is otherwise empty); the flags are free.
__asm__(
    ".text\n"
    "_SlideFrictionStub:\n"
    "    fmuls 0xEC(%esi)\n"
    "    cmpl $0x3F800000, _s_slideGrip\n"
    "    je 1f\n"
    "    subl $4, %esp\n"
    "    fstps (%esp)\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    // 24 bytes down from UpdateLoaded's esp: skid speed at 0x28, then (one push more) lateral at 0x48.
    "    pushl 0x28(%esp)\n"
    "    pushl 0x48(%esp)\n"
    "    pushl %esi\n"
    "    call _SlideFriction\n"
    "    addl $12, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    fmuls (%esp)\n"
    "    addl $4, %esp\n"
    "1:  ret\n");
extern "C" void SlideFrictionStub();

// In place of UpdateLoaded's fld [ecx+0xF0] (6 bytes): the same load, times LatGrip (s_latMul, the
// past-peak loss and the friction circle) for a tyre in s_latTires. The x87 stack is empty here; eax,
// ecx and edx are kept; the flags are free (the next test sets them again).
__asm__(
    ".text\n"
    "_LateralGripStub:\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    flds 0xF0(%ecx)\n"
    "    ret\n"
    // The grip budget: UpdateLoaded's local at esp+0x18 here, 12 bytes further down with the return
    // address and the two pushes.
    "2:  pushl %edx\n"
    "    movl 0x24(%esp), %edx\n"
    "    movl %edx, _s_budget(,%eax,4)\n"
    "    pushl %ecx\n"
    "    pushl %eax\n"
    "    pushl %eax\n"
    "    call _LatGrip\n"
    "    addl $4, %esp\n"
    "    popl %eax\n"
    "    popl %ecx\n"
    "    popl %edx\n"
    "    fmuls 0xF0(%ecx)\n"
    "    popl %eax\n"
    "    ret\n");
extern "C" void LateralGripStub();

bool OffroadTire(uint8_t* tire);

// Load sensitivity, for a player's tyre: its grip limit times (load / static load)^-LoadSensitivity.
extern "C" __attribute__((used)) void GripLimit(uint8_t* tire, float* limit) {
    float k = g_cfg.loadSensitivity;
    if (k <= 0.0f) return;
    for (int i = 0; i < 4; i++) {
        if (s_latTires[i] != tire) continue;
        float ref = s_refLoad[i >= 2], load = Field<float>(tire, kTire_Load);
        if (ref > 1.0f && load > 0.0f && *limit > 0.0f) {
            float r = Clamp(load / ref, 0.05f, 20.0f), f = powf(r, -k);
            *limit *= f;
            s_lsRatio[i] = r, s_lsFactor[i] = f;
        }
        return;
    }
}

// In place of the grip limit's last multiply and store (8 bytes): the same, then GripLimit on the
// stored value (UpdateLoaded's esp+0x1C there, 0x20 past the return address). The x87 stack is
// empty after the store; eax, ecx and edx are kept.
__asm__(
    ".text\n"
    "_GripLimitStub:\n"
    "    fmuls 0x20(%esp)\n"
    "    fstps 0x20(%esp)\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    leal 0x2C(%esp), %eax\n"
    "    pushl %eax\n"
    "    pushl %esi\n"
    "    call _GripLimit\n"
    "    addl $8, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    ret\n");
extern "C" void GripLimitStub();

// A player's tyre on a loose surface (no past-peak loss there), by its surface, cached per tyre.
bool LooseSurface(int i, uint8_t* tire) {
    static uintptr_t surface[4];
    static bool loose[4];
    uintptr_t s = *(uintptr_t*)(tire + kTire_Surface + 8);
    if (s != surface[i]) surface[i] = s, loose[i] = OffroadTire(tire);
    return loose[i];
}

// The past-peak loss for a player's tyre's slip angle (fading in from 2 to 5 m/s, where the angle
// means something).
float PastPeak(int i, uint8_t* tire) {
    float keep = g_cfg.pastPeakGrip;
    float a = fabsf(Field<float>(tire, kTire_SlipAngle)) * 360.0f;
    if (!(a <= 180.0f)) return 1.0f;        // NaN or nonsense
    if (a > 90.0f) a = 180.0f - a;           // rolling backwards
    float f = 1.0f - (1.0f - keep) * Smoothstep(8.0f, 25.0f, a) * Smoothstep(2.0f, 5.0f, s_playerSpeed);
    if (f < 1.0f && LooseSurface(i, tire)) f = 1.0f;
    s_peakFactor[i] = f, s_peakAngle[i] = a;
    return f;
}

// The friction circle for a player's tyre: the factor that holds its side force (after the game's
// cut and `m`) within budget * room. u is the drive torque's share of the grip (torque / radius /
// budget; none while it holds the car back, engine braking), and room = sqrt(1 - u^2). A wheel
// that spins (its slip from 0.5 to 2 m/s) has its force along its skid: its side share is the
// lateral speed over the skid speed (the lateral speed from the slip angle and the ground speed
// along the wheel, angular velocity * radius - the slip), and room is at least that, so a car
// sliding further sideways gets side grip back and a power slide can be held. FrictionCircle up to
// 1 blends room from 1 (0) to all of it (1); above 1 the drive torque counts that many times over
// (an exaggeration, for testing).
float Circle(int i, uint8_t* tire, float m) {
    float budget = s_budget[i], radius = Field<float>(tire, kTire_Radius);
    if (!(budget > 0.0f) || !(radius > 0.0f)) return 1.0f;
    float slip = Field<float>(tire, kTire_Slip);
    float fwd = Field<float>(tire, kTire_AngVel) * radius - slip;
    float torque = Field<float>(tire, kTire_DriveTorque);
    if (torque * fwd < 0.0f && fabsf(fwd) > 0.5f) return 1.0f;
    float k = g_cfg.frictionCircle;
    float u = fabsf(torque) / radius / budget * std::max(k, 1.0f);
    if (!(u > 0.02f)) return 1.0f;           // not driven, or NaN
    float room = sqrtf(std::max(0.0f, 1.0f - u * u));
    float spin = Smoothstep(0.5f, 2.0f, fabsf(slip));
    if (spin > 0.0f) {
        float angle = Clamp(fabsf(Field<float>(tire, kTire_SlipAngle)) * 6.2831853f, 0.0f, 1.55f);
        float lat = tanf(angle) * fabsf(fwd);
        float skid = sqrtf(lat * lat + slip * slip);
        room = std::max(room, lat / skid * spin);
    }
    float cap = budget * (1.0f - std::min(k, 1.0f) * (1.0f - room));
    float side = fabsf(Field<float>(tire, kTire_SideForce)) * m;
    float f = side > cap ? cap / side : 1.0f;
    CircleStats& c = s_circle[i >= 2];
    c.steps++, c.use += std::min(u, 9.0f), c.keep += f;
    c.useMax = std::max(c.useMax, u);
    if (f < 1.0f) c.held++;
    return f;
}

// The side grip factor for a player's tyre this step: s_latMul, times the past-peak loss and the
// friction circle.
extern "C" __attribute__((used)) float LatGrip(int i) {
    float m = s_latMul[i];
    uint8_t* tire = s_latTires[i];
    if (!tire) return m;
    if (g_cfg.pastPeakGrip < 1.0f) m *= PastPeak(i, tire);
    if (g_cfg.frictionCircle > 0.0f) m *= Circle(i, tire, m);
    return m;
}

bool Plausible(uintptr_t p);

typedef void(__thiscall* DoAerodynamics_t)(void* chassis, const uint8_t* state, float drag, float aero, float frontZ,
                                           float rearZ, const uint8_t* tunings);
typedef void*(__thiscall* GetRigidBody_t)(void* simable);
typedef void(__thiscall* ResolveForce_t)(void* rb, const float* force, const float* point);

// The game's own aerodynamics, then for the player's car the difference to [Physics] Downforce
// times the game's own downforce (as DoAerodynamics computes it, at the same point), times
// speed / DownforceMatchSpeed with DownforceSquared.
void __fastcall Hook_DoAerodynamics(uint8_t* chassis, void*, const uint8_t* state, float drag, float aero, float frontZ,
                                    float rearZ, const uint8_t* tunings) {
    ((DoAerodynamics_t)kDoAerodynamics)(chassis, state, drag, aero, frontZ, rearZ, tunings);
    if (!s_playerSusp || !g_cfg.physics || !state) return;
    s_diag.aeroSteps++;
    const float* m = (const float*)state;   // rows: right, up, forward, position (4 floats each)
    uint8_t* layout = Field<uint8_t*>(chassis, kCh_Layout);
    if (!Plausible((uintptr_t)layout)) return;
    float mass = *(const float*)(state + kSt_Mass), speed = *(const float*)(state + kSt_Speed);
    float tuning = tunings ? *(const float*)(tunings + kTunings_Aero) : 0.0f;
    float coef = Field<float>(layout, kChL_AeroCoef), aeroCg = Field<float>(layout, kChL_AeroCg);
    s_diag.weight = mass * 9.81f;
    // Each tyre's static load, for load sensitivity: its share of the car's weight.
    float bias = Field<float>(layout, kChL_WeightBias) * 0.01f;
    if (!(bias > 0.2f && bias < 0.8f)) bias = 0.5f;
    float weight = mass * 9.81f * (g_cfg.gravity > 0.0f ? g_cfg.gravity : 1.0f);
    s_refLoad[0] = weight * bias * 0.5f, s_refLoad[1] = weight * (1.0f - bias) * 0.5f;

    // The car's own aero, logged once per car: its downforce at 300 km/h as a share of its weight.
    static uint8_t* logged;
    if (logged != chassis && mass > 1.0f) {
        logged = chassis;
        float game = kAeroRefSpeed * coef * 2000.0f * (1.0f + 0.25f * tuning);
        float ours = game * g_cfg.downforce * (g_cfg.downforceSquared ? kAeroRefSpeed / (g_cfg.downforceMatch / 3.6f) : 1.0f);
        Log("physics: aero: the game's downforce at 300 km/h is %.0f%% of the car's weight, linear in speed "
            "(AERO_COEFFICIENT %.4f, AERO_CG %.0f%%, tuning %+.0f, mass %.0f kg); Downforce %.2f%s makes it %.0f%%",
            game / (mass * 9.81f) * 100.0f, coef, aeroCg, tuning, mass, g_cfg.downforce,
            g_cfg.downforceSquared ? " (with the speed squared)" : "", ours / (mass * 9.81f) * 100.0f);
    }
    s_diag.gameForce = s_diag.extraForce = 0.0f;
    if (aero <= 0.0f || mass <= 1.0f || speed < 1.0f) return;
    // The game's downforce, as DoAerodynamics has it: less when tilted with fewer than two wheels
    // down, none going backwards, 0.8 in the air, the Aerodynamics tuning a quarter per step.
    float ground = *(const float*)(state + kSt_Ground);
    float upness = ground >= 0.5f ? 1.0f : std::max(m[5], 0.0f);
    const float* vel = (const float*)(state + kSt_LinearVel);
    float forwardness = std::max((vel[0] * m[8] + vel[1] * m[9] + vel[2] * m[10]) / speed, 0.0f);
    forwardness = std::max(*(const float*)kAeroDropOffMin, powf(forwardness, *(const float*)kAeroDropOff));
    float game = aero * upness * forwardness * speed * coef * 2000.0f;
    if (ground == 0.0f) game *= 0.8f;
    game += game * 0.25f * tuning;
    if (game <= 0.0f) return;
    s_diag.gameForce = game;
    if (!g_cfg.useAero || (g_cfg.downforce == 1.0f && !g_cfg.downforceSquared)) return;
    float shape = g_cfg.downforceSquared ? speed / (g_cfg.downforceMatch / 3.6f) : 1.0f;
    float f = (g_cfg.downforce * shape - 1.0f) * game;   // + more down, - less (below 0 Downforce: lift)
    s_diag.extraForce = f;
    // At the game's aero point: the car's centre of gravity, moved along it by AERO_CG between the
    // axles while a wheel is down.
    const float* cog = (const float*)(state + kSt_Cog);
    float z = ground != 0.0f ? (frontZ - rearZ) * (aeroCg * 0.01f) + rearZ : cog[2];
    float point[3], force[3];
    for (int k = 0; k < 3; k++) {
        point[k] = m[k] * cog[0] + m[4 + k] * cog[1] + m[8 + k] * z + m[12 + k];
        force[k] = -f * m[4 + k];
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

void __fastcall Hook_DoDrifting(uint8_t* susp, void*, void* state) {
    ((DoDrifting_t)kDoDrifting)(susp, state);
    if (!s_playerSusp) return;
    // The game's throttle, for the diagnostics.
    if (state) s_gas = Clamp(*(const float*)((const uint8_t*)state + kState_Gas), 0.0f, 1.0f);
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

// Once a second for the player's car: whether its suspension steps reach this mod (a car it can't
// see gets none of the physics settings), and with [Physics] LogPhysics the aero forces and grip
// factors at that moment.
void Diagnostics(uint8_t* rb) {
    static double last;
    static uint8_t* warnedCar;
    double now = Now();
    if (now - last < 1.0) return;
    last = now;
    if (s_diag.suspSteps == 0 && g_cfg.physics && warnedCar != rb && s_playerSpeed > 2.0f) {
        warnedCar = rb;
        Log("physics: the player's car moves but its suspension steps don't reach the mod: no physics settings on it");
    }
    if (g_cfg.logPhysics && s_playerSpeed > 5.0f)
        Log("physics: %3.0f km/h: suspension %d steps (%d with the mod's values), aero %d; game downforce %.0f%% of the "
            "weight, ours %+.0f%%; side grip x%.2f/%.2f",
            s_playerSpeed * 3.6f, s_diag.suspSteps, s_diag.suspCustom, s_diag.aeroSteps,
            s_diag.weight > 0 ? s_diag.gameForce / s_diag.weight * 100.0f : 0.0f,
            s_diag.weight > 0 ? s_diag.extraForce / s_diag.weight * 100.0f : 0.0f, s_latMul[0], s_latMul[2]);
    for (int a = 0; a < 2; a++) {
        const CircleStats& c = s_circle[a];
        if (!g_cfg.logPhysics || c.steps == 0) continue;
        float n = (float)c.steps;
        Log("physics:   %s driven (%d tyre steps, throttle %.2f): drive torque %.0f%% of the grip (up to %.0f%%); "
            "side force held to the friction circle %.0f%% of the steps, kept x%.2f",
            a ? "rear" : "front", c.steps, s_gas, c.use / n * 100.0f, c.useMax * 100.0f, c.held / n * 100.0f, c.keep / n);
    }
    if (g_cfg.logPhysics && s_playerSpeed > 5.0f && (g_cfg.loadSensitivity > 0.0f || g_cfg.pastPeakGrip < 1.0f))
        Log("physics:   tyres FL FR RL RR: load %.2f %.2f %.2f %.2f of static, grip limit x%.2f %.2f %.2f %.2f; "
            "slip %.0f %.0f %.0f %.0f deg, side grip x%.2f %.2f %.2f %.2f", s_lsRatio[0], s_lsRatio[1], s_lsRatio[2],
            s_lsRatio[3], s_lsFactor[0], s_lsFactor[1], s_lsFactor[2], s_lsFactor[3], s_peakAngle[0], s_peakAngle[1],
            s_peakAngle[2], s_peakAngle[3], s_peakFactor[0], s_peakFactor[1], s_peakFactor[2], s_peakFactor[3]);
    for (int a = 0; a < 2; a++) {
        const SlideStats& s = s_slide[a];
        if (!g_cfg.logPhysics || s.steps == 0) continue;
        float n = (float)s.steps;
        Log("physics:   %s sliding (%d tyre steps, %.0f km/h): slide grip x%.2f (min x%.2f) of %.2f, wheel slip %.1f m/s",
            a ? "rear" : "front", s.steps, s_playerSpeed * 3.6f, s.factor / n, s.min, g_cfg.slideGrip, s.spin / n);
    }
    for (SlideStats& s : s_slide) s = SlideStats();
    for (CircleStats& c : s_circle) c = CircleStats();
    s_diag.suspSteps = s_diag.suspCustom = s_diag.aeroSteps = 0;
}

void __fastcall Hook_RBBeginFrame(uint8_t* rb, void*, float dT) {
    bool player = Owns(rb, Game_PlayerInterface((void*)kIRigidBodyHandle));
    uint8_t* data = player ? RBData(rb) : nullptr;
    if (data) {
        DropIfAsked(data);
        s_playerSpeed = Length((const float*)(data + kRV_LinearVel));
        Diagnostics(rb);
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
    {&Config::downforce, "Downforce", -0.25f, 2.0f},
    {&Config::suspFirmness, "SuspensionFirmness", 0.5f, 2.0f},
    {&Config::rollBalance, "RollBalance", -0.5f, 0.5f},
};
constexpr int kNumPerCar = sizeof(kPerCar) / sizeof(kPerCar[0]);
// Keys of settings that are gone or were replaced (FrontGrip and RearGrip by GripBalance): taken
// out of a section when it is written.
const char* const kOldPerCarKeys[] = {"FrontGrip", "RearGrip", "SmokeBack", "Aero", "PowerOversteer", "SlideOversteer"};

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

float Physics_GasInput() { return s_gas; }

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

// Wall bounce: RigidBody::ResolveWorldCollision (0x699410) sets the impact's elasticity (Moment
// +0x10C) from the car's WALL_ELASTICITY, then calls ModifyCollision for the world (the RBVehicle
// one, 0x68BD60, through the vtable slot 0x8AC96C and the same slot of three vehicle classes
// built on it, RBTractor's among them), which for the player's car already makes a
// head-on hit (Tweak_HeadOnDot) fully inelastic. After it, the player's car keeps [Physics]
// WallBounce of the elasticity on every wall hit: at 0 it slides along or stops at the wall instead
// of springing off. The friction, the spin from the hit and the collision reports stay the game's.
constexpr uintptr_t kSlotsWorldModify[] = {0x8AC96C, 0x8ACBDC, 0x8ACE14, 0x8AD264};
constexpr uintptr_t kWorldModify = 0x68BD60;      // thiscall (SimSurface&, Plane&, Moment&)
constexpr int kMo_Elasticity = 0x10C;
constexpr int kRBV_Behavior = 0x48;                // RBVehicle: its Behavior (vtable +4: GetOwner)

void __fastcall Hook_WorldModify(uint8_t* self, void*, const void* surface, const void* plane, uint8_t* moment) {
    ((void(__thiscall*)(void*, const void*, const void*, void*))kWorldModify)(self, surface, plane, moment);
    if (!g_cfg.physics || g_cfg.wallBounce >= 1.0f || !moment) return;
    // The player's car, by the same test the game makes here: Behavior::GetOwner()->IsPlayer().
    uint8_t* behavior = self + kRBV_Behavior;
    void* owner = ((void*(__thiscall*)(void*))(*(uintptr_t**)behavior)[1])(behavior);
    if (!Plausible((uintptr_t)owner) || !((bool(__thiscall*)(void*))(*(uintptr_t**)owner)[9])(owner)) return;
    float& e = Field<float>(moment, kMo_Elasticity);
    static int logged;
    if (g_cfg.logPhysics && logged < 5 && e > 0.0f) {
        logged++;
        Log("physics: wall hit: the game's bounce %.2f, kept %.2f", e, e * g_cfg.wallBounce);
    }
    e *= g_cfg.wallBounce;
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
            Log("physics: friction circle ready (%.2f)", g_cfg.frictionCircle);
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
    if (Expect(kGripLimit, {0xD8, 0x4C, 0x24, 0x1C, 0xD9, 0x5C, 0x24, 0x1C})) {
        WriteCallNops(kGripLimit, 8, (void*)&GripLimitStub);
        Log("physics: load sensitivity %.2f, past-peak grip %.2f, suspension firmness %.2f", g_cfg.loadSensitivity,
            g_cfg.pastPeakGrip, g_cfg.suspFirmness);
    } else {
        Log("physics: unexpected tyre code, no load sensitivity");
    }
    if (RetargetCall(kCallDoAero, kDoAerodynamics, (void*)&Hook_DoAerodynamics))
        Log("physics: downforce ready (%.2f x the game's%s, %s)", g_cfg.downforce,
            g_cfg.downforceSquared ? ", with the speed squared" : "", g_cfg.useAero ? "in use" : "off: speed grip loss in use");
    else
        Log("physics: unexpected suspension code, no aero setting");
    int slots = 0;
    for (uintptr_t slot : kSlotsWorldModify) slots += SetSlot(slot, kWorldModify, (void*)&Hook_WorldModify);
    if (slots)
        Log("physics: wall bounce %.0f%% of the game's (%d of 4 vehicle classes)", g_cfg.wallBounce * 100.0f, slots);
    else
        Log("physics: unexpected collision code, the game's wall bounce stays");
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
