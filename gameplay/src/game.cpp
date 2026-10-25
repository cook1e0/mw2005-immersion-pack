// Gearbox and clutch hooks.
//
// Gear selection: InputPlayer::FetchInput calls DoShifting(delta) (sequential up/down) and
// DoAutoReverse(gas, brake) (shifts into reverse when braking at a standstill and swaps the
// pedals while reversing). With the H-shifter active both calls are replaced: the gear comes
// from the stick through ITransmission::Shift, neutral and reverse included, and the pedals keep
// their meaning in reverse. Manual sequential shifting works the same way: down from first is
// neutral, then reverse (up from reverse back through neutral), and gas drives in reverse.
// InputPlayer::IsAutomaticShift reports false so EngineRacer's automatic gearbox (which also skips
// neutral) stays out of the way.
//
// Clutch: EngineRacer models a friction clutch. In its ENGAGING state the torque passed between
// engine and gearbox is (engine w - gearbox w) * ClutchStiffness * Clutch::Update(), and the
// engine revs freely otherwise. While the pedal is down, the player's clutch is held in that
// state with ClutchStiffness scaled by the pedal, so the game's own physics handle slip, launches
// and free revving. Once the pedal is up and the slip is small, the clutch is handed back to the
// game fully engaged.
//
// Engine wear: the drag race engine (EngineDragster) heats up above the redline, loses power when
// hot and blows at full heat. The same model runs here for the player's EngineRacer in every
// mode: heat is tracked in OnTaskSimulate, torque is scaled in GetEngineTorque and the rev
// limiter can be lifted (UseRevLimiter), the last two through EngineRacer's vtable. A blown
// engine uses the game's own IEngineDamage::Blow, which cuts the throttle and fires EEngineBlown
// (in a race the racer is knocked out). Drag races keep the game's own model.
//
// Car damage: the player's DamageRacer has the game's damage total (DamageVehicle::mDamageTotal,
// IDamageable::GetHealth = 1 - total), but racers have no hit points, so it stays at 0. Every
// impact goes through DamageVehicle::OnImpact (a vtable slot), where the speed change it causes
// (impact force / mass) is turned into damage and written to that total. Props (smackables:
// cones, barrels, signs) do no damage. The game resets the total with the engine (ResetDamage:
// safehouse, race restart). At 100% the car is destroyed like a blown engine. A damaged engine
// loses power, like a hot one (GetEngineTorque).
//
// Popped tires: spike strips call ISpikeable::Puncture (DamageRacer), the tire goes flat 0.5 s later
// and the chassis reports it to the suspension. The handicap itself is in physics.cpp.
#include "game.h"
#include "common.h"
#include "input.h"
#include "ui.h"
#include "hud.h"
#include "physics.h"
#include "vfx.h"
#include "traffic.h"
#include "controls.h"
#include "flow.h"
#include "patch.h"
#include <math.h>
#include <stdio.h>
#include <psapi.h>

namespace {

// --- speed.exe v1.3 -------------------------------------------------------------------------
constexpr uintptr_t kCallDoShifting = 0x6A8D80;     // InputPlayer::FetchInput: call DoShifting
constexpr uintptr_t kDoShifting = 0x69CE20;         // bool InputPlayer::DoShifting(int)
constexpr uintptr_t kCallDoAutoReverse = 0x6A8D89;  // InputPlayer::FetchInput: call DoAutoReverse
constexpr uintptr_t kDoAutoReverse = 0x69CEF0;      // void InputPlayer::DoAutoReverse(float, float)
constexpr uintptr_t kIsAutomaticShift = 0x68D090;   // bool InputPlayer::IsAutomaticShift() (IInput)
constexpr uintptr_t kEngineSimulate = 0x6AF740;     // void EngineRacer::OnTaskSimulate(float)
// OnTaskSimulate: torque = engine torque * throttle + engine braking * (1 - throttle), the engine
// braking (GetBrakingTorque, 0x6A0910) being -(full throttle torque) * the car's ENGINE_BRAKING at
// this rpm. Here, fsub [esi+0x8C] makes that 1 - throttle (x87: 1, engine braking).
constexpr uintptr_t kEngineBrakeWeight = 0x6AFAEB;
constexpr int kER_Throttle = 0x8C;
constexpr uintptr_t kIListFind = 0x5D59F0;          // IUnknown* UTL::COM::Object::_IList::Find(void*)
constexpr uintptr_t kITransmissionHandle = 0x404010;// ITransmission::_IHandle (its address is the key)
constexpr uintptr_t kGetDiffAngVel = 0x692140;      // float EngineRacer::GetDifferentialAngularVelocity(bool)
constexpr uintptr_t kGetGearRatio = 0x691AC0;       // float EngineRacer::GetGearRatio(unsigned)
constexpr uintptr_t kVtblRacerTrans = 0x8AB720;     // EngineRacer's ITransmission vtable
constexpr uintptr_t kVtblDragsterTrans = 0x8ABFC8;  // EngineDragster's ITransmission vtable
constexpr uintptr_t kClutchStiffness = 0x8F8D9C;    // float ClutchStiffness = 20
constexpr uintptr_t kSlotUseRevLimiter = 0x8AB78C;  // EngineRacer vtable: bool UseRevLimiter()
constexpr uintptr_t kUseRevLimiter = 0x6A05E0;
constexpr uintptr_t kSlotEngineTorque = 0x8AB7A8;   // EngineRacer vtable: float GetEngineTorque(float rpm)
constexpr uintptr_t kGetEngineTorque = 0x691D40;
constexpr uintptr_t kSlotRepair = 0x8AB6B4;         // EngineRacer IEngineDamage vtable: void Repair()
constexpr uintptr_t kRepair = 0x691AA0;
constexpr uintptr_t kBlow = 0x6A06E0;               // bool EngineRacer::Blow() (IEngineDamage this)
constexpr uintptr_t kIDamageableHandle = 0x405000;  // IDamageable::_IHandle; vtbl[4] ResetDamage, vtbl[7] Destroy
constexpr uintptr_t kDestroy = 0x69C540;            // void DamageVehicle::Destroy() (IDamageable this)
constexpr uintptr_t kResetDamageRacer = 0x69C930;   // void DamageRacer::ResetDamage()
constexpr uintptr_t kIInputPlayerHandle = 0x403F80; // IInputPlayer::_IHandle; vtbl+4 BlockInput(bool), +8 IsBlocked
constexpr uintptr_t kSlotOnImpact = 0x8AD378;     // DamageRacer vtable: DamageVehicle::OnImpact
constexpr uintptr_t kOnImpact = 0x69C030;
constexpr uintptr_t kGetTireDamage = 0x68CC80;     // eTireDamage DamageRacer::GetTireDamage(unsigned wheel)
constexpr uintptr_t kISpikeableHandle = 0x68CC70;  // ISpikeable::_IHandle; vtbl[1] GetTireDamage, [3] Puncture
constexpr uintptr_t kPuncture = 0x69C730;           // void DamageRacer::Puncture(unsigned wheel) (ISpikeable this)
constexpr uintptr_t kCanDoGameBreaker = 0x6EDC90;   // bool LocalPlayer::CanDoGameBreaker()
constexpr uintptr_t kMomentGetKind = 0x469B10;     // UCrc32 MGamePlayMoment::_GetKind() (returned through a pointer)
constexpr uintptr_t kMessageDeliver = 0x65FAF0;     // void Hermes::Message::Deliver()
constexpr uintptr_t kStringHash32 = 0x5CC240;       // unsigned stringhash32(const char*)
constexpr uintptr_t kRaceStatus = 0x91E000;         // GRaceStatus* GRaceStatus::fObj; +0x1960 mPlayMode (0 = roaming)
constexpr int kRS_RaceParms = 0x1968;               // GRaceStatus: the race's parameters, null outside a race
constexpr uintptr_t kCallRenderHud = 0x6E75A7;      // eDisplayFrame: call sub_6E6E40 (HUD / frontend), before EndScene
constexpr uintptr_t kRenderHud = 0x6E6E40;          // void __cdecl(int)

// InputPlayer (the "this" of DoShifting / DoAutoReverse)
constexpr int kIP_Vehicle = 0x7C;                   // owner; +4 holds its interface list

// EngineRacer
constexpr int kER_Transmission = 0x4C;              // ITransmission sub-object
constexpr int kER_Gear = 0x84;
constexpr int kER_ShiftTimer = 0x88;
constexpr int kER_AngularVelocity = 0xEC;           // engine, rad/s
constexpr int kER_TransmissionVelocity = 0xF4;      // gearbox input mapped to engine rad/s
constexpr int kER_EngineLayout = 0x14C;             // Attrib engine: +0x58 RED_LINE, +0x5C IDLE
constexpr int kER_TranyLayout = 0x160;              // Attrib transmission: +0x80 FINAL_GEAR
constexpr int kER_Clutch = 0x198;
constexpr int kER_Owner = 0x34;                     // ISimable: vtbl+0x20 GetPlayer, +4 interface list
constexpr int kER_Vehicle = 0x48;                   // IVehicle: vtbl+0x20 IsStaging, +0x44 GetVehicleName
constexpr uintptr_t kVtblPVehicle = 0x8AA828;       // PVehicle's IVehicle vtable
constexpr int kPV_Attributes = 0x24;                // ... + this: Attrib::Instance (+4 Collection*, key at +0x20)
constexpr uintptr_t kStringToKey = 0x454640;        // unsigned Attrib::StringToKey(const char*)
constexpr int kER_EngineDamage = 0x78;              // IEngineDamage sub-object (+0x12C mBlown)
constexpr int kER_RPM = 0x180;
constexpr int kER_Blown = 0x1A4;

// DamageVehicle (the object, OnImpact's this)
constexpr int kDV_RigidBody = 0x90;                 // IRigidBody*: vtbl+0x18 GetMass
constexpr int kDI_DamageTotal = 0x3C;               // mDamageTotal, from the IDamageable sub-object
constexpr int kSimableSmackable = 2;                // ISimable vtbl+4 GetSimableType: SIMABLE_SMACKABLE
// Attrib engine layout (kER_EngineLayout): +0x54 MAX_RPM, +0x58 RED_LINE

enum GearID { G_REVERSE = 0, G_NEUTRAL = 1, G_FIRST = 2 };

struct Clutch {
    enum State { ENGAGED = 0, ENGAGING = 1, DISENGAGED = 2 };
    int state;
    float time;
    float engageTime;
};

typedef bool(__thiscall* DoShifting_t)(void* ip, int delta);
typedef void(__thiscall* DoAutoReverse_t)(void* ip, float gas, float brake);
typedef bool(__thiscall* IsAutomaticShift_t)(void* input);
typedef void(__thiscall* EngineSimulate_t)(void* engine, float dT);
typedef void*(__thiscall* IListFind_t)(void* list, void* handle);
typedef float(__thiscall* GetDiffAngVel_t)(void* engine, bool locked);
typedef float(__thiscall* GetGearRatio_t)(void* engine, unsigned gear);
typedef int(__thiscall* TransGetGear_t)(void* trans);
typedef bool(__thiscall* TransShift_t)(void* trans, int gear);
typedef bool(__thiscall* Bool_t)(void* obj);
typedef void*(__thiscall* Ptr_t)(void* obj);
typedef void(__thiscall* Void_t)(void* obj);
typedef float(__thiscall* EngineTorque_t)(void* engine, float rpm);
typedef void(__thiscall* BlockInput_t)(void* input, bool block);
typedef void(__cdecl* RenderHud_t)(int);
typedef float(__thiscall* Float_t)(void* obj);
typedef int(__thiscall* Int_t)(void* obj);
typedef void(__thiscall* OnImpact_t)(void* dv, const float* arm, const float* normal, float force, float speed,
                                     const void* surface, void* other);

DoShifting_t s_origDoShifting = (DoShifting_t)kDoShifting;
DoAutoReverse_t s_origDoAutoReverse = (DoAutoReverse_t)kDoAutoReverse;
IsAutomaticShift_t s_origIsAutomaticShift;
EngineSimulate_t s_origEngineSimulate;
Bool_t s_origCanDoGameBreaker;
float s_baseStiffness = 20.0f;

template <class T> T& Field(uint8_t* p, int off) { return *(T*)(p + off); }
void** Vtbl(void* obj) { return *(void***)obj; }

int TransGear(void* t) { return ((TransGetGear_t)Vtbl(t)[1])(t); }
int TransTopGear(void* t) { return ((TransGetGear_t)Vtbl(t)[2])(t); }
bool TransShift(void* t, int gear) { return ((TransShift_t)Vtbl(t)[3])(t, gear); }

const char* GearName(int g) {
    static const char* names[] = {"R", "N", "1", "2", "3", "4", "5", "6", "7", "8"};
    return g >= 0 && g < 10 ? names[g] : "?";
}

// --- state (game thread only) -------------------------------------------------------------
struct {
    uint8_t* engine = nullptr;  // local player's EngineRacer
    double seen = -1e9;         // last FetchInput for it
    bool hActive = false;       // H-shifter owns the gearbox
    bool clutchActive = false;  // clutch pedal owns the clutch
    float pedal = 0.0f;
    int lastGear = -100;        // gear we last left the gearbox in (-100 = unknown)
    int refused = -100;         // gear we last refused to engage (log once)
    bool missed = false;        // ... for lack of clutch: it stays refused until the stick leaves the gate
    bool forcing = false;       // clutch state is currently ours
    bool launching = false;     // no clutch pedal: the launch clutch slips the clutch (LaunchClutch)
    uint32_t toggles = 0;
    uint32_t ups = 0, downs = 0;    // ShiftUp / ShiftDown presses seen
    bool inputSynced = false;
    uint32_t blows = 0, repairs = 0, hits = 0;
    float heat = 0.0f;          // 0..1, the player engine blows at 1
    int heatStep = 0;           // last logged quarter of heat
    bool blown = false;         // last seen blown state (log changes)
    bool autoShift = true;      // the game's automatic gearbox drives the player's car
    double simSeen = -1e9;      // last OnTaskSimulate of the player's engine
    bool menuOpen = false;      // tuning menu wants the car's controls blocked
    bool menuBlocked = false;   // ... and we blocked them
    int damageStep = 0;         // last logged tenth of damage
    bool totaled = false;       // the player's car is destroyed (health 0)
    bool totaledBlocked = false;// ... in free roam, and we blocked its controls
    double totaledMsg = 0;      // when the free roam "totaled" message was last shown
    bool wearLogged = false;    // last logged WearApplies
    int inputBlocked = -1;      // last logged state of the player's input block (-1 = none yet)
    uint32_t pops = 0;          // DebugPopTires presses seen
    bool popPending = false;    // the menu asked to pop the tires
} s;

bool Fresh() { return Now() - s.seen < 0.5; }
bool IsBlown(uint8_t* eng);
const char* VehicleName(uint8_t* eng);
void BlowEngine(uint8_t* eng);
void AddDamage(uint8_t* eng, float add, float impact);
void PopTires(uint8_t* eng);
float PlayerDamage(uint8_t* eng);
bool IsStaging(uint8_t* eng);

uint8_t* PlayerEngine(void* ip) {
    uint8_t* vehicle = Field<uint8_t*>((uint8_t*)ip, kIP_Vehicle);
    if (!vehicle) return nullptr;
    void* list = Field<void*>(vehicle, 4);
    if (!list) return nullptr;
    void* trans = ((IListFind_t)kIListFind)(list, (void*)kITransmissionHandle);
    if (!trans) return nullptr;
    uintptr_t vt = *(uintptr_t*)trans;
    if (vt != kVtblRacerTrans && vt != kVtblDragsterTrans) return nullptr;   // not a racer engine
    return (uint8_t*)trans - kER_Transmission;
}

// Clutch engagement 0..1 from pedal travel: no grip above BiteStart, full grip below BiteEnd.
float Engagement(float pedal) {
    float t = Clamp((g_cfg.biteStart - pedal) / (g_cfg.biteStart - g_cfg.biteEnd), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Driven wheels turning against the requested direction (e.g. reverse while rolling forward)?
bool WrongDirection(uint8_t* eng, int gear) {
    float w = ((GetDiffAngVel_t)kGetDiffAngVel)(eng, false);   // rad/s, + forward
    if (gear == G_REVERSE) return w > 3.0f;
    return w < -3.0f;
}

// Engine speed close enough to what the target gear would turn it at?
bool RevMatched(uint8_t* eng, int gear) {
    if (g_cfg.revMatchRpm <= 0.0f) return false;
    uint8_t* engine = Field<uint8_t*>(eng, kER_EngineLayout);
    uint8_t* tranny = Field<uint8_t*>(eng, kER_TranyLayout);
    if (!engine || !tranny) return false;
    const float rpmToW = 0.10471976f;
    float maxW = Field<float>(engine, 0x58) * rpmToW;
    float minW = Field<float>(engine, 0x5C) * rpmToW;
    float ratio = ((GetGearRatio_t)kGetGearRatio)(eng, (unsigned)gear) * Field<float>(tranny, 0x80);
    if (gear == G_REVERSE) ratio = -ratio;
    float wheels = ((GetDiffAngVel_t)kGetDiffAngVel)(eng, true);
    float target = minW + wheels * ratio * (maxW - minW) / maxW;
    float diffRpm = fabsf(Field<float>(eng, kER_AngularVelocity) - target) / rpmToW;
    return diffRpm <= g_cfg.revMatchRpm;
}

// Plays a sound "moment" (a streamed one-off sound) the way SFXCTL_Engine::MessageVehicleDestroyed
// does for a totaled car: an MGamePlayMoment delivered to the "MomentStrm" port.
void PlayMoment(uint32_t moment) {
    alignas(16) uint32_t msg[0x48 / 4] = {};
    ((void(__cdecl*)(uint32_t*))kMomentGetKind)(&msg[0]);
    msg[1] = ((uint32_t(__cdecl*)(const char*))kStringHash32)("MomentStrm");
    msg[2] = 0x48;                  // message size
    msg[0x44 / 4] = moment;
    ((void(__thiscall*)(void*))kMessageDeliver)(msg);
}

// A missed shift: the totaled car's engine failure sound ([Shifter] GrindSound). The drag race
// "missed shift" event isn't sent any more: its exhaust smoke stayed where the shift was missed.
void MissShift(uint8_t* eng) {
    if (!Field<uint8_t*>(eng, kER_Owner)) return;
    if (g_cfg.grindSound) PlayMoment(g_cfg.grindSound);
}

// A shift refused for lack of clutch: [Shifter] MisshiftChance of MisshiftDamage car damage.
void MisshiftDamage(uint8_t* eng) {
    if (!g_cfg.damage || g_cfg.misshiftChance <= 0.0f || g_cfg.misshiftDamage <= 0.0f || IsStaging(eng)) return;
    static uint32_t seed = (uint32_t)(Now() * 1e6) | 1;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    if ((seed >> 8) * (1.0f / 16777216.0f) >= g_cfg.misshiftChance) return;
    Log("shift: misshift damages the car (+%.0f%%)", g_cfg.misshiftDamage * 100.0f);
    AddDamage(eng, g_cfg.misshiftDamage, 0.0f);
}

void ApplyShifter(uint8_t* eng, const InputSnapshot& in) {
    void* trans = eng + kER_Transmission;
    int cur = TransGear(trans);
    int want = in.stickGear < 0 ? G_NEUTRAL : in.stickGear == 0 ? G_REVERSE : in.stickGear + 1;
    if (want > TransTopGear(trans)) want = G_NEUTRAL;   // gate beyond this car's top gear

    if (cur != s.lastGear) {
        // First time, or the game changed gear itself (race start, reset, ...): follow the stick.
        if (want != cur) TransShift(trans, want);
        s.lastGear = TransGear(trans);
        return;
    }
    if (want == cur) {
        s.refused = -100;
        s.missed = false;
        return;
    }
    if (want != s.refused) s.missed = false;

    bool clutchIn = s.clutchActive && in.clutchPedal >= g_cfg.shiftPedal;
    const char* refuse = nullptr;
    if (want != G_NEUTRAL) {
        if (g_cfg.requireClutch && s.clutchActive && !clutchIn && !RevMatched(eng, want))
            refuse = "clutch not pressed";
        else if (g_cfg.blockWrongDirection && WrongDirection(eng, want))
            refuse = "car rolling the other way";
        else if (s.missed && g_cfg.reselectAfterMiss && !g_cfg.latching)   // a latched key can't be re-selected
            refuse = "missed shift, select the gear again";
    }
    if (refuse) {
        // The stick has left the old gate, so the box is in neutral until the gear goes in.
        if (cur != G_NEUTRAL) TransShift(trans, G_NEUTRAL);
        if (s.refused != want) {
            if (refuse[0] == 'c') {   // "clutch not pressed"
                if (g_cfg.grind) MissShift(eng);
                MisshiftDamage(eng);
            }
        }
        if (refuse[0] == 'c') s.missed = true;
        s.refused = want;
    } else if (TransShift(trans, want)) {
        // With the clutch already down, the game's simulated clutch kick is redundant.
        if (clutchIn && !g_cfg.keepShiftDelay) Field<float>(eng, kER_ShiftTimer) = 0.0f;
        s.refused = -100;
        s.missed = false;
    }
    s.lastGear = TransGear(trans);
}

bool __fastcall Hook_DoShifting(void* ip, void*, int delta) {
    Input_Start();
    InputSnapshot in = Input_Get();
    if (!s.inputSynced) {
        s.inputSynced = true;
        s.toggles = in.toggles;
        s.ups = in.shiftUp;
        s.downs = in.shiftDown;
    }
    if (in.toggles != s.toggles) {
        s.toggles = in.toggles;
        g_cfg.sequential = !g_cfg.sequential;
        s.lastGear = -100;
        Log("toggle: %s shifting", g_cfg.sequential ? "sequential" : "H-pattern");
    }
    int presses = (int)(in.shiftUp - s.ups) - (int)(in.shiftDown - s.downs);
    s.ups = in.shiftUp;
    s.downs = in.shiftDown;

    uint8_t* eng = PlayerEngine(ip);
    if (eng != s.engine) {
        s.engine = eng;
        s.lastGear = -100;
        s.forcing = false;
        s.heat = 0.0f;
        s.heatStep = 0;
        s.menuBlocked = false;
        s.inputBlocked = -1;
        s.blown = eng && IsBlown(eng);
        if (eng) Log("player engine at %p%s", eng, s.blown ? " (blown)" : "");
        if (eng) Physics_SetCar(VehicleName(eng));
    }
    if (!eng) {
        s.hActive = s.clutchActive = false;
        return s_origDoShifting(ip, delta);
    }
    s.seen = Now();
    s.clutchActive = in.clutchBound;
    s.pedal = in.clutchPedal;
    bool h = !g_cfg.sequential && in.shifterBound;
    if (h != s.hActive) {
        Log("H-shifter %s", h ? "active" : "inactive");
        s.lastGear = -100;
    }
    s.hActive = h;

    if (s.hActive) {
        ApplyShifter(eng, in);
        return false;   // the game's shift up/down buttons do nothing while the stick decides
    }

    // Sequential (ours or the game's manual gearbox): our ShiftUp / ShiftDown bindings, or the
    // game's own shift controls.
    if (delta == 0 && presses != 0) delta = presses > 0 ? 1 : -1;
    bool manual = !s.autoShift;
    bool clutchIn = s.clutchActive && in.clutchPedal >= g_cfg.shiftPedal;
    bool needClutch = g_cfg.requireClutch && s.clutchActive && (g_cfg.sequential || g_cfg.sequentialNeedsClutch);
    if (delta != 0 && needClutch && !clutchIn) {
        // The game's shift control and ShiftUp / ShiftDown can be the same key: one press, one grind.
        static double lastRefused = -1e9;
        if (Now() - lastRefused > 0.2) {
            if (g_cfg.grind) MissShift(eng);
            MisshiftDamage(eng);
        }
        lastRefused = Now();
        delta = 0;
    }
    if (!manual) {
        bool shifted = s_origDoShifting(ip, delta);
        if (shifted && clutchIn && !g_cfg.keepShiftDelay) Field<float>(eng, kER_ShiftTimer) = 0.0f;
        return shifted;
    }
    // Manual: one gear per press through the whole box, R N 1 2 ... like the gates of an H.
    if (delta == 0) return s_origDoShifting(ip, 0);
    void* trans = eng + kER_Transmission;
    int cur = TransGear(trans);
    int want = cur + (delta > 0 ? 1 : -1);
    if (want < G_REVERSE || want > TransTopGear(trans)) return false;
    if (want == G_REVERSE && g_cfg.blockWrongDirection && WrongDirection(eng, want)) {
        return false;
    }
    bool shifted = TransShift(trans, want);
    if (shifted && clutchIn && !g_cfg.keepShiftDelay) Field<float>(eng, kER_ShiftTimer) = 0.0f;
    return shifted;
}

void __fastcall Hook_DoAutoReverse(void* ip, void*, float gas, float brake) {
    // Reverse is a gear of its own (the shifter's gate, or below neutral in sequential): the
    // pedals keep their roles.
    if ((s.hActive || !s.autoShift) && Fresh()) return;
    s_origDoAutoReverse(ip, gas, brake);
}

// Called by the player's EngineRacer::DoShifting every step.
bool __fastcall Hook_IsAutomaticShift(void* input) {
    // Our shifting modes are manual; otherwise the game's transmission setting decides.
    bool automatic = !((s.hActive || g_cfg.sequential) && Fresh()) && s_origIsAutomaticShift(input);
    s.autoShift = automatic;
    return automatic;
}

// --- engine wear ---------------------------------------------------------------------------
bool IsDragster(uint8_t* eng) { return *(uintptr_t*)(eng + kER_Transmission) == kVtblDragsterTrans; }
bool IsBlown(uint8_t* eng) { return Field<bool>(eng, kER_Blown); }

// Our model runs for the local player's EngineRacer only (drag races bring their own), and only
// with manual shifting: the automatic gearbox over-revs on its own in wheelspin.
bool WearApplies(uint8_t* eng) {
    return g_cfg.engineWear && eng == s.engine && Fresh() && !s.autoShift && !IsDragster(eng);
}

// The vehicle's name, e.g. "bmwm3gtre46": the car folder (CARS\<NAME>) whose Attrib key matches
// the vehicle's attribute collection or one of its parents, or the key in hex. Null if not a PVehicle.
const char* VehicleName(uint8_t* eng) {
    uint8_t* vehicle = Field<uint8_t*>(eng, kER_Vehicle);
    if (!vehicle || *(uintptr_t*)vehicle != kVtblPVehicle) return nullptr;
    uint8_t* collection = Field<uint8_t*>(vehicle, kPV_Attributes + 4);
    if (!collection) return nullptr;
    uint32_t key = Field<uint32_t>(collection, 0x20);
    static std::vector<std::pair<uint32_t, std::string>> cars;
    if (cars.empty()) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((std::string(GameDir()) + "CARS\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                std::string n = fd.cFileName;
                for (auto& c : n) c = (char)tolower((unsigned char)c);
                cars.push_back({((uint32_t(__cdecl*)(const char*))kStringToKey)(n.c_str()), n});
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        cars.push_back({0, ""});    // looked once
    }
    // A customised car (career, quick race) is a collection whose parent chain leads to the model.
    for (uint8_t* col = collection; col; col = Field<uint8_t*>(col, 0x10)) {
        uint32_t k = Field<uint32_t>(col, 0x20);
        for (auto& c : cars)
            if (c.first == k && !c.second.empty()) return c.second.c_str();
    }
    static char hex[16];
    snprintf(hex, sizeof(hex), "%08X", key);
    return hex;
}

bool IsStaging(uint8_t* eng) {
    void* vehicle = Field<void*>(eng, kER_Vehicle);
    return vehicle && ((Bool_t)Vtbl(vehicle)[0x20 / 4])(vehicle);
}

bool InSpeedBreaker(uint8_t* eng) {
    void* owner = Field<void*>(eng, kER_Owner);
    void* player = owner ? ((Ptr_t)Vtbl(owner)[0x20 / 4])(owner) : nullptr;
    return player && ((Bool_t)Vtbl(player)[0x40 / 4])(player);
}

void* PlayerDamageable(uint8_t* eng) {
    uint8_t* owner = Field<uint8_t*>(eng, kER_Owner);
    void* list = owner ? Field<void*>(owner, 4) : nullptr;
    void* damage = list ? ((IListFind_t)kIListFind)(list, (void*)kIDamageableHandle) : nullptr;
    // Only the racer damage model we know (DamageRacer / DamageVehicle).
    return damage && Vtbl(damage)[7] == (void*)kDestroy && Vtbl(damage)[4] == (void*)kResetDamageRacer ? damage : nullptr;
}

// The game's handling of a destroyed racer (a GRaceStatus method at 0x5FDE80) reads the race's
// parameters (GRaceStatus +0x1968) without checking them: destroying the car while there are none
// - free roam, or a race just over - crashed the game (reading 0x4 at 0x5FDEB5, 2026-10-20, an
// engine blowing in a drag race). IDamageable::Destroy is only used in a race that has them.
bool CanDestroyCar() {
    uint8_t* race = *(uint8_t**)kRaceStatus;
    return race && Field<int>(race, 0x1960) != 0 && Field<void*>(race, kRS_RaceParms) != nullptr;
}

// Totaled: the engine is marked blown without EEngineBlown, and the car is destroyed the way a
// drag race crash does it (IDamageable::Destroy -> EVehicleDestroyed: the racer is totaled,
// controls blocked, totaled camera and race-over message). Otherwise (and outside a race, see
// CanDestroyCar) the game's own Blow.
void BlowEngine(uint8_t* eng) {
    s.heat = 1.0f;
    if (IsBlown(eng)) return;
    void* damage = g_cfg.totaled && CanDestroyCar() ? PlayerDamageable(eng) : nullptr;
    if (damage) {
        Field<bool>(eng, kER_Blown) = true;
        ((Void_t)kDestroy)(damage);
        Log("engine: BLOWN (car totaled)");
    } else if (((Bool_t)kBlow)(eng + kER_EngineDamage)) {
        Log("engine: BLOWN");
    }
}

void* PlayerInput(uint8_t* eng) {
    uint8_t* owner = Field<uint8_t*>(eng, kER_Owner);
    void* list = owner ? Field<void*>(owner, 4) : nullptr;
    return list ? ((IListFind_t)kIListFind)(list, (void*)kIInputPlayerHandle) : nullptr;
}

// The game blocks the player's input when the engine blows (EEngineBlown).
void BlockPlayerInput(uint8_t* eng, bool block) {
    void* input = PlayerInput(eng);
    if (input) ((BlockInput_t)Vtbl(input)[1])(input, block);
}

bool PlayerInputBlocked(uint8_t* eng) {
    void* input = PlayerInput(eng);
    return input && ((Bool_t)Vtbl(input)[2])(input);
}

// While the menu is open the car's controls are blocked, so the menu keys don't also steer.
// Only undone if we did it, and never while the engine is blown.
void UpdateMenuBlock(uint8_t* eng) {
    if (s.menuOpen && !s.menuBlocked && !PlayerInputBlocked(eng)) {
        BlockPlayerInput(eng, true);
        s.menuBlocked = true;
    } else if (!s.menuOpen && s.menuBlocked) {
        if (!IsBlown(eng)) BlockPlayerInput(eng, false);
        s.menuBlocked = false;
    }
}

bool InRace() {
    uint8_t* race = *(uint8_t**)kRaceStatus;
    return race && Field<int>(race, 0x1960) != 0;
}

// A destroyed car ends a race with the game's "Totaled" screen. Free roam has no such screen, so
// the car is made undrivable (engine marked blown, controls blocked) and a HUD message marks it
// until the game repairs it (safehouse).
void UpdateTotaled(uint8_t* eng) {
    void* damage = PlayerDamageable(eng);
    bool totaled = damage && ((Float_t)Vtbl(damage)[5])(damage) <= 0.0f;   // GetHealth
    if (totaled != s.totaled) {
        s.totaled = totaled;
        s.totaledMsg = 0;
        Log("car %s", totaled ? (InRace() ? "totaled (race)" : "totaled (free roam)") : "no longer totaled");
        if (!totaled && s.totaledBlocked) {
            BlockPlayerInput(eng, false);
            s.totaledBlocked = false;
        }
    }
    if (!totaled || InRace()) return;
    if (!s.totaledBlocked) {
        Field<bool>(eng, kER_Blown) = true;
        if (!PlayerInputBlocked(eng)) {
            BlockPlayerInput(eng, true);
            s.totaledBlocked = true;
        }
    }
    if (Now() - s.totaledMsg > 6.0 && Hud_Message("TOTALED")) s.totaledMsg = Now();
}

// Runs every step for the player's engine, also while blown (input blocked, so no FetchInput).
void PlayerEngineStep(uint8_t* eng) {
    s.simSeen = Now();
    Flow_PlayerStep();
    UpdateMenuBlock(eng);
    InputSnapshot in = Input_Get();
    if (in.repairs != s.repairs) {
        s.repairs = in.repairs;
        Log("engine: debug repair");
        void* damage = PlayerDamageable(eng);
        if (damage) ((Void_t)Vtbl(damage)[4])(damage);   // ResetDamage, repairs the engine too
        else ((Void_t)kRepair)(eng + kER_EngineDamage);
        BlockPlayerInput(eng, false);
        s.heat = 0.0f;
        s.heatStep = 0;
    }
    if (in.hits != s.hits) {
        s.hits = in.hits;
        AddDamage(eng, 0.1f, 0.0f);
    }
    if (in.pops != s.pops || s.popPending) {
        s.pops = in.pops;
        s.popPending = false;
        PopTires(eng);
    }
    if (in.blows != s.blows) {
        s.blows = in.blows;
        Log("engine: debug blow");
        BlowEngine(eng);
    }
    UpdateTotaled(eng);
    s.inputBlocked = PlayerInputBlocked(eng) ? 1 : 0;
    bool blown = IsBlown(eng);
    if (blown != s.blown) {
        s.blown = blown;
        if (!blown) Log("engine: no longer blown");
    }
    if (blown) s.heat = 1.0f;
}

constexpr float kBadlyDamaged = 0.75f;      // from this much car damage the engine heats up faster
constexpr float kDamagedHeatUp = 1.33f;     // ... by this factor
constexpr float kNosHeatUp = 2.0f;          // nitrous on at the limiter heats it twice as fast
constexpr float kNosHeatUpDamaged = 1.33f;  // ... a third faster on a badly damaged car (on top of its own 1.33)
constexpr int kER_IEngine = 0x54;           // EngineRacer's IEngine sub-object (vtable 0x8AB6E0)
constexpr uintptr_t kVtblRacerEngine = 0x8AB6E0;

// The nitrous is on (IEngine::IsNOSEngaged, EngineRacer 0x6A0410, slot 8).
bool NosOn(uint8_t* eng) {
    void* iface = eng + kER_IEngine;
    if (*(uintptr_t*)iface != kVtblRacerEngine) return false;
    return (((uint32_t(__thiscall*)(void*))Vtbl(iface)[8])(iface) & 0xFF) != 0;
}

// The game resets the engine at a race start, restart or respawn: a rolling start puts the car at
// speed in whatever gear the shifter is in (2nd at 170 km/h blew the engine in 4 s), so the heat
// waits this long after a reset for a gear to be picked. Nor does it rise in a cutscene (NIS).
constexpr double kWearGraceAfterReset = 8.0;
constexpr uintptr_t kNisInstance = 0x9885C8;          // UTL::Singleton<INIS>::mInstance: non-null during an NIS
double s_wearGraceUntil = 0;

void UpdateWear(uint8_t* eng, float dT) {
    if (IsBlown(eng)) return;
    if (Now() < s_wearGraceUntil || *(void**)kNisInstance) return;
    // Like EngineDragster::ComputeEngineHeat: frozen at the start line and in the speedbreaker.
    if (dT <= 0.0f || IsStaging(eng) || InSpeedBreaker(eng)) return;
    uint8_t* layout = Field<uint8_t*>(eng, kER_EngineLayout);
    if (!layout) return;
    float maxRpm = Field<float>(layout, 0x54), redline = Field<float>(layout, 0x58);
    float rpm = Field<float>(eng, kER_RPM);
    // The limiter holds the engine at the redline, so count "on the limiter" as over it.
    float rate = -g_cfg.coolDown;
    if (rpm >= redline * 0.99f) {
        float over = maxRpm > redline ? Clamp((rpm - redline) / (maxRpm - redline), 0.0f, 1.0f) : 0.0f;
        rate = g_cfg.heatUp + (g_cfg.heatUpMax - g_cfg.heatUp) * over;
        // A wrecked car's engine runs hotter: a third faster from 75% damage. The nitrous on the
        // limiter heats it twice as fast, but only a third faster on a car that is already badly
        // damaged, so the two don't pile up (1.33 * 1.33, not 1.33 * 2). Cooling unchanged.
        bool damaged = PlayerDamage(eng) >= kBadlyDamaged;
        float k = damaged ? kDamagedHeatUp : 1.0f;
        bool nos = NosOn(eng);
        if (nos) k *= damaged ? kNosHeatUpDamaged : kNosHeatUp;
        rate *= k;
        static bool loggedNos;
        if (nos && !loggedNos) Log("engine: nitrous on the limiter, heating x%.2f", k);
        loggedNos = nos;
    }
    s.heat = Clamp(s.heat + rate * dT, 0.0f, 1.0f);
    int step = (int)(s.heat * 4.0f);
    s.heatStep = step;
    if (s.heat >= 1.0f) BlowEngine(eng);
}

bool __fastcall Hook_UseRevLimiter(uint8_t* eng) {
    if (WearApplies(eng) && !g_cfg.revLimiter && !IsStaging(eng)) return false;
    return true;
}

// Fraction of torque left when `level` (heat or damage, 0..1) is past `start`, losing `loss` at 1.
float PowerLeft(float level, float start, float loss) {
    if (level <= start) return 1.0f;
    return 1.0f - Clamp((level - start) / (1.0f - start), 0.0f, 1.0f) * loss;
}

float __fastcall Hook_GetEngineTorque(uint8_t* eng, void*, float rpm) {
    float torque = ((EngineTorque_t)kGetEngineTorque)(eng, rpm);
    if (torque <= 0.0f) return torque;
    if (WearApplies(eng)) torque *= PowerLeft(s.heat, g_cfg.powerLossStart, g_cfg.powerLoss);
    if (g_cfg.damage && eng == s.engine && Fresh() && !IsDragster(eng))
        torque *= PowerLeft(PlayerDamage(eng), g_cfg.damageLossStart, g_cfg.damageLoss);
    return torque;
}

// Logged with the caller so we can see which game paths give the player a fresh engine.
void __fastcall Hook_Repair(uint8_t* damage) {
    uint8_t* eng = damage - kER_EngineDamage;
    if (eng == s.engine) {
        Log("engine: repaired by game (caller %p, was %s, heat %.2f)", __builtin_return_address(0),
            IsBlown(eng) ? "blown" : "ok", s.heat);
        s.heat = 0.0f;
        s.heatStep = 0;
        s_wearGraceUntil = Now() + kWearGraceAfterReset;
    }
    ((Void_t)kRepair)(damage);
}

// --- car damage ------------------------------------------------------------------------------
float PlayerDamage(uint8_t* eng) {
    void* damage = PlayerDamageable(eng);
    return damage ? 1.0f - ((Float_t)Vtbl(damage)[5])(damage) : 0.0f;   // GetHealth
}

// At 100% the car is destroyed (IDamageable::Destroy, as for a blown engine). Destroy only acts
// below 100% and sets the total to 1 itself.
void AddDamage(uint8_t* eng, float add, float impact) {
    uint8_t* iface = (uint8_t*)PlayerDamageable(eng);
    if (!iface) return;
    float& total = Field<float>(iface, kDI_DamageTotal);
    if (total >= 1.0f) return;
    float now = total + add;
    int step = (int)(now * 10.0f + 0.001f);
    if (step != s.damageStep) {
        Log("damage: %.0f%% (impact %.1f m/s)", now * 100.0f, impact);
        s.damageStep = step;
    }
    if (now < 1.0f) {
        total = now;
        return;
    }
    if (!CanDestroyCar()) {
        // Health 0 without Destroy: in free roam UpdateTotaled makes the car undrivable.
        total = 1.0f;
        Log("damage: car TOTALED (no race to end, not destroyed)");
        return;
    }
    total = 0.999f;
    ((Void_t)kDestroy)(iface);
    Log("damage: car TOTALED");
}

// The force feedback mod's crash jolts: every impact that damages the car is counted in its
// DamageHits (NfsmwFfb_Setting), after its strength (the speed change over the damage threshold).
void SignalFfbDamage(float strength) {
    static float* hits = nullptr;
    static float* str = nullptr;
    static double looked = -1e9;
    if (!hits && Now() - looked > 5.0) {
        looked = Now();
        HMODULE mods[512];
        DWORD bytes = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes))
            for (DWORD k = 0; k < bytes / sizeof(HMODULE) && k < 512 && !hits; k++) {
                FARPROC get = GetProcAddress(mods[k], "NfsmwFfb_Setting");
                if (!get) continue;
                auto setting = (float* (__cdecl*)(const char*))get;
                hits = setting("DamageHits");
                str = hits ? setting("DamageHitStrength") : nullptr;
            }
        if (hits) Log("damage: impacts reported to the force feedback mod (damaging crashes)");
    }
    if (!hits || !str) return;
    *str = strength;
    *hits += 1.0f;
}

void __fastcall Hook_OnImpact(uint8_t* dv, void*, const float* arm, const float* normal, float force, float speed,
                              const void* surface, void* other) {
    ((OnImpact_t)kOnImpact)(dv, arm, normal, force, speed, surface, other);
    if (!g_cfg.damage || !s.engine || Now() - s.simSeen > 0.3) return;
    uint8_t* iface = (uint8_t*)PlayerDamageable(s.engine);
    if (!iface || iface < dv || iface >= dv + 0x100) return;   // not the player's car
    if (IsStaging(s.engine)) return;
    // Props (cones, barrels, signs, lamp posts) fly off without hurting the car.
    if (other && ((Int_t)Vtbl(other)[1])(other) == kSimableSmackable) return;
    void* rb = Field<void*>(dv, kDV_RigidBody);
    float mass = rb ? ((Float_t)Vtbl(rb)[6])(rb) : 0.0f;
    if (!(mass > 1.0f)) return;
    float dv_ms = force / mass;
    float add = (dv_ms - g_cfg.damageMinImpact) * 0.01f * g_cfg.damageStrength;
    if (g_cfg.logInputs) Log("damage: impact %.1f m/s (speed %.1f, mass %.0f)%s", dv_ms, speed, mass, add > 0 ? "" : " ignored");
    if (add > 0.0f) {
        AddDamage(s.engine, add, dv_ms);
        SignalFfbDamage(dv_ms / (g_cfg.damageMinImpact > 0.5f ? g_cfg.damageMinImpact : 0.5f));
    }
}

// --- popped tires ----------------------------------------------------------------------------
// Tires with damage (ISpikeable::GetTireDamage: 0 = intact).
int PoppedTires(uint8_t* eng) {
    uint8_t* owner = Field<uint8_t*>(eng, kER_Owner);
    void* list = owner ? Field<void*>(owner, 4) : nullptr;
    void* spike = list ? ((IListFind_t)kIListFind)(list, (void*)kISpikeableHandle) : nullptr;
    if (!spike || Vtbl(spike)[1] != (void*)kGetTireDamage) return 0;
    int n = 0;
    for (unsigned w = 0; w < 4; w++) n += ((int(__thiscall*)(void*, unsigned))kGetTireDamage)(spike, w) != 0;
    return n;
}

// Testing: the game's own puncture on all four wheels, as a spike strip does it.
void PopTires(uint8_t* eng) {
    uint8_t* owner = Field<uint8_t*>(eng, kER_Owner);
    void* list = owner ? Field<void*>(owner, 4) : nullptr;
    void* spike = list ? ((IListFind_t)kIListFind)(list, (void*)kISpikeableHandle) : nullptr;
    if (!spike || Vtbl(spike)[3] != (void*)kPuncture) {
        Log("tires: debug pop unavailable (no racer damage model)");
        return;
    }
    for (unsigned w = 0; w < 4; w++) ((void(__thiscall*)(void*, unsigned))kPuncture)(spike, w);
    Log("tires: debug pop (all four)");
}

bool __fastcall Hook_CanDoGameBreaker(void* player) {
    return g_cfg.speedbreaker && s_origCanDoGameBreaker(player);
}

// --- clutch ----------------------------------------------------------------------------------
// The engine step with the clutch held ENGAGING at `bite` of full grip (0..1). Clutch::Update
// returns 0.25 for ENGAGING with time == engageTime, and the huge time keeps the game from
// finishing the engagement on its own (Clutch::Engage only acts on a DISENGAGED clutch, so the
// game's own Engage calls leave it alone). The torque passed is (engine w - gearbox w) *
// ClutchStiffness * that 0.25, so 4x the stiffness is the game's full clutch.
void SlipClutch(uint8_t* eng, float dT, float bite) {
    Clutch& c = Field<Clutch>(eng, kER_Clutch);
    c.state = Clutch::ENGAGING;
    c.time = c.engageTime = 1.0e6f;
    float* stiffness = (float*)kClutchStiffness;
    float saved = *stiffness;
    *stiffness = s_baseStiffness * 4.0f * bite * g_cfg.strength;
    s_origEngineSimulate(eng, dT);
    *stiffness = saved;
}

// A clutch SlipClutch left ENGAGING (its huge time) and nobody holds any more: the game finishes the
// engagement in 0.05 s, as its own Engage(0.05) would; else it would slip at a quarter of its grip
// for good.
void ReleaseClutch(uint8_t* eng) {
    Clutch& c = Field<Clutch>(eng, kER_Clutch);
    if (c.state == Clutch::ENGAGING && c.time > 1000.0f) c.time = c.engageTime = 0.05f;
}

// Launch clutch, without a clutch pedal: the game engages the clutch in 0.05 s once the throttle
// passes 10% (OnTaskSimulate, 0x6AF9F2), so pulling away the engine is held near idle, where it has
// little torque, and the car crawls until the throttle is well in. Here, from a stop in first or
// reverse (the game's clutch DISENGAGED, the gearbox within 200 rpm of idle) the clutch slips, its
// grip rising from 300 rpm below to 300 rpm above a target rpm: idle + throttle * [Clutch]
// LaunchRevs of the way to the redline. The engine settles near that target and the car pulls on
// it; the clutch locks once the car is moving (the gearbox 200 rpm past idle: standing, the gearbox
// reads idle, as the engine does) with the gearbox within LockRPM of the engine, or past the target.
// Off the throttle the clutch stays open (as the game's does), and if the car is already rolling the
// game takes over again. Returns false when it isn't launching: the caller runs the game's step.
bool LaunchClutch(uint8_t* eng, float dT) {
    auto stop = [&]() {
        if (s.launching) ReleaseClutch(eng);
        s.launching = false;
        return false;
    };
    uint8_t* layout = Field<uint8_t*>(eng, kER_EngineLayout);
    int gear = Field<int>(eng, kER_Gear);
    if (g_cfg.launchRevs <= 0.0f || !layout || (gear != G_FIRST && gear != G_REVERSE)) return stop();
    float idle = Field<float>(layout, 0x5C), redline = Field<float>(layout, 0x58);
    if (!(idle > 100.0f && redline > idle + 500.0f)) return stop();
    Clutch& c = Field<Clutch>(eng, kER_Clutch);
    float rpm = Field<float>(eng, kER_AngularVelocity) * 9.5492966f;
    float boxRpm = Field<float>(eng, kER_TransmissionVelocity) * 9.5492966f;
    float throttle = Clamp(Field<float>(eng, kER_Throttle), 0.0f, 1.0f);
    if (!s.launching) {
        if (c.state != Clutch::DISENGAGED || boxRpm > idle + 200.0f) return false;
        s.launching = true;
    }
    float target = idle + throttle * g_cfg.launchRevs * (redline - idle);
    bool moving = boxRpm > idle + 200.0f;
    if (throttle < 0.05f) {
        // Off the throttle: open, as the game's clutch is; rolling, the game takes over.
        if (moving) return stop();
        SlipClutch(eng, dT, 0.0f);
        return true;
    }
    if (moving && (fabsf(rpm - boxRpm) < g_cfg.lockRpm || boxRpm > target)) {
        s.launching = false;
        c.state = Clutch::ENGAGED;
        c.time = c.engageTime = 0.0f;
        return false;
    }
    SlipClutch(eng, dT, Smoothstep(target - 300.0f, target + 300.0f, rpm));
    return true;
}

void ClutchSimulate(uint8_t* eng, float dT) {
    if (eng != s.engine || !s.clutchActive || !Fresh()) {
        if (eng == s.engine && !s.clutchActive && Fresh() && LaunchClutch(eng, dT)) return;
        if (eng == s.engine) {
            // The pedal unbound or the input gone stale while it held the clutch: hand it back.
            if (s.forcing || s.launching) ReleaseClutch(eng);
            s.forcing = s.launching = false;
        }
        s_origEngineSimulate(eng, dT);
        return;
    }
    Clutch& c = Field<Clutch>(eng, kER_Clutch);
    float e = Engagement(s.pedal);
    int gear = Field<int>(eng, kER_Gear);
    float slipRpm = fabsf(Field<float>(eng, kER_AngularVelocity) - Field<float>(eng, kER_TransmissionVelocity)) * 9.5492966f;
    // Pedal up: keep slipping until engine and gearbox speeds meet, then lock up.
    bool force = e < 0.999f || (s.forcing && gear != G_NEUTRAL && slipRpm > g_cfg.lockRpm);
    if (!force) {
        if (s.forcing) {
            s.forcing = false;
            c.state = Clutch::ENGAGED;
            c.time = c.engageTime = 0.0f;
        }
        s_origEngineSimulate(eng, dT);
        return;
    }
    s.forcing = true;
    SlipClutch(eng, dT, e);
}

}  // namespace

// Engine braking against the throttle: the game weighs it by 1 - throttle, so it fades out only at
// full throttle and the net torque turns positive only past ENGINE_BRAKING / (1 + ENGINE_BRAKING)
// of the pedal (25-40% by the car). For the player's engine it is gone by [Engine]
// EngineBrakingFade instead (a real engine stops braking as soon as the throttle opens), falling in a
// straight line from full at a closed throttle.
extern "C" uint8_t* s_brakeEngine;
uint8_t* s_brakeEngine;   // the player's EngineRacer while EngineBrakingFade applies, else null

extern "C" __attribute__((used)) float EngineBrakeWeight(uint8_t* eng) {
    float t = Field<float>(eng, kER_Throttle);
    return 1.0f - Clamp(t / g_cfg.engineBrakeFade, 0.0f, 1.0f);
}

// In place of fsub [esi+0x8C] (6 bytes): for s_brakeEngine, the 1 on the x87 stack becomes
// EngineBrakeWeight; the engine braking under it is held in memory over the call. eax, ecx and edx
// are kept (ebx, the gear, is callee-saved); the flags are free.
__asm__(
    ".text\n"
    "_EngineBrakeStub:\n"
    "    cmpl _s_brakeEngine, %esi\n"
    "    je 1f\n"
    "    fsubs 0x8C(%esi)\n"
    "    ret\n"
    "1:  fstp %st(0)\n"
    "    subl $4, %esp\n"
    "    fstps (%esp)\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %esi\n"
    "    call _EngineBrakeWeight\n"
    "    addl $4, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    flds (%esp)\n"
    "    fxch %st(1)\n"
    "    addl $4, %esp\n"
    "    ret\n");
extern "C" void EngineBrakeStub();

namespace {

// [Physics] LogPhysics: four times a second while the throttle is pressed under 30 km/h, the throttle
// at each stage (the pedal, what the chassis took, what the engine used), the brakes, gear, clutch,
// engine and gearbox rpm: where a launch loses the start of the pedal.
void LogLaunch(uint8_t* eng) {
    static double last;
    InputSnapshot in = Input_Get();
    float speed = Physics_PlayerSpeed() * 3.6f;
    double now = Now();
    if (!g_cfg.logPhysics || in.pedal[0] < 0.02f || speed > 30.0f || now - last < 0.25) return;
    last = now;
    const Clutch& c = Field<Clutch>(eng, kER_Clutch);
    Log("launch: %4.1f km/h: pedal %.2f, chassis %.2f, engine %.2f; brake %.2f, handbrake %.2f; gear %d, clutch %d%s "
        "(%.2f s); engine %.0f rpm, gearbox %.0f rpm", speed, in.pedal[0], Physics_GasInput(),
        Field<float>(eng, kER_Throttle), in.pedal[1], in.handbrake, Field<int>(eng, kER_Gear) - 1, c.state,
        s.launching ? " launch" : "", c.time,
        Field<float>(eng, kER_AngularVelocity) * 9.5492966f, Field<float>(eng, kER_TransmissionVelocity) * 9.5492966f);
}

void __fastcall Hook_EngineSimulate(uint8_t* eng, void*, float dT) {
    s_brakeEngine = g_cfg.engineBrakeFade < 1.0f && eng == s.engine ? eng : nullptr;
    ClutchSimulate(eng, dT);
    if (eng != s.engine) return;
    LogLaunch(eng);
    PlayerEngineStep(eng);
    if (WearApplies(eng)) UpdateWear(eng, dT);
}

// The overlay is drawn after the game's HUD, so the VR mod puts it on its HUD panel.
void __cdecl Hook_RenderHud(int arg) {
    Hud_Frame();
    ((RenderHud_t)kRenderHud)(arg);
    Ui_Frame();
}

// Trampoline that runs the `stolen` original bytes at `at` and continues after them.
// The stolen bytes must be position independent.
void* MakeTrampoline(uintptr_t at, size_t stolen) {
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, (void*)at, stolen);
    tramp[stolen] = 0xE9;
    int32_t rel = (int32_t)((at + stolen) - ((uintptr_t)tramp + stolen + 5));
    memcpy(tramp + stolen + 1, &rel, 4);
    return tramp;
}

}  // namespace

EngineStatus Game_Engine() {
    EngineStatus st;
    if (!s.engine || Now() - s.simSeen > 0.3) return st;
    st.driving = true;
    st.wear = g_cfg.engineWear && !s.autoShift && !IsDragster(s.engine);
    st.blown = s.blown;
    st.heat = s.heat;
    return st;
}

HudStatus Game_Hud() {
    HudStatus h;
    if (!s.engine || Now() - s.simSeen > 0.3) return h;
    h.valid = true;
    h.damage = Clamp(PlayerDamage(s.engine), 0.0f, 1.0f);
    if ((int)(h.damage * 10.0f + 0.001f) < s.damageStep) {
        Log("damage: repaired to %.0f%%", h.damage * 100.0f);
        s.damageStep = (int)(h.damage * 10.0f + 0.001f);
    }
    h.heat = g_cfg.engineWear && !s.autoShift && !IsDragster(s.engine) ? s.heat : 0.0f;
    h.poppedTires = PoppedTires(s.engine);
    h.engineBlown = s.blown;
    return h;
}

void* Game_PlayerInterface(void* handle) {
    if (!s.engine || Now() - s.simSeen > 0.5) return nullptr;
    uint8_t* owner = Field<uint8_t*>(s.engine, kER_Owner);
    if (!owner || !handle) return owner;
    void* list = Field<void*>(owner, 4);
    return list ? ((IListFind_t)kIListFind)(list, handle) : nullptr;
}

void Game_SetMenuOpen(bool open) { s.menuOpen = open; }

void Game_PlayMoment(uint32_t moment) {
    if (moment) PlayMoment(moment);
}

void Game_PopTires() { s.popPending = true; }

bool Game_Install() {
    // Every address we check must lie inside the host image (also true for patched v1.3 exes
    // with extra sections, which only make SizeOfImage larger).
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (base != (uint8_t*)0x400000 || nt->OptionalHeader.SizeOfImage < 0x5C7000) {
        Log("host is not speed.exe (base %p, size 0x%X)", base, (unsigned)nt->OptionalHeader.SizeOfImage);
        return false;
    }
    bool ok = Expect(kCallDoShifting, {0xE8, 0x9B, 0x40, 0xFF, 0xFF})
           && Expect(kCallDoAutoReverse, {0xE8, 0x62, 0x41, 0xFF, 0xFF})
           && Expect(kIsAutomaticShift, {0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x38})
           && Expect(kEngineSimulate, {0x83, 0xEC, 0x3C, 0x55, 0x56, 0x8B, 0xF1})
           && Expect(kIListFind, {0x83, 0xEC, 0x08, 0x56, 0x8B, 0x71, 0x08})
           && Expect(kGetDiffAngVel, {0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0xF1})
           && Expect(kGetGearRatio, {0x56, 0x8B, 0xB1, 0x60, 0x01, 0x00, 0x00})
           && ExpectPtr(kVtblRacerTrans + 4, 0x6A0590)       // GetGear
           && ExpectPtr(kVtblRacerTrans + 12, 0x6AF440)      // Shift
           && ExpectPtr(kVtblDragsterTrans + 4, 0x6A0590)
           && ExpectPtr(kVtblDragsterTrans + 12, 0x6AF440);
    if (!ok) return false;
    bool wear = ExpectPtr(kSlotUseRevLimiter, kUseRevLimiter)
             && ExpectPtr(kSlotEngineTorque, kGetEngineTorque)
             && ExpectPtr(kSlotRepair, kRepair)
             && ExpectPtr(kSlotRepair - 12, kBlow)
             && Expect(kUseRevLimiter, {0xB0, 0x01, 0xC3})
             && Expect(kGetEngineTorque, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1})
             && Expect(kRepair, {0x33, 0xC0, 0x89, 0x81, 0x30, 0x01, 0x00, 0x00, 0x88, 0x81, 0x2C, 0x01, 0x00, 0x00})
             && Expect(kBlow + 0x19, {0x8A, 0x86, 0x2C, 0x01, 0x00, 0x00})
             && Expect(kDestroy + 0x19, {0xD9, 0x46, 0x3C})
             && Expect(kResetDamageRacer, {0x56, 0x8B, 0xF1, 0xE8});
    if (!wear) {
        Log("engine wear unavailable");
        g_cfg.engineWear = false;
    }

    float k = *(float*)kClutchStiffness;
    if (k > 0.0f && k < 1000.0f) s_baseStiffness = k;

    s_origIsAutomaticShift = (IsAutomaticShift_t)MakeTrampoline(kIsAutomaticShift, 6);
    s_origEngineSimulate = (EngineSimulate_t)MakeTrampoline(kEngineSimulate, 5);
    if (!s_origIsAutomaticShift || !s_origEngineSimulate) {
        Log("out of memory for trampolines");
        return false;
    }
    WriteRel(kCallDoShifting, 0xE8, (void*)&Hook_DoShifting);
    WriteRel(kCallDoAutoReverse, 0xE8, (void*)&Hook_DoAutoReverse);
    WriteRel(kIsAutomaticShift, 0xE9, (void*)&Hook_IsAutomaticShift);
    const uint8_t nop = 0x90;
    Write(kIsAutomaticShift + 5, &nop, 1);
    WriteRel(kEngineSimulate, 0xE9, (void*)&Hook_EngineSimulate);
    if (Expect(kEngineBrakeWeight, {0xD8, 0xA6, 0x8C, 0x00, 0x00, 0x00})) {
        WriteCallNops(kEngineBrakeWeight, 6, (void*)&EngineBrakeStub);
        Log("engine braking: gone at %.0f%% throttle", g_cfg.engineBrakeFade * 100.0f);
    } else {
        Log("engine braking: not at 0x%06X, left as the game's", (unsigned)kEngineBrakeWeight);
    }
    if (ExpectPtr(kSlotOnImpact, kOnImpact)) {
        void* p = (void*)&Hook_OnImpact;
        Write(kSlotOnImpact, &p, 4);
        Log("car damage %s (from %.1f m/s, strength %.2f)", g_cfg.damage ? "on" : "off", g_cfg.damageMinImpact,
            g_cfg.damageStrength);
    } else {
        Log("car damage unavailable");
        g_cfg.damage = false;
    }
    if (Expect(kCanDoGameBreaker, {0xA1, 0x04, 0x8E, 0x98, 0x00, 0x85, 0xC0})) {
        s_origCanDoGameBreaker = (Bool_t)MakeTrampoline(kCanDoGameBreaker, 5);
        if (s_origCanDoGameBreaker) {
            WriteRel(kCanDoGameBreaker, 0xE9, (void*)&Hook_CanDoGameBreaker);
            Log("speedbreaker %s", g_cfg.speedbreaker ? "on" : "off");
        }
    }
    Hud_Install();
    Physics_Install();
    Vfx_Install();
    Traffic_Install();
    Controls_Install();
    Flow_Install();
    // Only the call itself: the widescreen fix hooks the instruction right after it.
    if (Expect(kCallRenderHud, {0xE8, 0x94, 0xF8, 0xFF, 0xFF})) {
        WriteRel(kCallRenderHud, 0xE8, (void*)&Hook_RenderHud);
        Log("overlay hook installed (menu %s)", g_cfg.menu.c_str());
    } else {
        Log("overlay unavailable");
    }
    if (wear) {
        void* p = (void*)&Hook_UseRevLimiter;
        Write(kSlotUseRevLimiter, &p, 4);
        p = (void*)&Hook_GetEngineTorque;
        Write(kSlotEngineTorque, &p, 4);
        p = (void*)&Hook_Repair;
        Write(kSlotRepair, &p, 4);
        Log("engine wear %s (rev limiter %s, heat +%.2f..%.2f/s, -%.2f/s)", g_cfg.engineWear ? "on" : "off",
            g_cfg.revLimiter ? "on" : "off", g_cfg.heatUp, g_cfg.heatUpMax, g_cfg.coolDown);
    }
    Log("hooks installed (clutch stiffness %.1f)", s_baseStiffness);
    return true;
}
