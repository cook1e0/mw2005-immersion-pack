// Driving controls ([Controls]): steering, pedals and buttons read by this mod's input thread and
// handed to the game as its own input actions, so the game's controller settings aren't needed.
//
// The game's input: IOModule devices (one GameDevice for the controller, mouse, keyboard) are read
// by ActionQueue::IO_UpdateFromDevice, which turns changes into ActionData {id, port, value} and
// queues them with ActionQueue::ReceiveAction. Each IInputPlayer owns an ActionQueue (+0x10) and
// drains it in InputPlayer::FetchInput (SimSystem::DoFetchInput calls it for every human player):
// steer left/right, gas and brake actions set its control values, buttons set flags or fire on
// every action.
//
// Injection: before each FetchInput, changed values are queued into the player's ActionQueue
// with ReceiveAction, on the queue's own port. Values above zero are re-sent now and then, so a
// held pedal survives the game clearing its input (pause, F4 menu, a new car).
// [Controls] GameController=0 can't be done here: on PC the one GameDevice merges keyboard and
// joystick per action. The force feedback mod's dinput8.dll proxy hides the controllers' input
// from the game instead (NfsmwFfb_Setting("GameController")), menus included.
#include "controls.h"
#include "common.h"
#include "input.h"
#include "physics.h"
#include <math.h>

// Steering type for SuspensionRacer::DoHumanSteering: -1 = the game's (gamepad on PC, as no
// ISteeringWheel device exists), else ISteeringWheel::SteeringType (1 speed-sensitive wheel,
// 2 speed-insensitive wheel). Set on the game thread before FetchInput.
extern "C" { volatile int g_steerType = -1; }
extern "C" { void* g_linearMaxSteering = nullptr; }

namespace {

// DoHumanSteering picks the steering type in ebx (0 = gamepad without a wheel device) and passes
// it to CalculateMaxSteering. As a gamepad the game remaps the input, scales the lock from 40
// degrees parked down to ~3 at speed and limits how fast the front wheels turn; as a wheel the
// input maps straight to the lock. The call is retargeted to a stub that swaps the type in.
constexpr uintptr_t kSteerTypePush = 0x69E89D;      // push ebx; push ebp; mov ecx, esi
constexpr uintptr_t kCallMaxSteering = 0x69E8A1;    // call SuspensionRacer::CalculateMaxSteering
constexpr uintptr_t kCalculateMaxSteering = 0x68E9E0;

// Speed-sensitive wheel steering, rewritten for a wheel. The game's version was made for pads:
//  - its lock grew up to 1.4 times as the wheel turned further (by the recent average input), and
//    up to ~1.7 times off the throttle or on the brakes: progressive, and lifting tightened a turn
//  - its speed table kinks (45 degrees parked, 15 at 64 km/h, 11, 8, then 7 flat)
//  - while countersteering it widened the lock to the rear tyres' slip angle, and dropped it back
//    the moment the wheel crossed over: the front wheels jumped as a slide was caught
// Here the lock is one smooth curve of speed (SteeringLockShare) times the car's steering tuning,
// so the front wheels always follow the wheel in proportion. In a slide the lock widens with the
// rear tyres' slip angle on both sides alike, so it changes only as the slide does: enough lock
// to catch it, and no jump when the wheel crosses centre.
constexpr uintptr_t kRadToDeg = 0x890960;            // the game's ANGLE2DEG factor
constexpr float kAbsoluteMaxSteering = 45.0f;
constexpr int kState_LocalVelZ = 0x48;
constexpr int kRacer_Vehicle = 0x48, kRacer_RearTires = 0x1BC, kTire_SlipAngle = 0x118;
constexpr int kVehicle_GetTunings = 0xCC;            // vtable slot: const Physics::Tunings*

constexpr uintptr_t kDoFetchInput = 0x6E8AC0;       // void SimSystem::DoFetchInput(IInputPlayer*)
constexpr uintptr_t kReceiveAction = 0x6440B0;      // bool ActionQueue::ReceiveAction(const ActionData&)
constexpr int kIP_Queue = 0x10;                     // IInputPlayer: ActionQueue
constexpr int kAQ_Port = 0x268;                     // ActionQueue: device index (-1 = none)

// ActionID (the game's GAMEACTION_* / CAMERAACTION_* names table)
enum {
    A_STEERLEFT = 1, A_STEERRIGHT = 2, A_GAS = 5, A_BRAKE = 6, A_HANDBRAKE = 7, A_GAMEBREAKER = 8,
    A_NOS = 11, A_RESET = 12, A_CAMERA = 20, A_LOOKBACK = 23,
};

struct ActionData {
    int id;
    int port;
    float value;
};

typedef bool(__thiscall* ReceiveAction_t)(void* queue, const ActionData* a);

// Held controls: value actions. Buttons that act on every action they get (the speedbreaker
// toggle, reset, camera) are only sent on a press.
struct Analog {
    int id;
    bool bound = false;
    float sent = 0.0f;
    double time = 0;
};

struct {
    void* player = nullptr;         // IInputPlayer we last fed
    Analog analog[7] = {{A_STEERLEFT}, {A_STEERRIGHT}, {A_GAS}, {A_BRAKE}, {A_HANDBRAKE}, {A_NOS}, {A_LOOKBACK}};
    uint32_t prevDown = 0;
    bool logged = false;
} s;

}  // namespace

static float __attribute__((thiscall)) LinearMaxSteering(uint8_t* racer, const uint8_t* state, int) {
    float kmh = fabsf(*(const float*)(state + kState_LocalVelZ)) * 3.6f;
    float lock = kAbsoluteMaxSteering * SteeringLockShare(kmh, g_cfg.steerSpeedSensitivity);
    void* vehicle = *(void**)(racer + kRacer_Vehicle);
    if (vehicle) {
        auto get = (const float* (__attribute__((thiscall))*)(void*))(*(uintptr_t**)vehicle)[kVehicle_GetTunings / 4];
        const float* tunings = get(vehicle);
        if (tunings) lock *= *tunings * 0.2f + 1.0f;
    }
    const float k = *(const float*)kRadToDeg;
    uintptr_t rearLeft = *(uintptr_t*)(racer + kRacer_RearTires), rearRight = *(uintptr_t*)(racer + kRacer_RearTires + 4);
    if (rearLeft && rearRight) {
        float slip = 0.5f * (fabsf(*(const float*)(rearLeft + kTire_SlipAngle)) + fabsf(*(const float*)(rearRight + kTire_SlipAngle))) * k;
        if (slip > 0.0f && slip < 90.0f) lock = std::max(lock, slip);
    }
    return std::min(std::max(lock, 0.0f), kAbsoluteMaxSteering);
}

// The type also stays in ebx for DoHumanSteering's own gamepad branch (remap and ramp), which is
// tested right after the call; ebx is restored by its epilogue. A speed-sensitive wheel (1) goes
// to LinearMaxSteering (same arguments, thiscall).
__asm__(
    ".text\n"
    "_Controls_SteerTypeStub:\n"
    "    movl _g_steerType, %eax\n"
    "    testl %eax, %eax\n"
    "    js 1f\n"
    "    movl %eax, %ebx\n"
    "    movl %eax, 8(%esp)\n"          // CalculateMaxSteering's steer_type argument
    "    cmpl $1, %eax\n"
    "    jne 1f\n"
    "    movl _g_linearMaxSteering, %eax\n"
    "    testl %eax, %eax\n"
    "    jz 1f\n"
    "    jmp *%eax\n"
    "1:  movl $0x68E9E0, %eax\n"
    "    jmp *%eax\n");
extern "C" void Controls_SteerTypeStub();

namespace {

void Send(void* queue, int port, int id, float value) {
    ActionData a = {id, port, value};
    ((ReceiveAction_t)kReceiveAction)(queue, &a);
}

// Sim pedals: InputPlayer::FetchInput clamps the gas and brake to a deadzone at each end (0x8F876C,
// 0.05 in the exe and used only there, four times): fine for a gamepad's triggers, but it cut the
// first and last 5% of a pedal's travel, so a light brake did nothing and 95% was already full.
// With pedals bound here it is set to 0, and this mod's own [Controls] PedalDeadzone applies. The
// game may set its own value at run time, so any small value is accepted (and logged).
constexpr uintptr_t kPedalDeadzone = 0x8F876C;

void Inject(void* ip) {
    InputSnapshot in = Input_Get();
    static bool deadzoneDone;
    if (!deadzoneDone && (in.pedalBound[0] || in.pedalBound[1])) {
        deadzoneDone = true;
        float was = *(float*)kPedalDeadzone;
        if (was > 0.0f && was <= 0.3f) {
            *(float*)kPedalDeadzone = 0.0f;   // .data: writable
            Log("controls: the game's pedal deadzone (%.3f) removed: the whole pedal travel counts", was);
        } else {
            Log("controls: the game's pedal deadzone reads %.3f, left as it is", was);
        }
    }
    uint8_t* queue = (uint8_t*)ip + kIP_Queue;
    int port = *(int*)(queue + kAQ_Port);
    if (port < 0) return;
    double now = Now();
    bool fresh = ip != s.player;
    s.player = ip;

    float steer = in.steerBound ? in.steer : 0.0f;
    int type = in.steerBound ? (g_cfg.steerSpeedSensitive ? 1 : 2) : -1;
    if (type != g_steerType) {
        g_steerType = type;
        Log("controls: steering %s", type < 0 ? "as the game's (gamepad)" : type == 1 ? "as a speed-sensitive wheel" : "direct (wheel)");
    }
    struct { bool bound; float value; } want[7] = {
        {in.steerBound, std::max(-steer, 0.0f)},
        {in.steerBound, std::max(steer, 0.0f)},
        {in.pedalBound[0], in.pedal[0] * Physics_TcThrottle()},   // traction control trims it
        {in.pedalBound[1], in.pedal[1]},
        // The game passes the handbrake action's value on as it is (InputPlayer::FetchInput,
        // 0x6A8BE5: SetControlHandBrake(value)), so an axis binding gives an analogue handbrake.
        {(in.buttonBound >> BTN_HANDBRAKE & 1) != 0, Clamp(in.handbrake, 0.0f, 1.0f)},
        {(in.buttonBound >> BTN_NITROUS & 1) != 0, (in.buttonDown >> BTN_NITROUS & 1) ? 1.0f : 0.0f},
        {(in.buttonBound >> BTN_LOOKBACK & 1) != 0, (in.buttonDown >> BTN_LOOKBACK & 1) ? 1.0f : 0.0f},
    };
    for (int i = 0; i < 7; i++) {
        Analog& a = s.analog[i];
        if (!want[i].bound) {
            // Unbound: let go once, then leave the action to the game's own controls.
            if (a.bound && a.sent != 0.0f) Send(queue, port, a.id, 0.0f);
            a.bound = false;
            a.sent = 0.0f;
            continue;
        }
        float v = want[i].value;
        bool send = fresh || !a.bound || fabsf(v - a.sent) > 0.004f || (v != 0.0f && now - a.time > 0.25);
        a.bound = true;
        if (!send) continue;
        Send(queue, port, a.id, v);
        a.sent = v;
        a.time = now;
    }

    uint32_t pressed = in.buttonDown & in.buttonBound & ~s.prevDown;
    s.prevDown = in.buttonDown & in.buttonBound;
    if (fresh) pressed = 0;
    if (pressed >> BTN_SPEEDBREAKER & 1) Send(queue, port, A_GAMEBREAKER, 1.0f);
    if (pressed >> BTN_RESET & 1) Send(queue, port, A_RESET, 1.0f);
    if (pressed >> BTN_CAMERA & 1) Send(queue, port, A_CAMERA, 1.0f);

    if (!s.logged && (in.steerBound || in.pedalBound[0] || in.pedalBound[1])) {
        s.logged = true;
        Log("controls: feeding player input (port %d, steering %s, lock %.0f of %.0f deg)", port,
            in.steerBound ? "bound" : "unbound", g_cfg.steeringLock, g_cfg.wheelRotation);
    }
}

// [Controls] ClassicInput: the game's own steering, throttle and brake. Nothing is injected and the
// steering type is the game's; what this mod last held down (a pedal, the wheel) is let go once.
// The game's controller input is switched on for it too (ui.cpp hands GameController to the FFB mod).
void ReleaseAll(void* ip) {
    uint8_t* queue = (uint8_t*)ip + kIP_Queue;
    int port = *(int*)(queue + kAQ_Port);
    for (Analog& a : s.analog) {
        if (a.bound && a.sent != 0.0f && port >= 0) Send(queue, port, a.id, 0.0f);
        a.bound = false;
        a.sent = 0.0f;
    }
    s.prevDown = 0;
    s.player = nullptr;
}

void __cdecl Hook_DoFetchInput(void* ip) {
    if (!ip) return;
    static bool classic = false;
    if (g_cfg.classicInput != classic) {
        classic = g_cfg.classicInput;
        Log("controls: %s", classic ? "classic input: the game's own steering, throttle and brake" : "the mod's driving controls again");
        if (classic) ReleaseAll(ip);
    }
    if (classic) {
        g_steerType = -1;
        ((void(__thiscall*)(void*))(*(void***)ip)[4])(ip);   // IInputPlayer::FetchInput
        return;
    }
    Inject(ip);
    ((void(__thiscall*)(void*))(*(void***)ip)[4])(ip);   // IInputPlayer::FetchInput
}

bool Expect(uintptr_t at, std::initializer_list<uint8_t> bytes) {
    const uint8_t* p = (const uint8_t*)at;
    size_t i = 0;
    for (uint8_t b : bytes)
        if (p[i++] != b) return false;
    return true;
}

void WriteRel(uintptr_t at, uint8_t op, const void* target) {
    uint8_t code[5] = {op};
    int32_t rel = (int32_t)((uintptr_t)target - (at + 5));
    memcpy(code + 1, &rel, 4);
    DWORD old;
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, code, 5);
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

}  // namespace

void Controls_Install() {
    if (!Expect(kDoFetchInput, {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x01, 0xFF, 0x60, 0x10})) {
        Log("controls: unexpected code, [Controls] unavailable");
        return;
    }
    WriteRel(kDoFetchInput, 0xE9, (void*)&Hook_DoFetchInput);
    if (Expect(kSteerTypePush, {0x53, 0x55, 0x8B, 0xCE, 0xE8}) &&
        kCallMaxSteering + 5 + *(const int32_t*)(kCallMaxSteering + 1) == kCalculateMaxSteering)
    {
        g_linearMaxSteering = (void*)&LinearMaxSteering;
        WriteRel(kCallMaxSteering, 0xE8, (void*)&Controls_SteerTypeStub);
    }
    else
        Log("controls: unexpected steering code, wheel steering type unavailable");
    Log("controls: installed");
}
