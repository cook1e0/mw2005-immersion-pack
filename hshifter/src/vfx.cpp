// Engine smoke on the player's damaged car.
//
// CarRenderConn::UpdateEffects runs each car's engine effects: DeathEffect at health 0,
// DamageEffect (the smoke cop cars trail) at health <= 0.5, EngineBlownEffect for a blown engine.
// The player's cars have no DamageEffect, so the code that loads that key is patched: for the
// car the camera follows, once its damage reaches [Damage] SmokeFrom (health = 1 - damage), the
// key becomes the cop cars' DamageEffect (read from the copmidsize car data, or taken from the
// first car seen with one). The car's own EngineBlownEffect is only a fallback: it is the drag
// race smoke for a car that has stopped, and stays where it started instead of trailing the car.
#include "vfx.h"
#include "common.h"

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

uint32_t s_copKey = 0;

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

}  // namespace

// Called from the patch with the render connection, the service packet and the car's own keys.
extern "C" uint32_t __cdecl Vfx_DamageKey(void* conn, uint8_t* pkt, uint32_t key, uint32_t blownKey) {
    bool player = ((IsViewAnchor_t)kIsViewAnchor)(conn);
    if (!player) {
        if (key && key != s_copKey) {
            s_copKey = key;
            Log("vfx: damage smoke effect %08X (from car %p)", key, conn);
        }
        return key;
    }
    if (!g_cfg.damageSmoke) return key;
    float health = *(float*)(pkt + kPkt_Health);
    if (health > 1.0f - g_cfg.smokeFrom) return 0;
    uint32_t cop = CopSmokeKey();
    uint32_t k = key ? key : cop ? cop : blownKey;
    static uint32_t logged;
    if (k != logged) {
        logged = k;
        Log("vfx: engine smoke on (health %.2f, effect %08X)", health, k);
    }
    return k;
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
    Log("vfx: engine smoke from %.0f%% damage (%s)", g_cfg.smokeFrom * 100, g_cfg.damageSmoke ? "on" : "off");
}
