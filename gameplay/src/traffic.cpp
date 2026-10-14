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

// Drag races: the race gives every car the drag driver style (STYLE_DRAG, PVehicle::SetDriverStyle),
// which swaps the car's parts for drag ones: InputPlayerDrag (steering becomes lane changes, the
// game steers), EngineDragster (its own heat and shift model), DamageDragster (totaled by a hit).
// None of that works with a wheel, the shifter and the gameplay mod's engine and damage, so with
// [Races] NormalDragRaces the player's car keeps the normal style: the game's normal input,
// engine and damage, and you steer. Opponents still race as drag cars, and the race itself (the
// drag HUD, staging, the finish) goes by the race type and is unchanged.
constexpr uintptr_t kSlotSetDriverStyle = 0x8AA854;   // PVehicle's IVehicle vtable: SetDriverStyle
constexpr uintptr_t kSetDriverStyle = 0x687690;       // thiscall(IVehicle*, DriverStyle)
constexpr int kIV_DriverClass = 0x94;                 // PVehicle+0x140 from its IVehicle: 0 = the player
constexpr int kStyleDrag = 1, kStyleRacing = 0;

static void __attribute__((thiscall)) HkSetDriverStyle(uint8_t* vehicle, int style) {
    if (style == kStyleDrag && g_cfg.normalDragRaces && *(int*)(vehicle + kIV_DriverClass) == 0) {
        Log("traffic: drag race - the player's car keeps the normal driving style");
        style = kStyleRacing;
    }
    ((void(__attribute__((thiscall))*)(uint8_t*, int))kSetDriverStyle)(vehicle, style);
}

void Traffic_Install() {
    if (*(uintptr_t*)kSlotSetDriverStyle == kSetDriverStyle) {
        DWORD old;
        VirtualProtect((void*)kSlotSetDriverStyle, 4, PAGE_READWRITE, &old);
        *(void**)kSlotSetDriverStyle = (void*)&HkSetDriverStyle;
        VirtualProtect((void*)kSlotSetDriverStyle, 4, old, &old);
        Log("traffic: drag races %s", g_cfg.normalDragRaces ? "with normal driving for the player" : "as the game's");
    } else {
        Log("traffic: unexpected vehicle code, NormalDragRaces unavailable");
    }
    Cops_Install();
}
