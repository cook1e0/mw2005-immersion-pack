// Physics sliders for the player's car: the game's driving aids, grip and gravity.
//
// The game models front, rear and all-wheel drive properly (SuspensionRacer::DoDriveForces
// splits the torque by the transmission's TORQUE_SPLIT), but several aids hide the difference:
// - traction control: DoDriveForces cuts torque to a spinning wheel while steering, scaled by
//   Ramp(forward slip, 1, 20). The 20 is a constant in the code (push 0x41A00000 at 0x68F3F7),
//   not Tweak_TractionFowardSlip: that float (0x8AAD94) holds 20 too, but only DoInduction and
//   Chassis::DoJumpStabilizer read it. Up to v0.29.0 the mod set it to 1e9 for TC off, which left
//   the game's TC on and made the jump stabiliser's in-air torque 5e7 times too strong (the
//   billions of N m the spin limit below was catching). The code's constant is swapped instead;
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
// 0x9196B8, intensity at +0x28C). UpdateLoaded applies the boost to both the tyre's sliding friction
// and its grip limit.
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
// AERO_CG, only while it is positive. It grows in a straight line with the speed and is large: the
// Mustang's is 2.4 times its weight at 300 km/h (already 41% at 50 km/h), which is most of the
// game's grip at speed (the grip limit is load * grip). That call is wrapped, and for the player's
// car the downforce becomes [Physics] Downforce times the game's own (so each car keeps its own aero
// and its Aerodynamics tuning), and with [Physics] DownforceSquared (the default) times
// speed / DownforceMatchSpeed too, so it grows with the square of the speed like real aero and equals
// Downforce times the game's at that speed (250 km/h): much less in town, a little more at top
// speed. The difference is added at the game's point (Chassis::State: matrix +0, linear_vel +0x50,
// ground_effect +0x6C, cog +0x70, mass +0xAC, speed +0xBC; IRigidBody::ResolveForce(force, point),
// vtable +0x88). Below 1 less, below 0 lift. It loads or unloads the tyres like the game's own, so
// the grip, the suspension and the steering weight all follow.
//
// Load sensitivity: UpdateLoaded's grip limit (the budget the tyre's force is cut to) is its load
// times its grip, exactly in proportion (built at 0x69DFAE..0x69DFD2: +0x12C * traction boost *
// STATIC_GRIP * load (+0xD4) * pilot factor). For the player's tyres it is then multiplied by
// (load / static load)^-[Physics] LoadSensitivity, the static load being the tyre's share of the
// car's weight (mass, gravity, FRONT_WEIGHT_BIAS at chassis layout +0x6C): at rest nothing changes,
// a tyre loaded more gets less than proportionally more grip. So load moved across an axle in a
// corner costs that axle grip (anti-roll bars and springs set the balance), a heavier axle has less
// grip for its weight (rear-engined cars are looser at the rear), and downforce glues less.
// Tyre curve: the game has two models for a tyre's force. Gripping (UpdateLoaded's traction at
// +0x110 was 1 after the last step and the wheel isn't locked): the side force is
// ComputeLateralForce(load, slip angle) (0x68E090: tables per 2 degrees of slip angle and by load,
// 0x8F8A70, rising to about 90% by 4 degrees and flat from 12 on), the drive force the torque over
// the radius, both cut to the grip limit together. Sliding (the last step was over the limit, the
// wheel spun past a slip speed or locked): the forces are DYNAMIC_GRIP * load along the tyre's skid,
// a different and flat number, and the game switches between the two at the limit, so a tyre's grip
// there came and went (a slide grip setting acting only in the sliding model was felt as random).
// For the player's tyres the side force (at the LATERAL_GRIP multiply, 0x69E098, after the cut) is
// now one curve in both: the gripping model's force, cut to the grip limit, times
// 1 - (1 - [Physics] TyreSlideGrip) * Smoothstep(peak, 3 * peak, the slip angle), peak being
// [Physics] TyrePeakAngle (degrees): a real tyre's side force peaks at a few degrees and falls to
// its sliding grip well past it. In the sliding model the gripping model's force is worked out the
// same way (ComputeLateralForce called for the tyre). A wheel that spins or locks (its slip speed
// from 0.5 to 2 m/s blends it in) slides instead: its force is the grip limit times TyreSlideGrip
// along its skid, as in the game's sliding model (DYNAMIC_GRIP 1.3 against STATIC_GRIP 1.5 on the
// Camaro), so its side force is the sideways share of that and a locked or spinning wheel can't
// hold the car sideways. (The gripping curve times that share took it twice: a spinning rear kept
// a tenth of the game's side grip.) Which model a step uses is
// read where UpdateLoaded tests the traction (0x69DD6E). The fall-off is not on loose surfaces
// (grass, dirt...), where the force keeps rising as the tyre ploughs, and fades in from 2 to 5 m/s,
// where the slip angle means something. The sliding model's force along the wheel stays the game's.
// Wheelspin in slides: UpdateLoaded ties the wheel's spin to the road twice. Its free angular
// acceleration (the torques less the tyre's force, over the wheel's inertia) also takes away
// traction * slip / (dT * radius) (0x69E1B4), which cancels that share of the wheel's slip in one
// step, and the result is then blended toward the road's acceleration (0x69E1E8: a + (road - a) *
// traction), traction being the share of its force the tyre kept (1 within the grip limit: the wheel
// rolls with the road). Just past the limit that is still about 0.9, so a tyre sliding sideways
// under power hardly spins up and the revs don't rise in a drift. For the player's tyres while the
// engine drives them both take traction^(1 + 15 * [Physics] WheelspinInSlides) instead: past the
// limit the surplus torque spins the wheel up until the tyre's sliding force along it takes the
// torque, as a real tyre's does, and the revs climb (0 = the game's).
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
#include "cars.h"
#include <math.h>
#include <vector>
#include <algorithm>

namespace {

// --- speed.exe v1.3 -------------------------------------------------------------------------
constexpr uintptr_t kSuspSimulate = 0x6B1DD0;       // void SuspensionRacer::OnTaskSimulate(float)
constexpr uintptr_t kRBBeginFrame = 0x6A7050;       // void RigidBody::OnBeginFrame(float)
constexpr uintptr_t kGlobalGripScale = 0x8AAC94;    // float Tweak_GlobalGripScale (1.2)
// DoDriveForces: push 20.0 (0x41A00000), the slip limit of its traction control's Ramp; the
// float is the instruction's operand, at +1.
constexpr uintptr_t kTcSlipPush = 0x68F3F7;
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
constexpr int kTire_Radius = 0xC4, kTire_AngVel = 0xD0;
// Tire: the ground's speed under the wheel along it at the last step (m/s), stored by
// SuspensionRacer::Tire::UpdateLoaded from its argument whatever the wheel does (0x69DB1C). (+0xF4
// is that in the traffic cars' tyres, 0x69F065, but in SuspensionRacer's it is a smoothed net torque
// on the wheel, 0x69E194: ABS and TC read it until v0.26.0, so they never acted under hard braking.)
constexpr int kTire_RoadSpeed = 0xFC;
// Tire: the wheel's slip along it this step (m/s), wheel speed (angular velocity * radius) less the
// ground's, stored by UpdateLoaded (0x69DCB1) before the sliding friction.
constexpr int kTire_Slip = 0xF0;
// Tire: the share of its force it kept at the last step (1, or the grip limit / the force when that
// was over it, set at 0x69DFB4 / 0x69E004). Below 1, UpdateLoaded uses the sliding model this step
// (TireModelStub keeps it, as the field is set again before the side force is final).
constexpr int kTire_GripUsed = 0x110;
// Tire: its tyre layout (Attrib::Instance, layout at +8: DYNAMIC_GRIP at +0x20, STATIC_GRIP at
// +0x38, per axle) and its axle (0 front, 1 rear), as UpdateLoaded reads them (0x69DAA9).
constexpr int kTire_Specs = 0x100, kTire_Axle = 0x10C;
constexpr uintptr_t kCallDoDrifting = 0x6AA006;     // TuneWheelParams: call SuspensionRacer::DoDrifting(state)
constexpr uintptr_t kDoDrifting = 0x6A9760;         // thiscall, 1 arg
constexpr uintptr_t kPlayerRain = 0x9196B8;         // Rain* of the player view (eViews[1] + 0x68)
// Rain: its intensity 0..1, as Rain::SetRainIntensity stores it (0x73CC9D) and the rain sound reads
// it (0x4DD08B). (+0x4C until v0.26.0, not a rain field: in the dry it read 0.28 and RainGrip took
// grip off for nothing.)
constexpr int kRain_Intensity = 0x28C;
constexpr uintptr_t kTireModel = 0x69DD6E;          // UpdateLoaded: fld [esi+0x110], the test for its gripping model
constexpr uintptr_t kWheelBlend = 0x69E1E8;         // UpdateLoaded: fmul [esi+0x110], the wheel's tie to the road
constexpr uintptr_t kWheelSlipCut = 0x69E1B4;       // UpdateLoaded: fld [esi+0x110], the share of the slip cancelled
constexpr uintptr_t kComputeLateralForce = 0x68E090; // float Tire::ComputeLateralForce(load, |slip angle|), thiscall
constexpr int kTire_LatBoost = 0xE8, kTire_DriftGrip = 0x11C;   // side force scales UpdateLoaded applies after it
constexpr int kTire_Locked = 0x134;                 // ... brake lock flag (byte): the sliding model
constexpr int kTire_SlipAngle = 0x118;
constexpr int kTire_Load = 0xD4;                    // ... load (N) for this step (0 off the ground)
constexpr int kTire_SideForce = 0xD8;               // ... side force this step (N), cut to the grip limit
// ... the drivetrain's torque on the wheel this step (N m): UpdateLoaded scales it by 1 - handbrake
// (0x69DBD5, the handbrake declutches); the brake torque is at +0xE4.
constexpr int kTire_DriveTorque = 0xE0;
constexpr int kTire_BrakeTorque = 0xE4;
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
constexpr float kAeroRefSpeed = 300.0f / 3.6f;      // the log gives each car's downforce at this speed

// Popped tire at Handicap 1: traction lost, and brake applied as rolling resistance.
constexpr float kPoppedTraction = 0.45f;
constexpr float kPoppedDrag = 0.06f;

typedef void(__thiscall* Simulate_t)(void* obj, float dT);
Simulate_t s_origSusp, s_origRB;
float* s_tcSlip = nullptr;      // DoDriveForces' traction control slip limit (kTcSlipPush + 1), once writable
bool s_playerSusp;      // the player's suspension is simulating
// The player's tyres while its suspension simulates (else null), and the traction each had where
// UpdateLoaded picked its model this step (below 1: the sliding model).
extern "C" uint8_t* s_latTires[4];
extern "C" float s_traction[4];
uint8_t* s_latTires[4];
float s_traction[4] = {1.0f, 1.0f, 1.0f, 1.0f};
// Each player tyre's tie to the road for this step's wheel spin (WheelBlendStub), set by LatGrip.
extern "C" float s_wheelTie[4];
float s_wheelTie[4] = {1.0f, 1.0f, 1.0f, 1.0f};
// Each player tyre's grip budget at its last UpdateLoaded (the grip limit, after load sensitivity).
extern "C" float s_budget[4];
float s_budget[4];
float s_gas = 0.0f;         // the player's throttle at the last DoDrifting
// The player's car for the telemetry panel (Physics_Sample), and its tyres and their grip limits
// while its suspension steps.
CarSample s_sample;
uint8_t* s_telTires[4];
float s_telGrip[4];
float s_refLoad[2];         // a front / rear tyre's static load (N), for load sensitivity
float s_lsRatio[4], s_lsFactor[4];  // the last step's load sensitivity, for the log
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
// The tyre curve this second, per axle (front, rear), over the player's tyre steps above 5 m/s:
// how many used the sliding model, the slip angle (mean, highest), the curve's factor (mean,
// lowest), the gripping model's force over the grip limit, and in the sliding model the side force
// a spinning or locked wheel got against the curve's, for [Physics] LogPhysics.
struct CurveStats {
    int steps = 0, sliding = 0;
    float angle = 0, angleMax = 0, factor = 0, min = 1, over = 0, share = 0;
    // Past the limit under power: steps, the game's tie of the wheel to the road, ours, wheel slip (m/s).
    int freed = 0;
    float tie = 0, ourTie = 0, slip = 0;
} s_curve[2];


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
    uint8_t* bias[2] = {};      // the chassis layouts (SuspensionRacer's and Chassis') whose weight bias was set
    float biasVals[2];
};

// The current car's [Physics] Character and RealWeight factors (cars.h), brought up to date at
// the start of each of its steps by Character().
CarCharacter s_char;
void Character();

const int kSuspOffsets[4] = {kChL_ShockExt, kChL_Shock, kChL_Spring, kChL_Swaybar};

void Apply(uint8_t* susp, Saved& sv) {
    const Config& c = g_cfg;
    sv.grip = *(float*)kGlobalGripScale;
    if (s_tcSlip) sv.slip = *s_tcSlip;
    sv.yaw = *(float*)kMaxYawBonus;
    Character();
    const CarCharacter& ch = s_char;
    *(float*)kGlobalGripScale = sv.grip * c.grip * ch.grip;
    // DoDriveForces scales the cut by Ramp(slip, 1, limit) = (slip - 1) / (limit - 1).
    float tcStrength = c.tc ? 1.0f : c.tractionControl;   // TC on: the game's own at full strength
    if (s_tcSlip) *s_tcSlip = tcStrength > 0.01f ? 1.0f + (sv.slip - 1.0f) / tcStrength : 1.0e9f;
    *(float*)kMaxYawBonus = sv.yaw * c.stability;
    float* graph = *(float**)kDriftStabTable;
    for (int i = 0; i < g.stabN; i++) graph[i * 2 + 1] = g.stab[i] * c.stability;
    float* rear = *(float**)(kDriftRearTable + 0x10);
    for (int i = 0; i < g.rearN; i++) rear[i] = 1.0f + (g.rear[i] - 1.0f) * c.driftAssist;
    uint8_t* layout = Field<uint8_t*>(susp, kSR_SuspLayout);
    float rb = Clamp(c.rollBalance + ch.rollBalance, -0.5f, 0.5f);
    if ((c.suspFirmness != 1.0f || rb != 0.0f || ch.spring != 1.0f || ch.swaybar != 1.0f) && Plausible((uintptr_t)layout)) {
        sv.susp = layout;
        float damp = sqrtf(c.suspFirmness) * ch.damper;
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 2; a++) {
                float& v = Field<float>(layout, kSuspOffsets[k] + a * 4);
                sv.suspVals[k * 2 + a] = v;
                v *= k < 2 ? damp : c.suspFirmness * (k == 2 ? ch.spring : ch.swaybar);
                // Roll balance: the anti-roll bars' stiffness moved to the front (+) or the rear (-).
                if (kSuspOffsets[k] == kChL_Swaybar) v *= a == 0 ? 1.0f + rb : 1.0f - rb;
            }
    }
    // Real weight: Chassis::SetCOG places the centre of gravity by the Chassis' layout, the load
    // sensitivity's static loads come from it too (Hook_DoAerodynamics).
    if (ch.weightFront > 0.0f) {
        uint8_t* layouts[2] = {layout, Field<uint8_t*>(susp, kCh_Layout)};
        for (int i = 0; i < 2; i++)
            if (Plausible((uintptr_t)layouts[i]) && (i == 0 || layouts[1] != layouts[0])) {
                float& v = Field<float>(layouts[i], kChL_WeightBias);
                sv.bias[i] = layouts[i], sv.biasVals[i] = v, v = ch.weightFront;
            }
    }
}

void Restore(const Saved& sv) {
    *(float*)kGlobalGripScale = sv.grip;
    if (s_tcSlip) *s_tcSlip = sv.slip;
    *(float*)kMaxYawBonus = sv.yaw;
    float* graph = *(float**)kDriftStabTable;
    for (int i = 0; i < g.stabN; i++) graph[i * 2 + 1] = g.stab[i];
    float* rear = *(float**)(kDriftRearTable + 0x10);
    memcpy(rear, g.rear, g.rearN * sizeof(float));
    if (sv.susp)
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 2; a++) Field<float>(sv.susp, kSuspOffsets[k] + a * 4) = sv.suspVals[k * 2 + a];
    for (int i = 0; i < 2; i++)
        if (sv.bias[i]) Field<float>(sv.bias[i], kChL_WeightBias) = sv.biasVals[i];
}

bool Stock() {
    const Config& c = g_cfg;
    return (c.tractionControl >= 1.0f || c.tc) && c.stability >= 1.0f && c.driftAssist >= 1.0f && c.grip == 1.0f &&
           c.tyreSlideGrip >= 1.0f && c.frictionCircle == 0.0f && c.loadSensitivity == 0.0f && c.slideWheelspin == 0.0f &&
           c.suspFirmness == 1.0f && c.rollBalance == 0.0f && c.character == 0.0f && !c.realWeight;
}

float s_suspDT = 0.0f;                  // the player's last suspension step (ABS and TC time their pulses by it)
float s_playerSpeed = 0.0f;             // m/s, from the player's rigid body at its last step

typedef float(__thiscall* WheelFloat_t)(void* susp, unsigned wheel);
typedef bool(__thiscall* WheelBool_t)(void* susp, unsigned wheel);
constexpr int kISusp_OnGround = 0x24, kISusp_Compression = 0x28;   // ISuspension vtable slots

// Before the player's suspension step: its tyres, whose grip limits GripLimit keeps for the
// telemetry panel.
void StartSample(uint8_t* iface) {
    for (int i = 0; i < 4; i++) {
        uint8_t* tire = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        s_telTires[i] = Plausible((uintptr_t)tire) ? tire : nullptr;
        s_telGrip[i] = 0.0f;
    }
    s_sample.body = false;
}

// After the player's suspension step: what it did to each wheel. The body (axes, velocity, mass,
// downforce) was taken during the step by Hook_DoAerodynamics.
void EndSample(uint8_t* iface, float dT) {
    CarSample& c = s_sample;
    c.wheels = true;
    for (int i = 0; i < 4; i++) {
        uint8_t* tire = s_telTires[i];
        s_telTires[i] = nullptr;
        if (!tire) {
            c.wheels = false;
            continue;
        }
        CarSample::Wheel& w = c.wheel[i];
        w.onGround = ((WheelBool_t)(*(uintptr_t**)iface)[kISusp_OnGround / 4])(iface, i);
        w.compression = ((WheelFloat_t)(*(uintptr_t**)iface)[kISusp_Compression / 4])(iface, i);
        w.load = Field<float>(tire, kTire_Load);
        w.side = Field<float>(tire, kTire_SideForce);
        // The force along the tyre as the gripping model has it: (drive + brake torque) / radius.
        float radius = Field<float>(tire, kTire_Radius);
        w.along = radius > 0.05f ? (Field<float>(tire, kTire_DriveTorque) + Field<float>(tire, kTire_BrakeTorque)) / radius : 0.0f;
        w.grip = s_telGrip[i];
        w.kept = Field<float>(tire, kTire_GripUsed);
    }
    c.dt = dT;
    c.step++;
}

void __fastcall Hook_SuspSimulate(uint8_t* susp, void*, float dT) {
    uint8_t* iface = (uint8_t*)Game_PlayerInterface((void*)kISuspensionHandle);
    bool player = Owns(susp, iface);
    s_playerSusp = player;
    if (player) s_suspDT = dT, s_diag.suspSteps++, StartSample(iface);
    if (!player || !g.ok || !g_cfg.physics || Stock()) {
        s_origSusp(susp, dT);
        if (player) EndSample(iface, dT);
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
        // The game's sliding and static grip, front and rear: its sliding model's force against the grip
        // limit the gripping model is cut to.
        float grip[2][2] = {};
        for (int a = 0; a < 2; a++) {
            uint8_t* tire = *(uint8_t**)(iface + kSuspIface_Tires + a * 8);
            uint8_t* specs = Plausible((uintptr_t)tire) ? Field<uint8_t*>(tire, kTire_Specs) : nullptr;
            uint8_t* tl = specs && Plausible((uintptr_t)specs) ? Field<uint8_t*>(specs, 8) : nullptr;
            int axle = tire ? Field<int>(tire, kTire_Axle) : -1;
            if (tl && Plausible((uintptr_t)tl) && (axle == 0 || axle == 1))
                grip[a][0] = Field<float>(tl, 0x20 + axle * 4), grip[a][1] = Field<float>(tl, 0x38 + axle * 4);
        }
        Log("physics: tyres front / rear: DYNAMIC_GRIP %.2f / %.2f, STATIC_GRIP %.2f / %.2f; tyre curve peak %.1f deg, "
            "sliding grip %.2f", grip[0][0], grip[1][0], grip[0][1], grip[1][1], g_cfg.tyrePeakAngle, g_cfg.tyreSlideGrip);
    }
    s_diag.suspCustom++;
    Saved sv;
    Apply(susp, sv);
    // The player's tyres, for the tyre stubs.
    for (int i = 0; i < 4; i++) {
        s_latTires[i] = *(uint8_t**)(iface + kSuspIface_Tires + i * 4);
        s_budget[i] = 0.0f;
        s_traction[i] = 1.0f;
    }
    s_origSusp(susp, dT);
    for (uint8_t*& t : s_latTires) t = nullptr;
    Restore(sv);
    EndSample(iface, dT);
    s_playerSusp = false;
}

// In place of UpdateLoaded's fld [esi+0x110] where it picks its model (6 bytes): the same load, and
// for a tyre in s_latTires its traction kept in s_traction. The x87 stack is empty before it; eax and
// edx are kept; the flags are free (the next fcomp and test set them).
__asm__(
    ".text\n"
    "_TireModelStub:\n"
    "    flds 0x110(%esi)\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    ret\n"
    "2:  pushl %edx\n"
    "    movl 0x110(%esi), %edx\n"
    "    movl %edx, _s_traction(,%eax,4)\n"
    "    popl %edx\n"
    "    popl %eax\n"
    "    ret\n");
extern "C" void TireModelStub();

// In place of UpdateLoaded's fmul [esi+0x110] in the wheel's spin (6 bytes): for a tyre in
// s_latTires times s_wheelTie instead. The x87 stack holds two values here (eight are free); eax is
// kept; the flags are free.
__asm__(
    ".text\n"
    "_WheelBlendStub:\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    fmuls 0x110(%esi)\n"
    "    ret\n"
    "2:  fmuls _s_wheelTie(,%eax,4)\n"
    "    popl %eax\n"
    "    ret\n");
extern "C" void WheelBlendStub();

// In place of UpdateLoaded's fld [esi+0x110] in the wheel's free acceleration (6 bytes): for a tyre
// in s_latTires s_wheelTie instead. The x87 stack holds two values here; eax is kept; the flags are
// free.
__asm__(
    ".text\n"
    "_WheelSlipCutStub:\n"
    "    pushl %eax\n"
    "    xorl %eax, %eax\n"
    "1:  cmpl _s_latTires(,%eax,4), %esi\n"
    "    je 2f\n"
    "    incl %eax\n"
    "    cmpl $4, %eax\n"
    "    jb 1b\n"
    "    popl %eax\n"
    "    flds 0x110(%esi)\n"
    "    ret\n"
    "2:  flds _s_wheelTie(,%eax,4)\n"
    "    popl %eax\n"
    "    ret\n");
extern "C" void WheelSlipCutStub();

// In place of UpdateLoaded's fld [ecx+0xF0] (6 bytes): the same load, times LatGrip (the tyre curve,
// written to the side force, and the friction circle) for a tyre in s_latTires. The x87 stack is
// empty here; eax, ecx and edx are kept; the flags are free (the next test sets them again).
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
    for (int i = 0; i < 4 && k > 0.0f; i++) {
        if (s_latTires[i] != tire) continue;
        float ref = s_refLoad[i >= 2], load = Field<float>(tire, kTire_Load);
        if (ref > 1.0f && load > 0.0f && *limit > 0.0f) {
            float r = Clamp(load / ref, 0.05f, 20.0f), f = powf(r, -k);
            *limit *= f;
            s_lsRatio[i] = r, s_lsFactor[i] = f;
        }
        break;
    }
    // The player's grip limits, for the telemetry panel (whatever the physics settings).
    for (int i = 0; i < 4; i++)
        if (s_telTires[i] == tire) s_telGrip[i] = *limit;
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

typedef float(__thiscall* ComputeLateralForce_t)(void* tire, float load, float angle);

// The tyre curve for a player's tyre at slip angle `a` (degrees): 1 up to TyrePeakAngle, falling to
// TyreSlideGrip by 3 times it, faded in from 2 to 5 m/s (where the angle means something); 1 on
// loose surfaces.
float Curve(int i, uint8_t* tire, float a) {
    float peak = g_cfg.tyrePeakAngle;
    float f = 1.0f - (1.0f - g_cfg.tyreSlideGrip) * Smoothstep(peak, 3.0f * peak, a) * Smoothstep(2.0f, 5.0f, s_playerSpeed);
    if (f < 1.0f && LooseSurface(i, tire)) f = 1.0f;
    return f;
}

// A player's tyre's side force this step on the tyre curve, written over UpdateLoaded's (before the
// surface's LATERAL_GRIP). In the gripping model it is the game's (cut to the grip limit together
// with the drive or brake force) times the curve. In the sliding model it is the gripping model's
// force for the slip angle and load (with the scales UpdateLoaded applies after it), cut to the grip
// limit, times the curve; a wheel that spins or locks slides on the grip limit times TyreSlideGrip
// along its skid instead, so its side force is that times the sideways share of the skid (the
// lateral speed from the slip angle and the ground speed along the wheel, as in Circle).
void TyreCurve(int i, uint8_t* tire) {
    float& side = Field<float>(tire, kTire_SideForce);
    float turns = Field<float>(tire, kTire_SlipAngle);
    float a = fabsf(turns) * 360.0f;
    if (!(a <= 180.0f)) return;              // NaN or nonsense
    if (a > 90.0f) a = 180.0f - a;           // rolling backwards
    float f = Curve(i, tire, a);
    bool sliding = !(s_traction[i] >= 1.0f) || Field<uint8_t>(tire, kTire_Locked);
    bool log = g_cfg.logPhysics && s_playerSpeed > 5.0f;
    float budget = s_budget[i], grip = 0.0f, kept = 1.0f;
    if (sliding || log)
        grip = fabsf(((ComputeLateralForce_t)kComputeLateralForce)(tire, Field<float>(tire, kTire_Load), fabsf(turns)) *
                     Field<float>(tire, kTire_LatBoost) * Field<float>(tire, kTire_DriftGrip));
    if (!(grip < 1.0e7f)) return;            // NaN or nonsense
    if (sliding) {
        float curve = (budget > 0.0f ? std::min(grip, budget) : grip) * f, want = curve;
        float slip = Field<float>(tire, kTire_Slip);
        float spin = Smoothstep(0.5f, 2.0f, fabsf(slip));
        if (spin > 0.0f && budget > 0.0f) {
            float lat = tanf(std::min(a * 0.017453293f, 1.55f)) * fabsf(Field<float>(tire, kTire_RoadSpeed));
            float skid = sqrtf(lat * lat + slip * slip);
            float slide = budget * g_cfg.tyreSlideGrip * (skid > 0.001f ? lat / skid : 0.0f);
            want = curve + (slide - curve) * spin;
            if (curve > 1.0f) kept = want / curve;
        }
        side = turns > 0.0f ? -want : want;
    } else {
        side *= f;
    }
    if (log) {
        CurveStats& s = s_curve[i >= 2];
        s.steps++, s.angle += a, s.factor += f;
        s.angleMax = std::max(s.angleMax, a), s.min = std::min(s.min, f);
        if (budget > 0.0f) s.over += std::min(grip / budget, 9.0f);
        if (sliding) s.sliding++, s.share += std::min(kept, 9.0f);
    }
}

// The friction circle for a player's tyre: the factor that holds its side force (after the game's
// cut and the tyre curve) within budget * room. u is the drive torque's share of the grip (torque / radius /
// budget; none while it holds the car back, engine braking), and room = sqrt(1 - u^2). A wheel
// that spins (its slip from 0.5 to 2 m/s) has its force along its skid: its side share is the
// lateral speed over the skid speed (the lateral speed from the slip angle and the ground speed
// along the wheel, angular velocity * radius - the slip), and room is at least that, so a car
// sliding further sideways gets side grip back and a power slide can be held. FrictionCircle up to
// 1 blends room from 1 (0) to all of it (1); above 1 the drive torque counts that many times over
// (an exaggeration, for testing).
float Circle(int i, uint8_t* tire) {
    float budget = s_budget[i], radius = Field<float>(tire, kTire_Radius);
    if (!(budget > 0.0f) || !(radius > 0.0f)) return 1.0f;
    float slip = Field<float>(tire, kTire_Slip);
    float wheel = Field<float>(tire, kTire_AngVel) * radius, fwd = wheel - slip;
    float torque = Field<float>(tire, kTire_DriveTorque);
    // Only while the engine drives the wheel: the throttle on, and the torque turning the wheel the
    // way it already turns (engine braking holds it back; a car sliding sideways or backwards can
    // have the ground going the other way under a wheel the engine brakes).
    if (s_gas < 0.05f || (torque * wheel < 0.0f && fabsf(wheel) > 0.5f)) return 1.0f;
    float k = g_cfg.frictionCircle * s_char.throttle;
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
    float side = fabsf(Field<float>(tire, kTire_SideForce));
    float f = side > cap ? cap / side : 1.0f;
    CircleStats& c = s_circle[i >= 2];
    c.steps++, c.use += std::min(u, 9.0f), c.keep += f;
    c.useMax = std::max(c.useMax, u);
    if (f < 1.0f) c.held++;
    return f;
}

// For a player's tyre this step: its side force put on the tyre curve, and the friction circle's
// factor returned.
// The tie of a player's tyre's wheel to the road for this step's spin: the game's (its traction,
// final by now), or while the engine drives it past the limit traction^(1 + 15 * WheelspinInSlides).
void WheelTie(int i, uint8_t* tire) {
    float t = Field<float>(tire, kTire_GripUsed);
    s_wheelTie[i] = t;
    if (!(t < 1.0f && t > 0.0f) || g_cfg.slideWheelspin <= 0.0f || s_gas < 0.05f) return;
    float wheel = Field<float>(tire, kTire_AngVel) * Field<float>(tire, kTire_Radius);
    float torque = Field<float>(tire, kTire_DriveTorque);
    if (!(torque * wheel > 0.0f)) return;    // not driven, or engine braking
    float tie = powf(t, 1.0f + 15.0f * g_cfg.slideWheelspin);
    s_wheelTie[i] = tie;
    if (g_cfg.logPhysics && s_playerSpeed > 5.0f) {
        CurveStats& s = s_curve[i >= 2];
        s.freed++, s.tie += t, s.ourTie += tie, s.slip += fabsf(Field<float>(tire, kTire_Slip));
    }
}

extern "C" __attribute__((used)) float LatGrip(int i) {
    uint8_t* tire = s_latTires[i];
    if (!tire) return 1.0f;
    WheelTie(i, tire);
    if (g_cfg.tyreSlideGrip < 1.0f) TyreCurve(i, tire);
    return g_cfg.frictionCircle > 0.0f ? Circle(i, tire) : 1.0f;
}

bool Plausible(uintptr_t p);

typedef void(__thiscall* DoAerodynamics_t)(void* chassis, const uint8_t* state, float drag, float aero, float frontZ,
                                           float rearZ, const uint8_t* tunings);
typedef void*(__thiscall* GetRigidBody_t)(void* simable);
typedef void(__thiscall* ResolveForce_t)(void* rb, const float* force, const float* point);

// The game's downforce (N), as DoAerodynamics has it: less when tilted with fewer than two wheels
// down, none going backwards, 0.8 in the air, the Aerodynamics tuning a quarter per step.
float GameDownforce(const uint8_t* state, uint8_t* layout, float aero, const uint8_t* tunings) {
    const float* m = (const float*)state;
    float mass = *(const float*)(state + kSt_Mass), speed = *(const float*)(state + kSt_Speed);
    if (aero <= 0.0f || mass <= 1.0f || speed < 1.0f) return 0.0f;
    float tuning = tunings ? *(const float*)(tunings + kTunings_Aero) : 0.0f;
    float ground = *(const float*)(state + kSt_Ground);
    float upness = ground >= 0.5f ? 1.0f : std::max(m[5], 0.0f);
    const float* vel = (const float*)(state + kSt_LinearVel);
    float forwardness = std::max((vel[0] * m[8] + vel[1] * m[9] + vel[2] * m[10]) / speed, 0.0f);
    forwardness = std::max(*(const float*)kAeroDropOffMin, powf(forwardness, *(const float*)kAeroDropOff));
    float game = aero * upness * forwardness * speed * Field<float>(layout, kChL_AeroCoef) * 2000.0f;
    if (ground == 0.0f) game *= 0.8f;
    return game + game * 0.25f * tuning;
}

// The player's body for the telemetry panel, from its Chassis::State in the suspension step: its
// axes, velocity and mass, the downforce on it (the game's, times [Physics] Downforce as
// Hook_DoAerodynamics adds it while the physics settings are on), and where that and the weight sit.
void SampleBody(const uint8_t* state, uint8_t* layout, float game) {
    CarSample& c = s_sample;
    const float* m = (const float*)state;
    for (int k = 0; k < 3; k++) c.right[k] = m[k], c.up[k] = m[4 + k], c.fwd[k] = m[8 + k];
    memcpy(c.vel, state + kSt_LinearVel, sizeof(c.vel));
    c.mass = *(const float*)(state + kSt_Mass);
    float speed = *(const float*)(state + kSt_Speed);
    float shape = g_cfg.downforceSquared ? speed / (g_cfg.downforceMatch / 3.6f) : 1.0f;
    c.downforce = g_cfg.physics ? game * g_cfg.downforce * shape : game;
    c.aeroFront = Clamp(Field<float>(layout, kChL_AeroCg) * 0.01f, 0.0f, 1.0f);
    float bias = Field<float>(layout, kChL_WeightBias) * 0.01f;
    c.weightFront = bias > 0.2f && bias < 0.8f ? bias : 0.5f;
    c.body = c.mass > 1.0f;
}

// The game's own aerodynamics, then for the player's car the difference between the game's own
// downforce (as DoAerodynamics computes it) and [Physics] Downforce times it, times
// speed / DownforceMatchSpeed with DownforceSquared, at the same point.
void __fastcall Hook_DoAerodynamics(uint8_t* chassis, void*, const uint8_t* state, float drag, float aero, float frontZ,
                                    float rearZ, const uint8_t* tunings) {
    ((DoAerodynamics_t)kDoAerodynamics)(chassis, state, drag, aero, frontZ, rearZ, tunings);
    if (!s_playerSusp || !state) return;
    const float* m = (const float*)state;   // rows: right, up, forward, position (4 floats each)
    uint8_t* layout = Field<uint8_t*>(chassis, kCh_Layout);
    if (!Plausible((uintptr_t)layout)) return;
    SampleBody(state, layout, GameDownforce(state, layout, aero, tunings));
    if (!g_cfg.physics) return;
    s_diag.aeroSteps++;
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
    float ground = *(const float*)(state + kSt_Ground);
    float game = GameDownforce(state, layout, aero, tunings);
    if (game <= 0.0f) return;
    s_diag.gameForce = game;
    float downforce = g_cfg.downforce;
    if (downforce == 1.0f && !g_cfg.downforceSquared) return;
    float shape = g_cfg.downforceSquared ? speed / (g_cfg.downforceMatch / 3.6f) : 1.0f;
    float f = (downforce * shape - 1.0f) * game;   // + more down, - less (below 0 Downforce: lift)
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
    if ((g_cfg.logInputs || g_cfg.logPhysics) && (absNow || s_aid.tcActive) && Now() - lastLog > 0.5) {
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
    float balance = Clamp(g_cfg.gripBalance + s_char.gripBalance, -0.5f, 0.5f);
    float front = 1.0f + balance, rear = 1.0f - balance, off = g_cfg.offroadGrip;
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
        Log("physics: %3.0f km/h: suspension %d steps (%d with the mod's values), aero %d; downforce %.0f%% of the "
            "weight (the game's %.0f%%)",
            s_playerSpeed * 3.6f, s_diag.suspSteps, s_diag.suspCustom, s_diag.aeroSteps,
            s_diag.weight > 0 ? (s_diag.gameForce + s_diag.extraForce) / s_diag.weight * 100.0f : 0.0f,
            s_diag.weight > 0 ? s_diag.gameForce / s_diag.weight * 100.0f : 0.0f);
    for (int a = 0; a < 2; a++) {
        const CircleStats& c = s_circle[a];
        if (!g_cfg.logPhysics || c.steps == 0) continue;
        float n = (float)c.steps;
        Log("physics:   %s driven (%d tyre steps, throttle %.2f): drive torque %.0f%% of the grip (up to %.0f%%); "
            "side force held to the friction circle %.0f%% of the steps, kept x%.2f",
            a ? "rear" : "front", c.steps, s_gas, c.use / n * 100.0f, c.useMax * 100.0f, c.held / n * 100.0f, c.keep / n);
    }
    if (g_cfg.logPhysics && s_playerSpeed > 5.0f && g_cfg.loadSensitivity > 0.0f)
        Log("physics:   tyres FL FR RL RR: load %.2f %.2f %.2f %.2f of static, grip limit x%.2f %.2f %.2f %.2f",
            s_lsRatio[0], s_lsRatio[1], s_lsRatio[2], s_lsRatio[3], s_lsFactor[0], s_lsFactor[1], s_lsFactor[2],
            s_lsFactor[3]);
    for (int a = 0; a < 2; a++) {
        const CurveStats& s = s_curve[a];
        if (!g_cfg.logPhysics || s.steps == 0) continue;
        float n = (float)s.steps;
        Log("physics:   %s tyre curve (%d tyre steps, %.0f%% sliding model): slip angle %.1f deg (up to %.0f), curve "
            "x%.2f (down to x%.2f), the game's side force %.2f of the grip limit; spinning or locked wheels x%.2f of it",
            a ? "rear" : "front", s.steps, s.sliding * 100.0f / n, s.angle / n, s.angleMax, s.factor / n, s.min,
            s.over / n, s.sliding ? s.share / s.sliding : 1.0f);
        if (s.freed)
            Log("physics:   %s past the limit under power %d tyre steps: wheel tied to the road x%.2f (the game's x%.2f), "
                "wheel slip %.1f m/s", a ? "rear" : "front", s.freed, s.ourTie / s.freed, s.tie / s.freed, s.slip / s.freed);
    }
    for (CurveStats& s : s_curve) s = CurveStats();
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
    {&Config::tyreSlideGrip, "TyreSlideGrip", 0.5f, 1.0f},
    {&Config::rainGrip, "RainGrip", 0.5f, 1.0f},
    {&Config::maxSpin, "MaxSpin", 0.0f, 30.0f},
    {&Config::downforce, "Downforce", -0.25f, 2.0f},
    {&Config::suspFirmness, "SuspensionFirmness", 0.5f, 2.0f},
    {&Config::rollBalance, "RollBalance", -0.5f, 0.5f},
};
constexpr int kNumPerCar = sizeof(kPerCar) / sizeof(kPerCar[0]);
// Keys of settings that are gone or were replaced (FrontGrip and RearGrip by GripBalance, SlideGrip
// by TyreSlideGrip; DownforceAt300 was in test builds only): taken out of a section when it is written.
const char* const kOldPerCarKeys[] = {"FrontGrip", "RearGrip", "SmokeBack", "Aero", "PowerOversteer", "SlideOversteer",
                                      "SlideGrip", "SpeedGripLoss", "DownforceAt300"};

int PerCarIndex(float Config::*field) {
    for (int i = 0; i < kNumPerCar; i++)
        if (kPerCar[i].field == field) return i;
    return -1;
}
// The driving aids' switches (ABS, TC) are the same for every car: always [Physics], never a car's
// own section (ABS / TC keys left in [Car.<name>] are ignored).

struct {
    std::string car;            // the player's vehicle name, empty = none yet
    bool own = false;           // it has its own [Car.<name>] section
    std::string group;          // else the group it is in ([Group.<name>]), empty = none
    float global[kNumPerCar];   // the [Physics] values while a car has its own or a group's
} pc;

void StoreGlobal() {
    for (int i = 0; i < kNumPerCar; i++) pc.global[i] = g_cfg.*kPerCar[i].field;
}

void LoadGlobal() {
    for (int i = 0; i < kNumPerCar; i++) g_cfg.*kPerCar[i].field = pc.global[i];
}

bool Shared() { return !pc.own && pc.group.empty(); }

// Groups: named sets of the per-car values that several cars share, one [Group.<name>] section
// each in the gameplay ini: the values, and Cars= the cars in it (CARS folder names, comma
// separated). Driving a car in a group gives it the group's values; changing one in the menu
// changes them for every car in it. A car is in one group at most; its own [Car.<name>] section
// goes before any group. Read once, kept here, written through inisave's thread.
struct Group {
    std::string name;
    std::vector<std::string> cars;
    float value[kNumPerCar];        // its values, as the ini has them (kept up to date here)
    bool has[kNumPerCar] = {};      // false: the key is missing, the shared value applies
};
std::vector<Group> s_groups;
std::vector<std::string> s_ownCars;     // the cars with their own [Car.<name>] section (not an old preset link)

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void ReadGroups() {
    static bool read;
    if (read) return;
    read = true;
    IniSave_Flush();
    std::vector<char> names(65536), cars(8192);
    std::vector<IniWrite> stale;   // [Car.<name>] sections that only followed a preset
    DWORD n = GetPrivateProfileSectionNamesA(names.data(), (DWORD)names.size(), IniPath());
    for (const char* sec = names.data(); sec < names.data() + n && *sec; sec += strlen(sec) + 1) {
        if (_strnicmp(sec, "Car.", 4) == 0 && sec[4]) {
            // A section with a Preset= link is from before groups: its values were a copy the preset
            // replaced, never the car's own (the car drives on its group or the shared settings), and
            // the links named the wrong drive for some cars. Removed; a car's own section has no link.
            char preset[8];
            if (GetPrivateProfileStringA(sec, "Preset", "", preset, sizeof(preset), IniPath()) == 0)
                s_ownCars.push_back(Lower(sec + 4));
            else
                stale.push_back({IniPath(), sec, "", ""});
            continue;
        }
        if (_strnicmp(sec, "Group.", 6) != 0 || !sec[6]) continue;
        Group g;
        g.name = sec + 6;
        GetPrivateProfileStringA(sec, "Cars", "", cars.data(), (DWORD)cars.size(), IniPath());
        std::string list = cars.data();
        size_t a = 0;
        while (a <= list.size()) {
            size_t b = list.find(',', a);
            if (b == std::string::npos) b = list.size();
            std::string car = list.substr(a, b - a);
            size_t x = car.find_first_not_of(' '), y = car.find_last_not_of(' ');
            if (x != std::string::npos) g.cars.push_back(Lower(car.substr(x, y - x + 1)));
            a = b + 1;
        }
        char buf[64];
        for (int i = 0; i < kNumPerCar; i++)
            if (GetPrivateProfileStringA(sec, kPerCar[i].key, "", buf, sizeof(buf), IniPath()) > 0)
                g.value[i] = Clamp((float)atof(buf), kPerCar[i].lo, kPerCar[i].hi), g.has[i] = true;
        s_groups.push_back(g);
    }
    // An ini from before groups (an update keeps the player's ini): the premade ones are added,
    // once ([Physics] PremadeGroups marks it, so groups deleted later stay deleted). A car with its
    // own settings in one keeps them: its own section goes first.
    if (s_groups.empty() && GetPrivateProfileIntA("Physics", "PremadeGroups", 0, IniPath()) == 0) {
        static const struct { const char* name; const char* cars; } kPremade[] = {
            {"Muscle", "camaro,corvette,cts,gto,monaro,mustanggt,viper"},
            {"Tuner FWD", "cobaltss,eclipsegt,gti,punto"},
            {"Tuner AWD", "a3,a4,imprezawrx,lancerevo8,tt"},
            {"Tuner RWD", "clio,is300,rx7,rx8,supra"},
            {"Exotic RWD", "911gt2,997s,bmwm3gtr,bmwm3gtre46,carreragt,caymans,clk500,corvettec6r,db9,elise,fordgt,sl500,sl65,slr"},
            {"Exotic AWD", "911turbo,gallardo,murcielago"},
            {"FWD", ""}, {"RWD", ""}, {"AWD", ""},
        };
        std::vector<IniWrite> w;
        for (const auto& pre : kPremade) {
            Group g;
            g.name = pre.name;
            std::string list = pre.cars;
            for (size_t a = 0; a < list.size();) {
                size_t b = list.find(',', a);
                if (b == std::string::npos) b = list.size();
                g.cars.push_back(list.substr(a, b - a));
                a = b + 1;
            }
            s_groups.push_back(g);
            w.push_back({IniPath(), "Group." + g.name, "Cars", list});
        }
        w.push_back({IniPath(), "Physics", "PremadeGroups", "1"});
        IniSave_Queue(w);
        Log("physics: premade groups added (Muscle, Tuner FWD / AWD / RWD, Exotic RWD / AWD, FWD, RWD, AWD)");
    }
    if (!stale.empty()) {
        IniSave_Queue(stale);
        Log("physics: %u car sections from the old presets removed (not the cars' own settings)", (unsigned)stale.size());
    }
    Log("physics: %u groups", (unsigned)s_groups.size());
}

Group* FindGroup(const std::string& name) {
    for (Group& g : s_groups)
        if (_stricmp(g.name.c_str(), name.c_str()) == 0) return &g;
    return nullptr;
}

Group* GroupOf(const std::string& car) {
    for (Group& g : s_groups)
        for (const std::string& c : g.cars)
            if (_stricmp(c.c_str(), car.c_str()) == 0) return &g;
    return nullptr;
}

void WriteCars(const Group& g) {
    std::string list;
    for (const std::string& c : g.cars) list += (list.empty() ? "" : ",") + c;
    IniSave_Queue({{IniPath(), "Group." + g.name, "Cars", list}});
}

// Takes `car` out of whichever group has it.
void LeaveGroups(const std::string& car) {
    for (Group& g : s_groups)
        for (size_t i = 0; i < g.cars.size(); i++)
            if (_stricmp(g.cars[i].c_str(), car.c_str()) == 0) {
                g.cars.erase(g.cars.begin() + i);
                WriteCars(g);
                break;
            }
}

// The per-car values in use into `section`.
void WriteValues(const std::string& section) {
    std::vector<IniWrite> w;
    char buf[32];
    for (const PerCar& p : kPerCar) {
        snprintf(buf, sizeof(buf), "%g", g_cfg.*p.field);
        w.push_back({IniPath(), section, p.key, buf});
    }
    IniSave_Queue(w);
}

// The current car's group keeps the values in use (the menu changes them in g_cfg): copied into
// its entry before the car leaves it.
void SyncGroup() {
    if (pc.own || pc.group.empty()) return;
    if (Group* g = FindGroup(pc.group))
        for (int i = 0; i < kNumPerCar; i++) g->value[i] = g_cfg.*kPerCar[i].field, g->has[i] = true;
}

// A group's values into g_cfg (on top of the shared ones, already there).
void ApplyGroup(const Group& g) {
    for (int i = 0; i < kNumPerCar; i++)
        if (g.has[i]) g_cfg.*kPerCar[i].field = g.value[i];
}

// The per-car values from `section` into g_cfg (keys it lacks keep the values there now).
void ReadValues(const std::string& section) {
    IniSave_Flush();
    char buf[64];
    for (const PerCar& p : kPerCar)
        if (GetPrivateProfileStringA(section.c_str(), p.key, "", buf, sizeof(buf), IniPath()) > 0)
            g_cfg.*p.field = Clamp((float)atof(buf), p.lo, p.hi);
}

void Character() {
    static std::string car = "";
    static float k = -1.0f;
    static bool real;
    if (car == pc.car && k == g_cfg.character && real == g_cfg.realWeight) return;
    car = pc.car, k = g_cfg.character, real = g_cfg.realWeight;
    s_char = Cars_Character(car.c_str(), k, real);
    const CarCharacter& c = s_char;
    if (!c.known) {
        Log("physics: %s isn't in the game's car data: no character or real weight", car.empty() ? "?" : car.c_str());
        return;
    }
    char weight[32] = "the game's";
    if (c.weightFront > 0.0f) snprintf(weight, sizeof(weight), "%.0f%%", c.weightFront);
    Log("physics: character %.0f%% for %s: springs %.2f, dampers %.2f, anti-roll bars %.2f, roll balance %+.2f, grip %.2f, "
        "throttle takes side grip %.2f; weight %s front, grip balance %+.3f", k * 100.0f, car.c_str(),
        c.spring, c.damper, c.swaybar, c.rollBalance, c.grip, c.throttle, weight, c.gripBalance);
}

}  // namespace

std::string Physics_Section() { return pc.own ? "Car." + pc.car : !pc.group.empty() ? "Group." + pc.group : "Physics"; }
const char* Physics_Car() { return pc.car.c_str(); }
const CarCharacter& Physics_Character() { return s_char; }

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

void Physics_SetCar(const char* name) {
    std::string car = name ? name : "";
    if (car == pc.car) return;
    IniSave_Flush();   // the last car's section written before this one's is read
    ReadGroups();
    if (Shared()) StoreGlobal();
    SyncGroup();
    pc.car = car;
    pc.group.clear();
    std::string section = "Car." + car;
    char buf[64];
    pc.own = !car.empty() && GetPrivateProfileSectionA(section.c_str(), buf, sizeof(buf), IniPath()) > 0;
    // A section that only followed a preset (from before groups: its values were a copy the
    // preset replaced) isn't the car's own: it drives on its group or the shared settings
    // (ReadGroups removes such sections; until the removal is written they may still be there).
    if (pc.own && GetPrivateProfileStringA(section.c_str(), "Preset", "", buf, sizeof(buf), IniPath()) > 0) pc.own = false;
    if (!pc.own && !car.empty())
        if (Group* g = GroupOf(car)) pc.group = g->name;
    LoadGlobal();
    if (pc.own) {
        ReadValues(section);
        LegacyGripBalance(section.c_str(), IniPath(), &g_cfg.grip, &g_cfg.gripBalance);
    } else if (Group* g = pc.group.empty() ? nullptr : FindGroup(pc.group)) {
        ApplyGroup(*g);
    }
    std::string from = pc.own ? "own settings" : !pc.group.empty() ? "group " + pc.group : "[Physics] settings";
    Log("physics: car %s (%s)", car.empty() ? "?" : car.c_str(), from.c_str());
}

bool Physics_OwnSettings() { return pc.own; }
const char* Physics_CarGroup() { return pc.group.c_str(); }

std::vector<std::string> Physics_Groups() {
    ReadGroups();
    std::vector<std::string> names;
    for (const Group& g : s_groups) names.push_back(g.name);
    return names;
}

std::vector<std::string> Physics_GroupCars(const std::string& name) {
    ReadGroups();
    Group* g = FindGroup(name);
    return g ? g->cars : std::vector<std::string>();
}

void SetOwnCar(const std::string& car, bool own) {
    std::string c = Lower(car);
    auto at = std::find(s_ownCars.begin(), s_ownCars.end(), c);
    if (own && at == s_ownCars.end()) s_ownCars.push_back(c);
    if (!own && at != s_ownCars.end()) s_ownCars.erase(at);
}

bool Physics_CarHasOwn(const std::string& car) {
    ReadGroups();
    return std::find(s_ownCars.begin(), s_ownCars.end(), Lower(car)) != s_ownCars.end();
}

void Physics_UseShared() {
    if (pc.car.empty() || Shared()) return;
    SyncGroup();
    SetOwnCar(pc.car, false);
    if (pc.own) IniSave_Queue({{IniPath(), "Car." + pc.car, "", ""}});
    else LeaveGroups(pc.car);
    pc.own = false;
    pc.group.clear();
    LoadGlobal();
    Log("physics: %s back to [Physics] settings", pc.car.c_str());
}

void Physics_UseOwn() {
    if (pc.car.empty() || pc.own) return;
    SyncGroup();
    if (Shared()) StoreGlobal();
    else LeaveGroups(pc.car);
    pc.group.clear();
    pc.own = true;
    SetOwnCar(pc.car, true);
    // From the values in use (an old preset link in the section goes).
    IniSave_Queue({IniErase(IniPath(), "Car." + pc.car, "Preset")});
    WriteValues("Car." + pc.car);
    Log("physics: %s has its own settings", pc.car.c_str());
}

void Physics_UseGroup(const std::string& name) {
    ReadGroups();
    Group* g = FindGroup(name);
    if (pc.car.empty() || !g || (!pc.own && _stricmp(pc.group.c_str(), g->name.c_str()) == 0)) return;
    if (Shared()) StoreGlobal();
    SyncGroup();
    if (pc.own) IniSave_Queue({{IniPath(), "Car." + pc.car, "", ""}});
    SetOwnCar(pc.car, false);
    LeaveGroups(pc.car);
    g->cars.push_back(Lower(pc.car));
    WriteCars(*g);
    pc.own = false;
    pc.group = g->name;
    LoadGlobal();
    ApplyGroup(*g);
    Log("physics: %s joins group %s", pc.car.c_str(), g->name.c_str());
}

std::string Physics_NewGroup() {
    ReadGroups();
    SyncGroup();
    Group g;
    for (int n = 1;; n++) {
        g.name = "Group " + std::to_string(n);
        if (!FindGroup(g.name)) break;
    }
    // The values in use (the car being driven), no cars yet.
    for (int i = 0; i < kNumPerCar; i++) g.value[i] = g_cfg.*kPerCar[i].field, g.has[i] = true;
    s_groups.push_back(g);
    WriteValues("Group." + g.name);
    WriteCars(g);
    Log("physics: group %s made from the values in use", g.name.c_str());
    return g.name;
}

namespace {
int PerCarOf(const float* value) {
    for (int i = 0; i < kNumPerCar; i++)
        if (value == &(g_cfg.*kPerCar[i].field)) return i;
    return -1;
}
}  // namespace

float Physics_GroupValue(const std::string& name, const float* value) {
    ReadGroups();
    int i = PerCarOf(value);
    Group* g = FindGroup(name);
    if (i < 0 || !g) return value ? *value : 0.0f;
    // The current car's group: what it drives on now.
    if (!pc.own && _stricmp(pc.group.c_str(), g->name.c_str()) == 0) return *value;
    if (g->has[i]) return g->value[i];
    return Shared() ? *value : pc.global[i];
}

void Physics_SetGroupValue(const std::string& name, const float* value, float v) {
    ReadGroups();
    int i = PerCarOf(value);
    Group* g = FindGroup(name);
    if (i < 0 || !g) return;
    v = Clamp(v, kPerCar[i].lo, kPerCar[i].hi);
    g->value[i] = v, g->has[i] = true;
    if (!pc.own && _stricmp(pc.group.c_str(), g->name.c_str()) == 0) g_cfg.*kPerCar[i].field = v;
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v);
    IniSave_Queue({{IniPath(), "Group." + g->name, kPerCar[i].key, buf}});
}

const char* Physics_RenameGroup(const std::string& from, const std::string& to) {
    ReadGroups();
    Group* g = FindGroup(from);
    if (!g) return "No group picked";
    size_t a = to.find_first_not_of(' '), b = to.find_last_not_of(' ');
    std::string n = a == std::string::npos ? "" : to.substr(a, b - a + 1);
    if (n.empty()) return "A name needs at least one letter";
    if (n.find_first_of("[]=;,") != std::string::npos) return "Names can't have [ ] = ; or ,";
    Group* other = FindGroup(n);
    if (other && other != g) return "Another group has that name";
    if (g->name == n) return nullptr;
    // The old section's keys into the new one, then the old one goes.
    IniSave_Flush();
    std::vector<char> text(16384);
    DWORD len = GetPrivateProfileSectionA(("Group." + g->name).c_str(), text.data(), (DWORD)text.size(), IniPath());
    std::vector<IniWrite> w;
    for (const char* kv = text.data(); kv < text.data() + len && *kv; kv += strlen(kv) + 1) {
        const char* eq = strchr(kv, '=');
        if (eq) w.push_back({IniPath(), "Group." + n, std::string(kv, eq - kv), eq + 1});
    }
    w.push_back({IniPath(), "Group." + g->name, "", ""});
    IniSave_Queue(w);
    Log("physics: group %s renamed to %s", g->name.c_str(), n.c_str());
    if (_stricmp(pc.group.c_str(), g->name.c_str()) == 0) pc.group = n;
    g->name = n;
    return nullptr;
}

void Physics_DeleteGroup(const std::string& name) {
    ReadGroups();
    for (size_t i = 0; i < s_groups.size(); i++)
        if (_stricmp(s_groups[i].name.c_str(), name.c_str()) == 0) {
            IniSave_Queue({{IniPath(), "Group." + s_groups[i].name, "", ""}});
            Log("physics: group %s deleted, its cars back to [Physics] settings", s_groups[i].name.c_str());
            if (_stricmp(pc.group.c_str(), s_groups[i].name.c_str()) == 0) {
                pc.group.clear();
                LoadGlobal();
            }
            s_groups.erase(s_groups.begin() + i);
            return;
        }
}

void Physics_SetInGroup(const std::string& group, const std::string& car, bool in) {
    ReadGroups();
    Group* g = FindGroup(group);
    if (!g || car.empty()) return;
    if (_stricmp(car.c_str(), pc.car.c_str()) == 0) {
        // The car being driven switches now.
        if (in) Physics_UseGroup(g->name);
        else if (_stricmp(pc.group.c_str(), g->name.c_str()) == 0) Physics_UseShared();
        return;
    }
    if (in) {
        if (GroupOf(car) == g) return;
        LeaveGroups(car);
        g->cars.push_back(Lower(car));
        WriteCars(*g);
    } else {
        for (size_t i = 0; i < g->cars.size(); i++)
            if (_stricmp(g->cars[i].c_str(), car.c_str()) == 0) {
                g->cars.erase(g->cars.begin() + i);
                WriteCars(*g);
                break;
            }
    }
    Log("physics: %s %s group %s", car.c_str(), in ? "added to" : "taken out of", g->name.c_str());
}

// --- profiles: a group or a car's own settings in a file of their own, to share ----------------
// scripts\profiles\<Group|Car> - <name>.ini: [Profile] Kind=Group|Car, Name= the group's name or
// the car (CARS folder name); [Values] every per-car value (missing ones in the group or section
// filled in from the settings they fell back to, so the file drives the same anywhere); Cars= the
// group's cars.
namespace {

std::string ProfileDir() {
    std::string dir = IniPath();
    size_t slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos ? std::string() : dir.substr(0, slash + 1)) + "profiles\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

// A name for a file: the characters Windows doesn't allow in one replaced.
std::string FileSafe(std::string name) {
    for (char& c : name)
        if (strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
    return name;
}

// The value a car on Shared has now.
float SharedValue(int i) { return Shared() ? g_cfg.*kPerCar[i].field : pc.global[i]; }

std::string WriteProfile(const std::string& kind, const std::string& name, const float* values, const std::string* cars) {
    std::string path = ProfileDir() + kind + " - " + FileSafe(name) + ".ini";
    DeleteFileA(path.c_str());
    WritePrivateProfileStringA("Profile", "Kind", kind.c_str(), path.c_str());
    WritePrivateProfileStringA("Profile", "Name", name.c_str(), path.c_str());
    char buf[32];
    for (int i = 0; i < kNumPerCar; i++) {
        snprintf(buf, sizeof(buf), "%g", values[i]);
        WritePrivateProfileStringA("Values", kPerCar[i].key, buf, path.c_str());
    }
    if (cars) WritePrivateProfileStringA("Values", "Cars", cars->c_str(), path.c_str());
    Log("physics: profile written: %s", path.c_str());
    return path;
}

}  // namespace

std::string Physics_ProfileDir() { return ProfileDir(); }

std::string Physics_ExportGroup(const std::string& name) {
    ReadGroups();
    Group* g = FindGroup(name);
    if (!g) return "";
    bool current = !pc.own && _stricmp(pc.group.c_str(), g->name.c_str()) == 0;
    float v[kNumPerCar];
    for (int i = 0; i < kNumPerCar; i++)
        v[i] = current ? g_cfg.*kPerCar[i].field : g->has[i] ? g->value[i] : SharedValue(i);
    std::string cars;
    for (const std::string& c : g->cars) cars += (cars.empty() ? "" : ",") + c;
    return WriteProfile("Group", g->name, v, &cars);
}

std::string Physics_ExportCar(const std::string& car) {
    ReadGroups();
    if (!Physics_CarHasOwn(car)) return "";
    float v[kNumPerCar];
    if (pc.own && _stricmp(pc.car.c_str(), car.c_str()) == 0) {
        for (int i = 0; i < kNumPerCar; i++) v[i] = g_cfg.*kPerCar[i].field;
    } else {
        IniSave_Flush();
        char buf[64];
        std::string section = "Car." + car;
        for (int i = 0; i < kNumPerCar; i++)
            v[i] = GetPrivateProfileStringA(section.c_str(), kPerCar[i].key, "", buf, sizeof(buf), IniPath()) > 0
                       ? Clamp((float)atof(buf), kPerCar[i].lo, kPerCar[i].hi) : SharedValue(i);
    }
    return WriteProfile("Car", Lower(car), v, nullptr);
}

std::vector<PhysicsProfile> Physics_Profiles() {
    std::vector<PhysicsProfile> list;
    std::string dir = ProfileDir();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return list;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string path = dir + fd.cFileName;
        char kind[16], name[128], cars[8192];
        GetPrivateProfileStringA("Profile", "Kind", "", kind, sizeof(kind), path.c_str());
        GetPrivateProfileStringA("Profile", "Name", "", name, sizeof(name), path.c_str());
        GetPrivateProfileStringA("Values", "Cars", "", cars, sizeof(cars), path.c_str());
        PhysicsProfile pr;
        pr.file = fd.cFileName;
        pr.car = _stricmp(kind, "Car") == 0;
        pr.name = name;
        if (pr.name.empty() || (!pr.car && _stricmp(kind, "Group") != 0)) continue;
        std::string list2 = cars;
        for (size_t a = 0; a < list2.size();) {
            size_t b = list2.find(',', a);
            if (b == std::string::npos) b = list2.size();
            std::string c = list2.substr(a, b - a);
            size_t x = c.find_first_not_of(' '), y = c.find_last_not_of(' ');
            if (x != std::string::npos) pr.cars.push_back(Lower(c.substr(x, y - x + 1)));
            a = b + 1;
        }
        list.push_back(pr);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(list.begin(), list.end(), [](const PhysicsProfile& a, const PhysicsProfile& b) {
        return a.car != b.car ? !a.car : _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return list;
}

std::string Physics_ImportProfile(const std::string& file) {
    ReadGroups();
    std::string path = ProfileDir() + file;
    char kind[16], name[128], buf[64];
    GetPrivateProfileStringA("Profile", "Kind", "", kind, sizeof(kind), path.c_str());
    GetPrivateProfileStringA("Profile", "Name", "", name, sizeof(name), path.c_str());
    if (!name[0]) return "Not a profile: " + file;
    // Its values (one missing in the file keeps the value it falls back to here).
    float v[kNumPerCar];
    bool has[kNumPerCar] = {};
    for (int i = 0; i < kNumPerCar; i++)
        if (GetPrivateProfileStringA("Values", kPerCar[i].key, "", buf, sizeof(buf), path.c_str()) > 0)
            v[i] = Clamp((float)atof(buf), kPerCar[i].lo, kPerCar[i].hi), has[i] = true;
    if (_stricmp(kind, "Car") == 0) {
        // The car's own settings, as its [Car.<name>] section.
        std::string car = Lower(name), section = "Car." + car;
        std::vector<IniWrite> w = {{IniPath(), section, "", ""}};
        for (int i = 0; i < kNumPerCar; i++) {
            if (!has[i]) continue;
            snprintf(buf, sizeof(buf), "%g", v[i]);
            w.push_back({IniPath(), section, kPerCar[i].key, buf});
        }
        IniSave_Queue(w);
        SetOwnCar(car, true);
        if (_stricmp(car.c_str(), pc.car.c_str()) == 0) {
            if (Shared()) StoreGlobal();
            SyncGroup();
            pc.group.clear();
            pc.own = true;
            LoadGlobal();
            for (int i = 0; i < kNumPerCar; i++)
                if (has[i]) g_cfg.*kPerCar[i].field = v[i];
        }
        Log("physics: profile %s imported: %s's own settings", file.c_str(), car.c_str());
        return "Imported: " + car + "'s own settings";
    }
    if (_stricmp(kind, "Group") != 0) return "Not a profile: " + file;
    // A group: made, or the one with its name replaced; its cars move into it.
    SyncGroup();
    Group* g = FindGroup(name);
    if (!g) {
        Group ng;
        ng.name = name;
        s_groups.push_back(ng);
        g = &s_groups.back();
    }
    std::vector<IniWrite> w = {{IniPath(), "Group." + g->name, "", ""}};
    for (int i = 0; i < kNumPerCar; i++) {
        g->has[i] = has[i];
        if (!has[i]) continue;
        g->value[i] = v[i];
        snprintf(buf, sizeof(buf), "%g", v[i]);
        w.push_back({IniPath(), "Group." + g->name, kPerCar[i].key, buf});
    }
    IniSave_Queue(w);
    std::string groupName = g->name;
    std::vector<std::string> cars;
    std::vector<char> list(8192);
    GetPrivateProfileStringA("Values", "Cars", "", list.data(), (DWORD)list.size(), path.c_str());
    std::string text = list.data();
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find(',', a);
        if (b == std::string::npos) b = text.size();
        std::string c = text.substr(a, b - a);
        size_t x = c.find_first_not_of(' '), y = c.find_last_not_of(' ');
        if (x != std::string::npos) cars.push_back(Lower(c.substr(x, y - x + 1)));
        a = b + 1;
    }
    for (const std::string& c : cars) LeaveGroups(c);
    g = FindGroup(groupName);     // LeaveGroups doesn't move the list, but look it up again all the same
    g->cars = cars;
    WriteCars(*g);
    // The car being driven, if it is in the group (and has no settings of its own), drives on it now.
    if (!pc.own && !pc.car.empty()) {
        bool in = std::find(cars.begin(), cars.end(), Lower(pc.car)) != cars.end();
        if (in || _stricmp(pc.group.c_str(), groupName.c_str()) == 0) {
            if (Shared()) StoreGlobal();
            pc.group = in ? groupName : "";
            LoadGlobal();
            if (in) ApplyGroup(*g);
        }
    }
    Log("physics: profile %s imported: group %s with %u cars", file.c_str(), groupName.c_str(), (unsigned)cars.size());
    char note[160];
    snprintf(note, sizeof(note), "Imported: group %s with %u cars", groupName.c_str(), (unsigned)cars.size());
    return note;
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
        *(float*)kGlobalGripScale <= 0.0f) {
        Log("physics: sliders unavailable");
        return;
    }
    // Tweak_GlobalGripScale lives in .rdata.
    DWORD old;
    if (!VirtualProtect((void*)kGlobalGripScale, 4, PAGE_READWRITE, &old)) {
        Log("physics: cannot unprotect 0x%06X", (unsigned)kGlobalGripScale);
        return;
    }
    // The traction control's slip limit is an operand in the code: left writable to swap per step.
    if (Expect(kTcSlipPush, {0x68, 0x00, 0x00, 0xA0, 0x41}) &&
        VirtualProtect((void*)(kTcSlipPush + 1), 4, PAGE_EXECUTE_READWRITE, &old))
        s_tcSlip = (float*)(kTcSlipPush + 1);
    else
        Log("physics: unexpected traction control code, the game's traction control stays");
    s_origSusp = (Simulate_t)Detour(kSuspSimulate, 6, (void*)&Hook_SuspSimulate);
    s_origRB = (Simulate_t)Detour(kRBBeginFrame, 7, (void*)&Hook_RBBeginFrame);
    InstallBlownTire();
    if (Expect(kTireModel, {0xD9, 0x86, 0x10, 0x01, 0x00, 0x00}) &&
        Expect(kLateralGrip, {0xD9, 0x81, 0xF0, 0x00, 0x00, 0x00}) &&
        Expect(kComputeLateralForce, {0x51, 0xD9, 0x44, 0x24, 0x0C, 0x56})) {
        WriteCallNops(kTireModel, 6, (void*)&TireModelStub);
        WriteCallNops(kLateralGrip, 6, (void*)&LateralGripStub);
        if (Expect(kWheelBlend, {0xD8, 0x8E, 0x10, 0x01, 0x00, 0x00}) &&
            Expect(kWheelSlipCut, {0xD9, 0x86, 0x10, 0x01, 0x00, 0x00})) {
            WriteCallNops(kWheelBlend, 6, (void*)&WheelBlendStub);
            WriteCallNops(kWheelSlipCut, 6, (void*)&WheelSlipCutStub);
            Log("physics: wheelspin in slides ready (%.2f)", g_cfg.slideWheelspin);
        }
        Log("physics: tyre curve ready (peak at %.1f deg, sliding grip %.2f), friction circle ready (%.2f)",
            g_cfg.tyrePeakAngle, g_cfg.tyreSlideGrip, g_cfg.frictionCircle);
    } else {
        Log("physics: unexpected tyre code, no tyre curve or friction circle");
    }
    if (Expect(kPerfectLaunch + 0x41, {0xD9, 0x86, 0xB8, 0x00, 0x00, 0x00}) &&
        SetSlot(kSlotPerfectLaunch, kPerfectLaunch, (void*)&Hook_PerfectLaunch)) {
        Log("physics: perfect launch %s", g_cfg.physics ? "off" : "as the game's (physics mod off)");
    } else {
        Log("physics: unexpected vehicle code, the perfect launch stays");
    }
    if (Expect(kGripLimit, {0xD8, 0x4C, 0x24, 0x1C, 0xD9, 0x5C, 0x24, 0x1C})) {
        WriteCallNops(kGripLimit, 8, (void*)&GripLimitStub);
        Log("physics: load sensitivity %.2f, suspension firmness %.2f", g_cfg.loadSensitivity, g_cfg.suspFirmness);
    } else {
        Log("physics: unexpected tyre code, no load sensitivity");
    }
    if (RetargetCall(kCallDoAero, kDoAerodynamics, (void*)&Hook_DoAerodynamics))
        Log("physics: downforce ready (%.2f x the game's%s)", g_cfg.downforce,
            g_cfg.downforceSquared ? ", with the speed squared" : "");
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

const CarSample& Physics_Sample() { return s_sample; }

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
