// Traffic density multiplier ([Traffic] Density).
//
// AITrafficManager::Update takes ComputeDensity (free roam 1.0, races the event's traffic level,
// x0.75 in a pursuit), looks up a spawn rate in TrafficDensitySpawnRates (8 at density 1) and
// adds rate * dt to each pattern vehicle's spawn timer; a car spawns when its timer passes the
// pattern's threshold. That table lookup is wrapped to multiply the rate by Density.
// NeedsTraffic and NextSpawn both budget for 20 cars ("vehicles - 20 < traffic"); above 1 the
// budget grows with Density, up to kMaxCars.
//
// No SUV cops ([Traffic] NoSuvCops): every cop car comes from
// AICopManager::GetAvailableCopVehicleByName (reuses an idle cop of that name or creates one),
// so its calls are redirected. Heavy support asks for copsuv / copsuvl by name and gets nothing
// (the game drops the request); other spawns asking for an SUV (copsuvpatrol from the heat
// level's cop list) re-roll GetRandomValidCopCar, else copmidsize.
#include "traffic.h"
#include "common.h"

namespace {

constexpr uintptr_t kSpawnRateCall = 0x439BF0;      // AITrafficManager::Update: call Table::GetValue(density)
constexpr uintptr_t kTableGetValue = 0x6533B0;      // float Table::GetValue(float)
constexpr uintptr_t kNeedsCap = 0x41EC15;           // NeedsTraffic: cmp eax,0x14 (imm8)
constexpr uintptr_t kNextSpawnCap = 0x426332;       // NextSpawn: add edx,0x14 (imm8)
constexpr int kGameCars = 20;
constexpr int kMaxCars = 40;

typedef float(__thiscall* GetValue_t)(void* table, float x);

bool s_capOk;

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

float __fastcall SpawnRate(void* table, void*, float density) {
    float rate = ((GetValue_t)kTableGetValue)(table, density);
    float k = g_cfg.trafficDensity;
    if (s_capOk) {
        int cap = k > 1.0f ? std::min(kMaxCars, (int)(kGameCars * k + 0.5f)) : kGameCars;
        *(uint8_t*)kNeedsCap = (uint8_t)cap;
        *(uint8_t*)kNextSpawnCap = (uint8_t)cap;
    }
    return rate * k;
}

}  // namespace

void Traffic_Install() {
    Cops_Install();
    if (*(uint8_t*)kSpawnRateCall != 0xE8 ||
        kSpawnRateCall + 5 + *(int32_t*)(kSpawnRateCall + 1) != kTableGetValue) {
        Log("traffic: unexpected code, no density multiplier");
        return;
    }
    DWORD old;
    s_capOk = *(uint8_t*)kNeedsCap == kGameCars && *(uint8_t*)kNextSpawnCap == kGameCars &&
              *(uint8_t*)(kNeedsCap - 2) == 0x83 && *(uint8_t*)(kNextSpawnCap - 2) == 0x83;
    if (s_capOk) {
        VirtualProtect((void*)kNeedsCap, 1, PAGE_EXECUTE_READWRITE, &old);
        VirtualProtect((void*)kNextSpawnCap, 1, PAGE_EXECUTE_READWRITE, &old);
    }
    int32_t rel = (int32_t)((uintptr_t)&SpawnRate - (kSpawnRateCall + 5));
    VirtualProtect((void*)kSpawnRateCall, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)(kSpawnRateCall + 1), &rel, 4);
    VirtualProtect((void*)kSpawnRateCall, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kSpawnRateCall, 5);
    Log("traffic: density x%.2f%s", g_cfg.trafficDensity, s_capOk ? "" : " (car limit not patched)");
}
