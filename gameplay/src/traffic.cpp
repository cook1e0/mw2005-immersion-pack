// No SUV cops ([Traffic] NoSuvCops): every cop car comes from
// AICopManager::GetAvailableCopVehicleByName (reuses an idle cop of that name or creates one),
// so its calls are redirected. Heavy support asks for copsuv / copsuvl by name and gets nothing
// (the game drops the request); other spawns asking for an SUV (copsuvpatrol from the heat
// level's cop list) re-roll GetRandomValidCopCar, else copmidsize.
#include "traffic.h"
#include "common.h"

namespace {

constexpr uintptr_t kCopByName = 0x41ECD0;          // IVehicle* AICopManager::GetAvailableCopVehicleByName(const char*)
constexpr uintptr_t kRandomCopCar = 0x419AE0;       // const char* GetRandomValidCopCar()
constexpr uintptr_t kHeavySupportCall = 0x41F1CD;   // GetHeavySupportVehicles: copsuv / copsuvl
constexpr uintptr_t kCopByNameCalls[] = {kHeavySupportCall, 0x41F588, 0x4266E9, 0x426726,
                                         0x42E729, 0x431495, 0x43EBCB};

typedef void*(__thiscall* CopByName_t)(void* mgr, const char* name);
typedef const char*(__cdecl* RandomCopCar_t)();

bool IsSuv(const char* name) { return name && _strnicmp(name, "copsuv", 6) == 0; }

void* CopByName(void* mgr, const char* name, bool heavy) {
    if (g_cfg.noSuvCops && IsSuv(name)) {
        if (heavy) return nullptr;
        const char* alt = nullptr;
        for (int i = 0; i < 8 && (!alt || IsSuv(alt)); i++) alt = ((RandomCopCar_t)kRandomCopCar)();
        name = alt && !IsSuv(alt) ? alt : "copmidsize";
    }
    return ((CopByName_t)kCopByName)(mgr, name);
}

void* __fastcall CopByNameHeavy(void* mgr, void*, const char* name) { return CopByName(mgr, name, true); }
void* __fastcall CopByNameOther(void* mgr, void*, const char* name) { return CopByName(mgr, name, false); }

void PatchCall(uintptr_t at, void* fn) {
    DWORD old;
    int32_t rel = (int32_t)((uintptr_t)fn - (at + 5));
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)(at + 1), &rel, 4);
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

void Cops_Install() {
    for (auto at : kCopByNameCalls)
        if (*(uint8_t*)at != 0xE8 || at + 5 + *(int32_t*)(at + 1) != kCopByName) {
            Log("traffic: unexpected cop spawn code at %08X, SUV cops stay", (unsigned)at);
            return;
        }
    for (auto at : kCopByNameCalls)
        PatchCall(at, at == kHeavySupportCall ? (void*)&CopByNameHeavy : (void*)&CopByNameOther);
    Log("traffic: SUV cops %s", g_cfg.noSuvCops ? "off" : "on");
}

}  // namespace

void Traffic_Install() {
    Cops_Install();
}
