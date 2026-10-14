// Hooks LocalPlayer::DoFFB (called every sim tick from LocalPlayer::OnTask) and samples the
// player vehicle through the game's own COM-style interfaces (ISuspension, IEngine).
//
// Addresses are for speed.exe v1.3 (6,029,312 bytes); see tools/dis.py for how they were found.
#include "telemetry.h"
#include <math.h>
#include <string.h>
#include <algorithm>

#define THISCALL __attribute__((thiscall))

namespace addr {
const uintptr_t CallDoFFB      = 0x6F22C6;  // call ?DoFFB@LocalPlayer@@IAEXXZ inside LocalPlayer::OnTask
const uintptr_t DoFFB          = 0x6EE490;
const uintptr_t CanDoFFB       = 0x6EE400;  // bool LocalPlayer::CanDoFFB() const
const uintptr_t bRumbleEnabled = 0x8F86E0;  // int, checked by CanDoFFB (in-game "rumble" option)
const uintptr_t QI_ISuspension = 0x40E4E0;  // bool IUnknown::QueryInterface<ISuspension>(ISuspension**)
const uintptr_t QI_IEngine     = 0x40E4A0;
const uintptr_t VT_SuspensionRacer_ISuspension = 0x8ABAC8;
}

// ISuspension vtable slots (byte offsets)
enum {
    kGetNumWheels = 0x08, kGetWheelPos = 0x0C, kGetWheelLoad = 0x18, kIsWheelOnGround = 0x24,
    kGetCompression = 0x28, kGetWheelSlipAngle = 0x3C, kGetWheelVelocity = 0x48,
    kGetNumWheelsOnGround = 0x4C, kGetWheelSteer = 0x58, kGetMaxSteering = 0x6C,
};
// IEngine vtable slots
enum { kGetRPM = 0x04, kGetRedline = 0x08, kGetMinRPM = 0x10 };

struct Vec3 { float x, y, z; };
static inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float Len(Vec3 a) { return sqrtf(Dot(a, a)); }
static inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline Vec3 Norm(Vec3 a) { float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 0}; }

template <class R> static inline R VCall0(void* obj, int off) {
    typedef R (THISCALL *Fn)(void*);
    return (*(Fn*)(*(uintptr_t*)obj + off))(obj);
}
template <class R> static inline R VCallI(void* obj, int off, unsigned i) {
    typedef R (THISCALL *Fn)(void*, unsigned);
    return (*(Fn*)(*(uintptr_t*)obj + off))(obj, i);
}

typedef void (THISCALL *DoFFB_t)(void*);
typedef bool (THISCALL *CanDoFFB_t)(void*);
typedef bool (THISCALL *QI_t)(void*, void**);

static CRITICAL_SECTION s_lock;
static Telemetry s_cur;          // published sample
static uint32_t s_seq;

// SuspensionRacer::Tire (derived from Wheel): road surface and air time
enum { kTireAirTime = 0x68, kTireSurface = 0x9C };
// The surface is an Attrib::Instance (layout pointer at +8) of class simsurface
enum { kInstLayout = 0x08, kSurfRollingResistance = 0xE4, kSurfEffectFrequency = 0xF8, kSurfEffectIntensity = 0xFA };

// Game-thread history
static struct {
    bool  have;
    double time;
    Vec3  vel, fwd, pos;
    float comp[2], compVel[2];
    float vertVel, vertAccel;
    Vec3  accelSmooth;
    uint32_t hitSeq;
    float hitMag, hitLat, hitLon;
    uint32_t scrapeSeq;
    float scrapeLat, scrapeMag;
    double airSince;
    bool  airborne;
    uint32_t landSeq;
    float landSpeed;
} s_prev;

// A pointer the game keeps inside its own heap: readable, not one of the small sentinel values.
static bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x7FFF0000; }

static void Publish(const Telemetry& t) {
    EnterCriticalSection(&s_lock);
    s_cur = t;
    LeaveCriticalSection(&s_lock);
}

void Telemetry_Get(Telemetry* out) {
    EnterCriticalSection(&s_lock);
    *out = s_cur;
    LeaveCriticalSection(&s_lock);
}

static void Sample(void* player) {
    Telemetry t;
    memset(&t, 0, sizeof(t));
    t.seq = ++s_seq;
    t.time = Now();
    t.hitSeq = s_prev.hitSeq;
    t.hitMag = s_prev.hitMag;
    t.hitLat = s_prev.hitLat;
    t.hitLon = s_prev.hitLon;
    t.scrapeSeq = s_prev.scrapeSeq;
    t.scrapeLat = s_prev.scrapeLat;
    t.scrapeMag = s_prev.scrapeMag;
    t.landSeq = s_prev.landSeq;
    t.landSpeed = s_prev.landSpeed;

    // CanDoFFB() covers pause, AI-controlled player car, cutscenes - but also the in-game
    // rumble option, which we don't want to depend on.
    int* rumble = (int*)addr::bRumbleEnabled;
    int saved = *rumble;
    *rumble = 1;
    t.driving = ((CanDoFFB_t)addr::CanDoFFB)(player);
    *rumble = saved;

    // ISimable* = player->mSimable.Get()  (handle at +0x2c, vtable slot 3)
    void* handle = (char*)player + 0x2C;
    void* simable = VCall0<void*>(handle, 0x0C);
    void* susp = nullptr;
    void* engine = nullptr;
    if (simable) {
        ((QI_t)addr::QI_ISuspension)(simable, &susp);
        ((QI_t)addr::QI_IEngine)(simable, &engine);
    }
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
        t.slipAngle[i] = VCallI<float>(susp, kGetWheelSlipAngle, i);
        t.load[i] = VCallI<float>(susp, kGetWheelLoad, i);
        t.onGround[i] = VCallI<bool>(susp, kIsWheelOnGround, i);
    }
    t.rearSlipAngle = 0.5f * (VCallI<float>(susp, kGetWheelSlipAngle, 2) + VCallI<float>(susp, kGetWheelSlipAngle, 3));
    float comp[2] = {VCallI<float>(susp, kGetCompression, 0), VCallI<float>(susp, kGetCompression, 1)};
    // GetWheelSteer returns revolutions; convert back to radians.
    t.steer = 0.5f * (VCallI<float>(susp, kGetWheelSteer, 0) + VCallI<float>(susp, kGetWheelSteer, 1)) * 6.2831853f;
    t.maxSteer = VCall0<float>(susp, kGetMaxSteering) * 360.0f;
    t.wheelsOnGround = VCall0<int>(susp, kGetNumWheelsOnGround);

    // Brake lock and the road surface live on the concrete SuspensionRacer::Tire, only read them
    // for that class.
    if (*(uintptr_t*)susp == addr::VT_SuspensionRacer_ISuspension) {
        for (int i = 0; i < 2; ++i) {
            uintptr_t tire = *(uintptr_t*)((char*)susp + 0x168 + i * 4);
            if (!Plausible(tire)) continue;
            t.brakeLocked[i] = *(uint8_t*)(tire + 0x134) != 0;
            uintptr_t layout = *(uintptr_t*)(tire + kTireSurface + kInstLayout);
            if (!t.onGround[i] || !Plausible(layout)) continue;
            t.rollingResist[i] = *(float*)(layout + kSurfRollingResistance);
            t.surfaceHz[i] = *(uint16_t*)(layout + kSurfEffectFrequency);
            t.surfaceRumble[i] = *(uint8_t*)(layout + kSurfEffectIntensity) / 255.0f;
            if (!(t.rollingResist[i] >= 0.0f && t.rollingResist[i] < 100.0f)) t.rollingResist[i] = 0.0f;
        }
    }

    if (engine) {
        float rpm = VCall0<float>(engine, kGetRPM);
        float red = VCall0<float>(engine, kGetRedline);
        float idle = VCall0<float>(engine, kGetMinRPM);
        t.rpm = rpm;
        t.rpmNorm = red > idle + 1.0f ? Clamp((rpm - idle) / (red - idle), 0.0f, 1.0f) : 0.0f;
    }

    // Body frame from the wheel contact points: forward = rear->front axle, right guess = wheel0->wheel1.
    Vec3 front = (pos[0] + pos[1]) * 0.5f, rear = (pos[2] + pos[3]) * 0.5f;
    Vec3 fwd = Norm(front - rear);
    Vec3 side = pos[1] - pos[0];
    Vec3 right = Norm(side - fwd * Dot(side, fwd));
    Vec3 v = (vel[0] + vel[1] + vel[2] + vel[3]) * 0.25f;
    t.fwdSpeed = Dot(v, fwd);
    t.speed = Len(v);
    t.wheelbase = Len(front - rear);
    // The car's up axis (world Z is up; the guessed right may point either way).
    Vec3 up = Norm(Cross(fwd, right));
    if (up.z < 0) up = up * -1.0f;
    t.vertVel = Dot(v, up);
    t.valid = true;

    // Airborne from the moment no wheel touches; a landing is the first contact after 0.15 s.
    double now = t.time;
    bool air = t.wheelsOnGround == 0;
    if (air && !s_prev.airborne) s_prev.airSince = now;
    if (!air && s_prev.airborne && now - s_prev.airSince > 0.15 && s_prev.have) {
        s_prev.landSeq++;
        s_prev.landSpeed = std::max(0.0f, -s_prev.vertVel);
        t.landSeq = s_prev.landSeq;
        t.landSpeed = s_prev.landSpeed;
    }
    s_prev.airborne = air;
    t.airborne = air && now - s_prev.airSince > 0.1;
    t.airTime = air ? (float)(now - s_prev.airSince) : 0.0f;

    double dt = s_prev.have ? t.time - s_prev.time : 0.0;
    bool continuous = s_prev.have && dt > 0.0005 && dt < 0.1 && Len(front - s_prev.pos) < 10.0f;
    if (continuous) {
        float fdt = (float)dt;
        t.dt = fdt;
        t.yawRate = Dot(fwd - s_prev.fwd, right) / fdt;
        for (int i = 0; i < 2; ++i) {
            t.compVel[i] = (comp[i] - s_prev.comp[i]) / fdt;
            t.compAccel[i] = (t.compVel[i] - s_prev.compVel[i]) / fdt;
        }
        // Vertical acceleration in the car's frame, lightly smoothed (the sim ticks unevenly), and
        // its jerk: driving over a crest changes it slowly, a kerb strike within a tick or two.
        float accelNow = (t.vertVel - s_prev.vertVel) / fdt;
        float k = std::min(1.0f, fdt / 0.03f);
        t.vertAccel = s_prev.vertAccel + (accelNow - s_prev.vertAccel) * k;
        t.vertJerk = (t.vertAccel - s_prev.vertAccel) / fdt;

        // Impacts: velocity changes far beyond what tyres/brakes can produce (~1.5 g).
        Vec3 dv = v - s_prev.vel;
        float dvLen = Len(dv);
        float accel = dvLen / fdt;
        Vec3 a = dv * (1.0f / fdt);
        if (accel > 40.0f && dvLen > 0.6f) {
            s_prev.hitSeq++;
            s_prev.hitMag = dvLen;
            s_prev.hitLat = Dot(dv, right) / dvLen;
            s_prev.hitLon = Dot(dv, fwd) / dvLen;
            t.hitSeq = s_prev.hitSeq;
            t.hitMag = s_prev.hitMag;
            t.hitLat = s_prev.hitLat;
            t.hitLon = s_prev.hitLon;
        } else {
            // Cornering and braking change the acceleration smoothly; a scrape or tap spikes it.
            Vec3 spike = a - s_prev.accelSmooth;
            float lat = Dot(spike, right), mag = Len(spike);
            if (mag > 15.0f && fabsf(lat) > 0.5f * mag && t.wheelsOnGround >= 2) {
                s_prev.scrapeSeq++;
                s_prev.scrapeLat = lat / mag;
                s_prev.scrapeMag = mag;
                t.scrapeSeq = s_prev.scrapeSeq;
                t.scrapeLat = s_prev.scrapeLat;
                t.scrapeMag = s_prev.scrapeMag;
            }
        }
        s_prev.accelSmooth = s_prev.accelSmooth + (a - s_prev.accelSmooth) * std::min(1.0f, fdt / 0.15f);
        s_prev.vertAccel = t.vertAccel;
        for (int i = 0; i < 2; ++i) s_prev.compVel[i] = t.compVel[i];
    } else {
        s_prev.vertAccel = 0.0f;
        s_prev.accelSmooth = {0, 0, 0};
        for (int i = 0; i < 2; ++i) s_prev.compVel[i] = 0.0f;
    }
    s_prev.have = true;
    s_prev.time = t.time;
    s_prev.vel = v;
    s_prev.fwd = fwd;
    s_prev.pos = front;
    s_prev.comp[0] = comp[0];
    s_prev.comp[1] = comp[1];
    s_prev.vertVel = t.vertVel;

    Publish(t);
}

static void __fastcall Hook_DoFFB(void* player, void* /*edx*/) {
    Sample(player);
    ((DoFFB_t)addr::DoFFB)(player);
}

bool Telemetry_Install() {
    InitializeCriticalSection(&s_lock);
    uint8_t* p = (uint8_t*)addr::CallDoFFB;
    int32_t rel = *(int32_t*)(p + 1);
    if (p[0] != 0xE8 || (uintptr_t)(p + 5 + rel) != addr::DoFFB) {
        Log("telemetry: unexpected code at %08X (not speed.exe v1.3?) - car telemetry disabled", (unsigned)addr::CallDoFFB);
        return false;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)&Hook_DoFFB - (uintptr_t)(p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("telemetry: hooked LocalPlayer::DoFFB call at %08X", (unsigned)addr::CallDoFFB);
    return true;
}
