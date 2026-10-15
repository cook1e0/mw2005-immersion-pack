// Engine smoke on the player's damaged or overheated car.
//
// CarRenderConn::UpdateEffects runs each car's engine effects: DeathEffect at health 0,
// DamageEffect (the smoke cop cars trail) at health <= 0.5, EngineBlownEffect for a blown engine.
// The player's cars have no DamageEffect, so the code that loads that key is patched: for the
// car the camera follows, once its damage reaches [Damage] SmokeFrom (health = 1 - damage) or
// its engine heat reaches [Engine] SmokeFrom, the key becomes the cop cars' DamageEffect (read
// from the copmidsize car data, or taken from the first car seen with one). The health <= 0.5
// test after it reads the health from s_smokeHealth instead, which is 0 while that car smokes,
// so a hot engine smokes on an undamaged car. The car's own EngineBlownEffect is only a fallback: it is the drag
// race smoke for a car that has stopped, and stays where it started instead of trailing the car.
//
// Particles take a fraction (the emitter data's VelocityInherit, 0.5 for the cop smoke) of the
// velocity UpdateEffects passes. For the player's car that vector falls well short of how fast
// the car really moves, so at speed the particles were left behind the chase camera and the
// smoke only showed once the car stopped.
// The player's smoke effect update is wrapped: its particles get [Damage] SmokeTrail of the
// car's render-space velocity (from its render matrix's movement) instead.
//
// When the smoke stops (repaired, respawned, the engine cooled down) UpdateEffects only switches
// the effect's emitter group off (EmitterGroup::Disable, 0x503C50) and no longer updates it: its
// particles stop ageing and hang where they were for good. That call is wrapped: a group that was
// the player's smoke goes on being updated (EmitterGroup::Update, 0x50AF90) until it is empty.
#include "vfx.h"
#include "common.h"
#include "game.h"

namespace {

constexpr uintptr_t kDamageKeyLoad = 0x75642E;      // UpdateEffects: mov ecx,[esp+0x18] (DamageEffect key); test ecx,ecx
constexpr uintptr_t kIsViewAnchor = 0x744C00;       // bool VehicleRenderConn::IsViewAnchor()
constexpr int kPkt_Health = 0x90;                   // Pkt_Car_Service: mHealth
constexpr uintptr_t kFindCollection = 0x455FD0;     // const Attrib::Collection* Attrib::FindCollection(class, key)
constexpr uintptr_t kCollectionGetData = 0x454190;  // void* Attrib::Collection::GetData(attrib, index)
constexpr uintptr_t kStringToKey = 0x454640;        // unsigned Attrib::StringToKey(const char*)
constexpr uint32_t kClassEcar = 0xA5B543B7;
constexpr uint32_t kAttrDamageEffect = 0x30B2997B;  // ecar DamageEffect: RefSpec, +4 = emittergroup key

typedef bool(__thiscall* IsViewAnchor_t)(void* conn);
typedef void*(__cdecl* FindCollection_t)(uint32_t cls, uint32_t key);
typedef void*(__thiscall* GetData_t)(void* collection, uint32_t attrib, uint32_t index);
typedef uint32_t(__cdecl* StringToKey_t)(const char*);

constexpr uintptr_t kSmokeUpdateCall = 0x75645D;    // UpdateEffects: call Effect::Update for the DamageEffect
constexpr uintptr_t kEffectUpdate = 0x744A50;       // VehicleRenderConn::Effect::Update(mtx, key, dt, intensity, vel)
constexpr uintptr_t kGroupNumParticles = 0x503A90;  // unsigned EmitterGroup::GetNumParticles()
constexpr int kEffect_Group = 0x50;                 // Effect: EmitterGroup*
constexpr int kEffect_Key = 0x54;                   // Effect: emittergroup key of the group
constexpr int kGroup_Emitters = 0x08;               // EmitterGroup: list of Emitters (the list node is the Emitter)
constexpr int kEmitter_Inherit = 0x60;              // Emitter: inherited velocity (bVector3)
constexpr int kEmitter_Data = 0x7C;                 // Emitter: EmitterDataAttribWrapper*, +8 = attribute layout
constexpr int kData_InheritLocal = 0xE0;            // layout: nonzero = no velocity inheritance
constexpr int kData_Inherit = 0xEC;                 // layout: VelocityInherit

typedef void(__thiscall* EffectUpdate_t)(void* eff, const void* mtx, uint32_t key, float dt, float intensity, const float* vel);

constexpr uintptr_t kSmokeDisableCall = 0x756491;   // UpdateEffects: call EmitterGroup::Disable (no effect for the slot)
constexpr uintptr_t kGroupDisable = 0x503C50;       // thiscall
constexpr uintptr_t kGroupUpdate = 0x50AF90;        // void EmitterGroup::Update(float dt), thiscall

// Emitter groups that have carried the player's smoke, and when each was last aged by us.
struct SmokeGroup {
    uint8_t* group;
    double aged;
};
SmokeGroup s_smokeGroups[4];

constexpr uintptr_t kSmokeHealthLoad = 0x756436;    // UpdateEffects: fld [edx+0x90] (health) before the <= 0.5 test

uint32_t s_copKey = 0;
bool s_playerSmoke;                                 // the DamageEffect update about to run is the player's smoke

// The cop cars' DamageEffect from the car data, so it is known before any cop shows up.
uint32_t CopSmokeKey() {
    static bool looked;
    if (s_copKey || looked) return s_copKey;
    looked = true;
    for (const char* car : {"copmidsize", "copsport", "copsuv", "copghost", "copheavy"}) {
        void* c = ((FindCollection_t)kFindCollection)(kClassEcar, ((StringToKey_t)kStringToKey)(car));
        uint32_t* ref = c ? (uint32_t*)((GetData_t)kCollectionGetData)(c, kAttrDamageEffect, 0) : nullptr;
        if (ref && ref[1]) {
            s_copKey = ref[1];
            Log("vfx: damage smoke effect %08X (from %s)", s_copKey, car);
            break;
        }
    }
    if (!s_copKey) Log("vfx: no cop damage smoke in the car data");
    return s_copKey;
}

float s_smokeHealth = 1.0f;                         // the health the DamageEffect test sees

}  // namespace

// Called from the patch with the render connection, the service packet and the car's own keys.
extern "C" uint32_t __cdecl Vfx_DamageKey(void* conn, uint8_t* pkt, uint32_t key, uint32_t blownKey) {
    bool player = ((IsViewAnchor_t)kIsViewAnchor)(conn);
    float health = *(float*)(pkt + kPkt_Health);
    s_smokeHealth = health;
    s_playerSmoke = false;
    if (!player) {
        if (key && key != s_copKey) {
            s_copKey = key;
            Log("vfx: damage smoke effect %08X (from car %p)", key, conn);
        }
        return key;
    }
    if (!g_cfg.damageSmoke) return key;
    bool hot = g_cfg.heatSmokeFrom < 1.0f && Game_Hud().heat >= g_cfg.heatSmokeFrom;
    if (health > 1.0f - g_cfg.smokeFrom && !hot) return 0;
    s_playerSmoke = true;
    s_smokeHealth = 0.0f;
    uint32_t cop = CopSmokeKey();
    uint32_t k = key ? key : cop ? cop : blownKey;
    static uint32_t logged;
    if (k != logged) {
        logged = k;
        Log("vfx: engine smoke on (health %.2f, effect %08X)", health, k);
    }
    return k;
}

// Replaces UpdateEffects' call of Effect::Update for the DamageEffect. Before the group spawns
// particles, each emitter's inherited velocity is set so that, after the game scales it by the
// emitter's VelocityInherit, particles leave with SmokeTrail of the car's velocity.
static void __fastcall SmokeUpdate(void* eff, void*, const float* mtx, uint32_t key, float dt, float intensity, const float* vel) {
    uint8_t* group = *(uint8_t**)((uint8_t*)eff + kEffect_Group);
    if (s_playerSmoke && vel && mtx && group && key == *(uint32_t*)((uint8_t*)eff + kEffect_Key)) {
        // The car's velocity in render space, from its render matrix's movement (the game's
        // vector is in simulation space, whose axes differ).
        static float lastPos[3];
        static double lastTime;
        static float v[3];
        double now = Now();
        float rdt = (float)(now - lastTime);
        if (rdt > 0.001f && rdt < 0.2f)
            for (int a = 0; a < 3; a++) v[a] = v[a] * 0.5f + (mtx[12 + a] - lastPos[a]) / rdt * 0.5f;
        else if (rdt >= 0.2f)
            v[0] = v[1] = v[2] = 0;
        if (rdt > 0.001f || rdt < 0) {
            memcpy(lastPos, mtx + 12, sizeof(lastPos));
            lastTime = now;
        }
        uint8_t* head = group + kGroup_Emitters;
        for (uint8_t* e = *(uint8_t**)head; e && e != head; e = *(uint8_t**)e) {
            uint8_t* wrapper = *(uint8_t**)(e + kEmitter_Data);
            uint8_t* data = wrapper ? *(uint8_t**)(wrapper + 8) : nullptr;
            float inherit = data ? *(float*)(data + kData_Inherit) : 0.0f;
            float k = inherit > 0.01f ? g_cfg.smokeTrail / inherit : 0.0f;
            float* iv = (float*)(e + kEmitter_Inherit);
            for (int a = 0; a < 3; a++) iv[a] = v[a] * k;
        }
        static double lastLog;
        if (now - lastLog > 2.0) {
            lastLog = now;
            Log("vfx: smoke car velocity %.1f %.1f %.1f (game's %.1f %.1f %.1f), %u particles", v[0], v[1], v[2], vel[0],
                vel[1], vel[2], ((unsigned(__thiscall*)(void*))kGroupNumParticles)(group));
        }
        vel = nullptr;  // keep these instead of the game's
    }
    bool player = s_playerSmoke;
    ((EffectUpdate_t)kEffectUpdate)(eff, mtx, key, dt, intensity, vel);
    uint8_t* now = *(uint8_t**)((uint8_t*)eff + kEffect_Group);
    if (player && now) {
        bool known = false;
        for (SmokeGroup& g : s_smokeGroups) known = known || g.group == now;
        for (SmokeGroup& g : s_smokeGroups)
            if (!known && !g.group) {
                g = {now, Now()};
                known = true;
            }
    }
}

// Replaces UpdateEffects' EmitterGroup::Disable for an effect slot with nothing to show.
static void __fastcall SmokeDisable(uint8_t* group, void*) {
    ((void(__thiscall*)(void*))kGroupDisable)(group);
    for (SmokeGroup& g : s_smokeGroups) {
        if (g.group != group) continue;
        double now = Now();
        float dt = (float)(now - g.aged);
        g.aged = now;
        if (dt <= 0.0005f) return;               // twice in a frame (both eyes in VR)
        if (dt > 0.1f) dt = 0.1f;
        ((void(__thiscall*)(void*, float))kGroupUpdate)(group, dt);
        if (((unsigned(__thiscall*)(void*))kGroupNumParticles)(group) == 0) g.group = nullptr;
        return;
    }
}

// Replaces "mov ecx,[esp+0x18]; test ecx,ecx" (6 bytes) with a call; the flags are set on return.
// [esp+0x1c] at the patch holds the EngineBlownEffect key (read there by the blown engine branch).
__asm__(
    ".text\n"
    "_Vfx_DamageKeyStub:\n"
    "    pushal\n"
    "    pushl 0x40(%esp)\n"            // EngineBlownEffect key: 0x20 past our return address, + pushal
    "    pushl 0x40(%esp)\n"            // DamageEffect key: 0x1c past it, + pushal + the push above
    "    pushl %edx\n"                  // Pkt_Car_Service*
    "    pushl %esi\n"                  // CarRenderConn*
    "    call _Vfx_DamageKey\n"
    "    addl $16, %esp\n"
    "    movl %eax, 0x18(%esp)\n"       // pushal's ecx slot
    "    popal\n"
    "    testl %ecx, %ecx\n"
    "    ret\n");
extern "C" void Vfx_DamageKeyStub();

void Vfx_Install() {
    static const uint8_t expect[] = {0x8B, 0x4C, 0x24, 0x18, 0x85, 0xC9, 0x74, 0x2E};
    if (memcmp((void*)kDamageKeyLoad, expect, sizeof(expect)) != 0) {
        Log("vfx: unexpected code, no engine smoke");
        return;
    }
    uint8_t code[6] = {0xE8, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)&Vfx_DamageKeyStub - (kDamageKeyLoad + 5));
    memcpy(code + 1, &rel, 4);
    DWORD old;
    VirtualProtect((void*)kDamageKeyLoad, sizeof(code), PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)kDamageKeyLoad, code, sizeof(code));
    VirtualProtect((void*)kDamageKeyLoad, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kDamageKeyLoad, sizeof(code));
    int32_t cur = *(int32_t*)(kSmokeUpdateCall + 1);
    if (*(uint8_t*)kSmokeUpdateCall == 0xE8 && kSmokeUpdateCall + 5 + cur == kEffectUpdate) {
        int32_t r = (int32_t)((uintptr_t)&SmokeUpdate - (kSmokeUpdateCall + 5));
        VirtualProtect((void*)kSmokeUpdateCall, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)(kSmokeUpdateCall + 1), &r, 4);
        VirtualProtect((void*)kSmokeUpdateCall, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kSmokeUpdateCall, 5);
    }
    if (*(uint8_t*)kSmokeDisableCall == 0xE8 && kSmokeDisableCall + 5 + *(int32_t*)(kSmokeDisableCall + 1) == kGroupDisable) {
        int32_t r = (int32_t)((uintptr_t)&SmokeDisable - (kSmokeDisableCall + 5));
        VirtualProtect((void*)kSmokeDisableCall, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)(kSmokeDisableCall + 1), &r, 4);
        VirtualProtect((void*)kSmokeDisableCall, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kSmokeDisableCall, 5);
    } else {
        Log("vfx: unexpected code, stopped smoke may linger");
    }
    static const uint8_t healthLoad[] = {0xD9, 0x82, 0x90, 0x00, 0x00, 0x00};   // fld [edx+0x90]
    if (memcmp((void*)kSmokeHealthLoad, healthLoad, sizeof(healthLoad)) == 0) {
        uint8_t fld[6] = {0xD9, 0x05};                                          // fld [s_smokeHealth]
        float* p = &s_smokeHealth;
        memcpy(fld + 2, &p, 4);
        VirtualProtect((void*)kSmokeHealthLoad, sizeof(fld), PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)kSmokeHealthLoad, fld, sizeof(fld));
        VirtualProtect((void*)kSmokeHealthLoad, sizeof(fld), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kSmokeHealthLoad, sizeof(fld));
    } else {
        Log("vfx: unexpected code, no heat smoke on an undamaged car");
    }
    Log("vfx: engine smoke from %.0f%% damage or %.0f%% heat, trail %.2f (%s)", g_cfg.smokeFrom * 100,
        g_cfg.heatSmokeFrom * 100, g_cfg.smokeTrail, g_cfg.damageSmoke ? "on" : "off");
}
