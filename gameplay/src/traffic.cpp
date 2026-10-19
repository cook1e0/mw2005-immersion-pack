// No heavy support ([Traffic] NoHeavySupport): heavy support is the game's ramming SUVs (copsuv /
// copsuvl, "Rhinos") that a pursuit sends at you head-on out of oncoming traffic. They are asked for
// by name in GetHeavySupportVehicles, through AICopManager::GetAvailableCopVehicleByName (reuses an
// idle cop of that name or creates one); that one call is redirected and gets nothing, so the
// game drops the request. Every other cop spawn is left alone: SUV patrols and SUVs in roadblocks
// still come.
#include "traffic.h"
#include "common.h"

namespace {

constexpr uintptr_t kCopByName = 0x41ECD0;          // IVehicle* AICopManager::GetAvailableCopVehicleByName(const char*)
constexpr uintptr_t kHeavySupportCall = 0x41F1CD;   // GetHeavySupportVehicles: copsuv / copsuvl

typedef void*(__thiscall* CopByName_t)(void* mgr, const char* name);

void* __fastcall HeavySupportByName(void* mgr, void*, const char* name) {
    if (g_cfg.noHeavySupport) {
        static int logged;
        if (logged < 5) {
            logged++;
            Log("traffic: heavy support (%s, head-on ramming SUV) not sent", name ? name : "?");
        }
        return nullptr;
    }
    return ((CopByName_t)kCopByName)(mgr, name);
}

void PatchCall(uintptr_t at, void* fn) {
    DWORD old;
    int32_t rel = (int32_t)((uintptr_t)fn - (at + 5));
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)(at + 1), &rel, 4);
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

void Cops_Install() {
    if (*(uint8_t*)kHeavySupportCall != 0xE8 || kHeavySupportCall + 5 + *(int32_t*)(kHeavySupportCall + 1) != kCopByName) {
        Log("traffic: unexpected heavy support code, heavy support stays");
        return;
    }
    PatchCall(kHeavySupportCall, (void*)&HeavySupportByName);
    Log("traffic: heavy support (head-on ramming SUVs) %s; SUV patrols and roadblocks as the game's",
        g_cfg.noHeavySupport ? "off" : "on");
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

// The drag HUD: the local player's SetHud(type) (0x6E99C0, thiscall, ret 4; slot 9 of both
// LocalPlayer vtables) builds the FEngHud for a HUD type, and the type picks everything: the package
// (0x5694E0: 2 = HUD_Drag.fng, else HUD_SingleRace.fng for 1), the elements the FEngHud constructor
// (0x5A6600) creates, and the texture pack it loads (0x5A6540: 2 = HUDTEXDRAG.BIN, 1 = HUDTEXRACE).
// With [Races] NormalDragHud a drag race's type 2 becomes 1, the normal race HUD (tachometer,
// speedometer, minimap, damage meter), as fits a drag race the player drives normally.
constexpr uintptr_t kSetHud = 0x6E99C0;
constexpr uintptr_t kSlotsSetHud[] = {0x8B0B34, 0x8B0BF4};
constexpr int kHudStandard = 1, kHudDrag = 2;

static void __fastcall HkSetHud(void* player, void* edx, int type) {
    if (type == kHudDrag && g_cfg.normalDragHud) {
        Log("traffic: drag race - the normal race HUD instead of the drag HUD");
        type = kHudStandard;
    }
    ((void(__fastcall*)(void*, void*, int))kSetHud)(player, edx, type);
}

// The turbo dial (its update at 0x57BB40) is shared by the race and drag HUDs and picks its needle's
// angle range by the race type (GRaceParameters::GetRaceType, 0x5FAA20; 2 = drag) at 0x57BB62. With
// the normal HUD in a drag race the drag range put the needle in the wrong place on the race HUD's
// dial: there it is told the race isn't a drag.
constexpr uintptr_t kGetRaceType = 0x5FAA20;          // int GRaceParameters::GetRaceType(), thiscall
constexpr uintptr_t kCallTurboRaceType = 0x57BB62;
constexpr int kRaceDrag = 2;

static int __fastcall HkTurboRaceType(void* parms, void* edx) {
    int type = ((int(__fastcall*)(void*, void*))kGetRaceType)(parms, edx);
    return type == kRaceDrag && g_cfg.normalDragHud ? 0 : type;
}

void Traffic_Install() {
    if (*(uint8_t*)kCallTurboRaceType == 0xE8 && kCallTurboRaceType + 5 + *(int32_t*)(kCallTurboRaceType + 1) == kGetRaceType) {
        int32_t rel = (int32_t)((uintptr_t)&HkTurboRaceType - (kCallTurboRaceType + 5));
        DWORD old;
        VirtualProtect((void*)(kCallTurboRaceType + 1), 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)(kCallTurboRaceType + 1), &rel, 4);
        VirtualProtect((void*)(kCallTurboRaceType + 1), 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kCallTurboRaceType, 5);
    } else {
        Log("traffic: unexpected turbo dial code, its needle keeps the drag range in drag races");
    }
    bool hudOk = true;
    for (uintptr_t slot : kSlotsSetHud) hudOk = hudOk && *(uintptr_t*)slot == kSetHud;
    if (hudOk) {
        for (uintptr_t slot : kSlotsSetHud) {
            DWORD old;
            VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old);
            *(void**)slot = (void*)&HkSetHud;
            VirtualProtect((void*)slot, 4, old, &old);
        }
        Log("traffic: drag races with the %s HUD", g_cfg.normalDragHud ? "normal race" : "drag");
    } else {
        Log("traffic: unexpected player code, NormalDragHud unavailable");
    }
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
