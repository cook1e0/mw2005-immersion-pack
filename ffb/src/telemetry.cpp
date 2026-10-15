// Samples the player car for force feedback: LocalPlayer::OnTask calls LocalPlayer::DoFFB every
// physics tick; that call is retargeted to Hook_DoFFB, which samples the car through the game's
// ISuspension and IEngine interfaces and then calls the original. The player's collision reports
// (LocalPlayer::OnCollision, a slot in its vtables) are hooked too, for crashes and wall scrapes.
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
const uintptr_t QI_IEngine     = 0x40E4A0;
const uintptr_t VT_SuspensionRacer_ISuspension = 0x8ABAC8;
const uintptr_t OnCollision    = 0x6F2370;  // void LocalPlayer::OnCollision(const Sim::Collision::Info&)
const uintptr_t OnCollisionSlots[] = {0x8B0B08, 0x8B0BCC};  // LocalPlayer's and OnlineLocalPlayer's listener vtables
const uintptr_t FindSimable    = 0x41AD40;  // ISimable* ISimable::FindInstance(HSIMABLE), cdecl
// Breakable props: a Smackable (Sim object of a bench, sign, post, fence...) is created from the
// world's placed scenery when something hits it, by sub_6895A0 at CallSmackableCtor. Its
// vtables (primary and interfaces, as Smackable::Smackable installs them) identify it in a
// collision report.
const uintptr_t CallSmackableCtor = 0x6897DB;
const uintptr_t SmackableCtor  = 0x688660;  // Smackable::Smackable(const Matrix4&, ...), thiscall, 7 args
const uintptr_t SmackableVtables[] = {0x8AA6D0, 0x8AA638, 0x8AA618, 0x8AA5FC, 0x8AA5F4, 0x8AA5D8, 0x8AA5D4, 0x8AA5CC, 0x8AA5C4};
}

// ISuspension vtable slots (byte offsets)
enum {
    kGetNumWheels = 0x08, kGetWheelPos = 0x0C, kGetWheelLoad = 0x18, kIsWheelOnGround = 0x24,
    kGetCompression = 0x28, kGetWheelVelocity = 0x48, kGetNumWheelsOnGround = 0x4C, kGetWheelSteer = 0x58,
    kGetMaxSteering = 0x6C,
};
// IEngine vtable slots
enum { kGetRPM = 0x04, kGetRedline = 0x08, kGetMinRPM = 0x10 };
// SuspensionRacer: its front Tire objects; the Tire's road surface, an Attrib::Instance of class
// simsurface (layout pointer at +8) with the game's own per-surface rumble.
enum { kRacerTires = 0x168, kTireSurface = 0x9C, kInstLayout = 0x08, kSurfEffectFrequency = 0xF8, kSurfEffectIntensity = 0xFA };
// Sim::Collision::Info
enum {
    kCiPosition = 0x00, kCiNormal = 0x10, kCiFlags = 0x1C, kCiClosingVel = 0x20, kCiObjA = 0x3C, kCiObjB = 0x4C,
    kCiImpulseA = 0x5C, kCiImpulseB = 0x6C, kCiSlidingVel = 0x70,
};
enum { kCollisionObject = 1, kCollisionWorld = 2 };

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
typedef void (THISCALL *OnCollision_t)(void*, const uint8_t*);

// A pointer into the game's heap: not null or one of the small sentinel values.
bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x7FFF0000; }

CRITICAL_SECTION s_lock;
Telemetry s_cur;
uint32_t s_seq;

// Game thread state: the previous sample, and the car as the collision hook sees it.
struct {
    bool have;
    double time;
    Vec3 fwd, front;
    float comp[2], compVel[2], compVelSlow[2];
} s_prev;
struct {
    void* simable;          // the player car's ISimable, from the latest sample
    Vec3 centre, fwd, right;
    float halfBase, speed;
    uint32_t breakSeq;
    float breakSpeed, breakLat;
    double breakTime;
    void* breakObject;
    uint32_t hitSeq;
    float hitSpeed, hitImpulse, hitLat;
    double hitTime, scrapeTime, scrapeLogTime;
    float scrapeSpeed, scrapeLat;
} s_car;

void Publish(const Telemetry& t) {
    EnterCriticalSection(&s_lock);
    s_cur = t;
    LeaveCriticalSection(&s_lock);
}

Vec3 ReadVec(const uint8_t* p) { Vec3 v; memcpy(&v, p, sizeof(v)); return v; }

bool IsSmackable(const void* simable) {
    if (!simable) return false;
    uintptr_t vt = *(const uintptr_t*)simable;
    for (uintptr_t v : addr::SmackableVtables)
        if (vt == v) return true;
    return false;
}

// One knock per prop: the reports keep coming while the car pushes it along.
void Break(const void* object, Vec3 at, const char* how) {
    double now = Now();
    if (now - s_car.breakTime < 0.25 || (object && object == s_car.breakObject && now - s_car.breakTime < 1.0)) return;
    s_car.breakSeq++;
    s_car.breakTime = now;
    s_car.breakObject = (void*)object;
    s_car.breakSpeed = s_car.speed;
    Vec3 rel = at - s_car.centre;
    float side = Dot(rel, s_car.right);
    s_car.breakLat = Clamp(side / 1.0f, -1.0f, 1.0f);
    if (g_cfg.logTelemetry) Log("contact: breakable prop (%s) at %.1f m/s, side %.2f", how, s_car.speed, s_car.breakLat);
}

// The game reports a contact of the player car with another car, a prop (benches, sign posts,
// fences) or the world. Hits become jolts; contact sliding along something becomes a scrape.
void OnPlayerCollision(const uint8_t* info) {
    int type = *(const int*)(info + kCiFlags) & 7;
    if ((type != kCollisionObject && type != kCollisionWorld) || !s_car.simable) return;
    uint32_t flags = *(const uint32_t*)(info + kCiFlags);
    void* a = ((void* (__cdecl*)(void*))addr::FindSimable)(*(void* const*)(info + kCiObjA));
    void* b = type == kCollisionObject ? ((void* (__cdecl*)(void*))addr::FindSimable)(*(void* const*)(info + kCiObjB)) : nullptr;
    bool isA = a == s_car.simable;
    if (!isA && b != s_car.simable) return;
    // A breakable prop: its own knock, not a crash.
    void* other = isA ? b : a;
    if (type == kCollisionObject && IsSmackable(other)) {
        Break(other, ReadVec(info + kCiPosition), "contact");
        return;
    }

    // The normal pushes object A away from B.
    Vec3 n = Norm(ReadVec(info + kCiNormal)) * (isA ? 1.0f : -1.0f);
    Vec3 closing = ReadVec(info + kCiClosingVel);
    float closingSpeed = fabsf(Dot(closing, n));
    float impulse = fabsf(*(const float*)(info + (isA ? kCiImpulseA : kCiImpulseB)));
    Vec3 sliding = ReadVec(info + kCiSlidingVel);
    float slideSpeed = Len(sliding - n * Dot(sliding, n));
    float lat = Dot(n, s_car.right);
    // Contacts at the nose turn the steering most, at the tail least.
    float along = s_car.halfBase > 0.1f ? Dot(ReadVec(info + kCiPosition) - s_car.centre, s_car.fwd) / s_car.halfBase : 0.0f;
    float noseShare = Clamp(0.6f + 0.4f * along, 0.25f, 1.0f);

    double now = Now();
    bool slidingFlag = (flags >> 7 & 1) != 0;
    if ((slidingFlag || slideSpeed > 2.0f) && closingSpeed < 4.0f) {
        s_car.scrapeTime = now;
        s_car.scrapeSpeed = slideSpeed;
        s_car.scrapeLat = lat;
        if (g_cfg.logTelemetry && now - s_car.scrapeLogTime > 1.0) {
            s_car.scrapeLogTime = now;
            Log("contact: scrape %.1f m/s, side %.2f (%s)", slideSpeed, lat, type == kCollisionWorld ? "world" : "object");
        }
    }
    if (closingSpeed > 1.5f && (now - s_car.hitTime > 0.15 || closingSpeed > s_car.hitSpeed * 1.5f)) {
        s_car.hitSeq++;
        s_car.hitTime = now;
        s_car.hitSpeed = closingSpeed;
        s_car.hitImpulse = impulse;
        s_car.hitLat = Clamp(lat * noseShare, -1.0f, 1.0f);
        if (g_cfg.logTelemetry)
            Log("contact: hit %.1f m/s, impulse %.1f, side %.2f, nose %.2f (%s)", closingSpeed, impulse, lat, noseShare,
                type == kCollisionWorld ? "world" : "object");
    }
}

void* THISCALL Hook_SmackableCtor(void* self, const float* m, void* a2, void* a3, int a4, void* a5, int a6, int a7) {
    typedef void* (THISCALL *Ctor_t)(void*, const float*, void*, void*, int, void*, int, int);
    void* r = ((Ctor_t)addr::SmackableCtor)(self, m, a2, a3, a4, a5, a6, a7);
    // Created where the car is, while it moves: the car just knocked it loose.
    if (m && s_car.simable && s_car.speed > 2.0f) {
        Vec3 at = {m[12], m[13], m[14]};
        if (Len(at - s_car.centre) < 4.0f) Break(nullptr, at, "knocked loose");
    }
    return r;
}

void THISCALL Hook_OnCollision(void* self, const uint8_t* info) {
    if (info) OnPlayerCollision(info);
    ((OnCollision_t)addr::OnCollision)(self, info);
}

// Wheels 0 and 1 are the front axle, 2 and 3 the rear (the game's order).
void Sample(void* player) {
    Telemetry t;
    memset(&t, 0, sizeof(t));
    t.seq = ++s_seq;
    t.time = Now();
    t.hitSeq = s_car.hitSeq;
    t.hitSpeed = s_car.hitSpeed;
    t.hitImpulse = s_car.hitImpulse;
    t.hitLat = s_car.hitLat;
    t.breakSeq = s_car.breakSeq;
    t.breakSpeed = s_car.breakSpeed;
    t.breakLat = s_car.breakLat;
    t.scrapeTime = s_car.scrapeTime;
    t.scrapeSpeed = s_car.scrapeSpeed;
    t.scrapeLat = s_car.scrapeLat;

    // CanDoFFB also requires the in-game rumble option; force it on just for the check.
    int* rumble = (int*)addr::bRumbleEnabled;
    int saved = *rumble;
    *rumble = 1;
    t.driving = ((CanDoFFB_t)addr::CanDoFFB)(player);
    *rumble = saved;

    void* simable = VCall0<void*>((char*)player + 0x2C, 0x0C);   // player->mSimable.Get()
    void* susp = nullptr;
    void* engine = nullptr;
    if (simable) {
        ((QI_t)addr::QI_ISuspension)(simable, &susp);
        ((QI_t)addr::QI_IEngine)(simable, &engine);
    }
    s_car.simable = simable;
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
    bool onGround[2];
    for (unsigned i = 0; i < 2; ++i) {
        onGround[i] = VCallI<bool>(susp, kIsWheelOnGround, i);
        if (onGround[i]) {
            t.frontOnGround++;
            t.frontLoad += VCallI<float>(susp, kGetWheelLoad, i);
        }
    }
    t.wheelsOnGround = VCall0<int>(susp, kGetNumWheelsOnGround);
    float maxSteer = VCall0<float>(susp, kGetMaxSteering) * 360.0f;   // revolutions -> degrees
    t.maxSteerDeg = maxSteer > 0.5f && maxSteer < 90.0f ? maxSteer : 0.0f;
    float steer = 0.5f * (VCallI<float>(susp, kGetWheelSteer, 0) + VCallI<float>(susp, kGetWheelSteer, 1)) * 360.0f;
    t.steerDeg = steer > -90.0f && steer < 90.0f ? steer : 0.0f;

    // The car's frame: forward from the rear axle to the front one, right = forward x world up.
    Vec3 front = (pos[0] + pos[1]) * 0.5f, rear = (pos[2] + pos[3]) * 0.5f;
    Vec3 fwd = Norm(front - rear);
    Vec3 right = Norm(Cross(fwd, Vec3{0, 0, 1}));
    Vec3 vFront = (vel[0] + vel[1]) * 0.5f, vRear = (vel[2] + vel[3]) * 0.5f;
    Vec3 v = (vFront + vRear) * 0.5f;
    t.speed = Len(v);
    s_car.speed = t.speed;
    t.vLong = Dot(v, fwd);
    t.vLatFront = Dot(vFront, right);
    t.vLatRear = Dot(vRear, right);
    t.wheelbase = Len(front - rear);
    t.valid = Len(fwd) > 0.5f && Len(right) > 0.5f;
    s_car.centre = (front + rear) * 0.5f;
    s_car.fwd = fwd;
    s_car.right = right;
    s_car.halfBase = t.wheelbase * 0.5f;
    int rightWheel = Dot(pos[1] - pos[0], right) >= 0.0f ? 1 : 0;   // which front wheel is on the right

    if (engine) {
        float rpm = VCall0<float>(engine, kGetRPM);
        float red = VCall0<float>(engine, kGetRedline);
        float idle = VCall0<float>(engine, kGetMinRPM);
        if (rpm >= 0.0f && rpm < 30000.0f) {
            t.rpm = rpm;
            t.rpmNorm = red > idle + 1.0f ? Clamp((rpm - idle) / (red - idle), 0.0f, 1.0f) : 0.0f;
        }
    }

    // The road surface lives on the concrete SuspensionRacer's tyres: only read it for that class.
    if (*(uintptr_t*)susp == addr::VT_SuspensionRacer_ISuspension) {
        for (int i = 0; i < 2; ++i) {
            uintptr_t tire = *(uintptr_t*)((char*)susp + kRacerTires + i * 4);
            if (!onGround[i] || !Plausible(tire)) continue;
            uintptr_t layout = *(uintptr_t*)(tire + kTireSurface + kInstLayout);
            if (!Plausible(layout)) continue;
            float rumbleNow = *(uint8_t*)(layout + kSurfEffectIntensity) / 255.0f;
            if (rumbleNow > t.surfaceRumble) {
                t.surfaceRumble = rumbleNow;
                t.surfaceHz = *(uint16_t*)(layout + kSurfEffectFrequency);
            }
        }
    }

    double dt = s_prev.have ? t.time - s_prev.time : 0.0;
    bool continuous = dt > 0.0005 && dt < 0.1 && Len(front - s_prev.front) < 10.0f;
    float comp[2] = {VCallI<float>(susp, kGetCompression, 0), VCallI<float>(susp, kGetCompression, 1)};
    for (int i = 0; i < 2; ++i) {
        if (!(comp[i] > -10.0f && comp[i] < 10.0f)) comp[i] = s_prev.comp[i];
        float cv = continuous ? Clamp((comp[i] - s_prev.comp[i]) / (float)dt, -20.0f, 20.0f) : 0.0f;
        // The slow part (roll, pitch, hills) out: what stays is the quick hit of a kerb or a bump.
        float k = continuous ? Clamp((float)dt / 0.15f, 0.0f, 1.0f) : 1.0f;
        s_prev.compVelSlow[i] += (cv - s_prev.compVelSlow[i]) * k;
        s_prev.compVel[i] = cv;
        int side = i == rightWheel ? 1 : 0;
        t.bump[side] = onGround[i] && continuous ? cv - s_prev.compVelSlow[i] : 0.0f;
        s_prev.comp[i] = comp[i];
    }

    if (continuous)
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

void PatchSlot(uintptr_t slot, const void* target) {
    DWORD old;
    VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old);
    *(const void**)slot = target;
    VirtualProtect((void*)slot, 4, old, &old);
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

    int hooked = 0;
    for (uintptr_t slot : addr::OnCollisionSlots) {
        if (*(uintptr_t*)slot != addr::OnCollision) continue;
        PatchSlot(slot, (const void*)&Hook_OnCollision);
        hooked++;
    }
    if (hooked) Log("telemetry: hooked the player's collision reports (%d)", hooked);
    else Log("telemetry: unexpected collision listener - crash and scrape effects off");

    uint8_t* c = (uint8_t*)addr::CallSmackableCtor;
    if (c[0] == 0xE8 && (uintptr_t)(c + 5 + *(int32_t*)(c + 1)) == addr::SmackableCtor) {
        VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(c + 1) = (int32_t)((uintptr_t)&Hook_SmackableCtor - (uintptr_t)(c + 5));
        VirtualProtect(c, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), c, 5);
        Log("telemetry: hooked breakable props being knocked loose");
    } else {
        Log("telemetry: unexpected breakable prop code - knocked-loose detection off");
    }
    return true;
}
