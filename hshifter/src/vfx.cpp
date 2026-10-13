// Engine smoke on the player's damaged car.
//
// CarRenderConn::UpdateEffects runs each car's engine effects: DeathEffect at health 0,
// DamageEffect (the smoke cop cars trail) at health <= 0.5, EngineBlownEffect for a blown engine.
// The player's cars have no DamageEffect, so the code that loads that key is patched: for the
// car the camera follows, once its damage reaches [Damage] SmokeFrom (health = 1 - damage), the
// key becomes the car's own EngineBlownEffect (the drag race blown engine smoke). Cars without
// one get the cop cars' DamageEffect, taken from the first car seen with one.
#include "vfx.h"
#include "common.h"

namespace {

constexpr uintptr_t kDamageKeyLoad = 0x75642E;      // UpdateEffects: mov ecx,[esp+0x18] (DamageEffect key); test ecx,ecx
constexpr uintptr_t kIsViewAnchor = 0x744C00;       // bool VehicleRenderConn::IsViewAnchor()
constexpr int kPkt_Health = 0x90;                   // Pkt_Car_Service: mHealth

typedef bool(__thiscall* IsViewAnchor_t)(void* conn);

uint32_t s_copKey = 0;

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
    uint32_t k = key ? key : blownKey ? blownKey : s_copKey;
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
