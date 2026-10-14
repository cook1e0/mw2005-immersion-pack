// Samples the player car for force feedback: LocalPlayer::OnTask calls LocalPlayer::DoFFB every
// physics tick; that call is retargeted to Hook_DoFFB, which samples the car through the game's
// ISuspension interface and then calls the original.
//
// speed.exe v1.3 addresses and vtable slots (from the dbalatoni13/nfsmw symbol map):
#include "telemetry.h"
#include <math.h>
#include <string.h>

#define THISCALL __attribute__((thiscall))

namespace addr {
const uintptr_t CallDoFFB      = 0x6F22C6;  // call LocalPlayer::DoFFB inside LocalPlayer::OnTask
const uintptr_t DoFFB          = 0x6EE490;
const uintptr_t CanDoFFB       = 0x6EE400;  // bool LocalPlayer::CanDoFFB() const: pause, AI, cutscenes
const uintptr_t bRumbleEnabled = 0x8F86E0;  // int, also checked by CanDoFFB (the in-game rumble option)
const uintptr_t QI_ISuspension = 0x40E4E0;  // bool IUnknown::QueryInterface<ISuspension>(ISuspension**)
}

// ISuspension vtable slots (byte offsets)
enum {
    kGetNumWheels = 0x08, kGetWheelPos = 0x0C, kGetWheelLoad = 0x18, kIsWheelOnGround = 0x24,
    kGetWheelVelocity = 0x48, kGetNumWheelsOnGround = 0x4C,
};

namespace {

struct Vec3 { float x, y, z; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(Vec3 a) { return sqrtf(Dot(a, a)); }
Vec3 Norm(Vec3 a) { float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 0}; }

template <class R> R VCall0(void* obj, int off) {
    typedef R (THISCALL *Fn)(void*);
    return (*(Fn*)(*(uintptr_t*)obj + off))(obj);
}
template <class R> R VCallI(void* obj, int off, unsigned i) {
    typedef R (THISCALL *Fn)(void*, unsigned);
    return (*(Fn*)(*(uintptr_t*)obj + off))(obj, i);
}
typedef void (THISCALL *DoFFB_t)(void*);
typedef bool (THISCALL *CanDoFFB_t)(void*);
typedef bool (THISCALL *QI_t)(void*, void**);

CRITICAL_SECTION s_lock;
Telemetry s_cur;
uint32_t s_seq;
struct { bool have; double time; Vec3 fwd, front; } s_prev;

void Publish(const Telemetry& t) {
    EnterCriticalSection(&s_lock);
    s_cur = t;
    LeaveCriticalSection(&s_lock);
}

// Wheels 0 and 1 are the front axle, 2 and 3 the rear (the game's order).
void Sample(void* player) {
    Telemetry t;
    memset(&t, 0, sizeof(t));
    t.seq = ++s_seq;
    t.time = Now();

    // CanDoFFB also requires the in-game rumble option; force it on just for the check.
    int* rumble = (int*)addr::bRumbleEnabled;
    int saved = *rumble;
    *rumble = 1;
    t.driving = ((CanDoFFB_t)addr::CanDoFFB)(player);
    *rumble = saved;

    void* simable = VCall0<void*>((char*)player + 0x2C, 0x0C);   // player->mSimable.Get()
    void* susp = nullptr;
    if (simable) ((QI_t)addr::QI_ISuspension)(simable, &susp);
    if (!susp || VCall0<unsigned>(susp, kGetNumWheels) != 4) {
        s_prev.have = false;
        Publish(t);
        return;
    }

    Vec3 pos[4], vel[4];
    for (unsigned i = 0; i < 4; ++i) {
        pos[i] = *VCallI<const Vec3*>(susp, kGetWheelPos, i);
        vel[i] = *VCallI<const Vec3*>(susp, kGetWheelVelocity, i);
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (VCallI<bool>(susp, kIsWheelOnGround, i)) {
            t.frontOnGround++;
            t.frontLoad += VCallI<float>(susp, kGetWheelLoad, i);
        }
    }
    t.wheelsOnGround = VCall0<int>(susp, kGetNumWheelsOnGround);

    // The car's frame: forward from the rear axle to the front one, right = forward x world up.
    Vec3 front = (pos[0] + pos[1]) * 0.5f, rear = (pos[2] + pos[3]) * 0.5f;
    Vec3 fwd = Norm(front - rear);
    Vec3 right = Norm(Cross(fwd, Vec3{0, 0, 1}));
    Vec3 vFront = (vel[0] + vel[1]) * 0.5f, vRear = (vel[2] + vel[3]) * 0.5f;
    Vec3 v = (vFront + vRear) * 0.5f;
    t.speed = Len(v);
    t.vLong = Dot(v, fwd);
    t.vLatFront = Dot(vFront, right);
    t.vLatRear = Dot(vRear, right);
    t.wheelbase = Len(front - rear);
    t.valid = Len(fwd) > 0.5f && Len(right) > 0.5f;

    double dt = s_prev.have ? t.time - s_prev.time : 0.0;
    if (dt > 0.0005 && dt < 0.1 && Len(front - s_prev.front) < 10.0f)
        t.yawRate = Dot(fwd - s_prev.fwd, right) / (float)dt;
    s_prev.have = true;
    s_prev.time = t.time;
    s_prev.fwd = fwd;
    s_prev.front = front;
    Publish(t);
}

void __fastcall Hook_DoFFB(void* player, void*) {
    Sample(player);
    ((DoFFB_t)addr::DoFFB)(player);
}

}  // namespace

void Telemetry_Get(Telemetry* out) {
    EnterCriticalSection(&s_lock);
    *out = s_cur;
    LeaveCriticalSection(&s_lock);
}

bool Telemetry_Install() {
    InitializeCriticalSection(&s_lock);
    uint8_t* p = (uint8_t*)addr::CallDoFFB;
    if (p[0] != 0xE8 || (uintptr_t)(p + 5 + *(int32_t*)(p + 1)) != addr::DoFFB) {
        Log("telemetry: unexpected code at %08X (not speed.exe v1.3?) - car telemetry disabled", (unsigned)addr::CallDoFFB);
        return false;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)&Hook_DoFFB - (uintptr_t)(p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("telemetry: hooked LocalPlayer::DoFFB at %08X", (unsigned)addr::CallDoFFB);
    return true;
}
