// Entry point. Works as a dinput8.dll proxy in the game folder, or as an .asi plugin
// (in which case speed.exe's DirectInput8Create import is redirected instead).
#include "common.h"
#include "dinput_wrap.h"
#include "ffb.h"
#include "telemetry.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static HMODULE s_self;
static HMODULE s_realDll;
static DirectInput8Create_t s_origCreate;  // .asi mode: the game's original import
static bool s_active;

// The master Gain, for the gameplay mod's F4 tuning menu (read live by the effect update).
extern "C" float* __cdecl NfsmwFfb_Gain() { return &g_cfg.gain; }
extern "C" int __cdecl NfsmwFfb_Reset() { return s_active && Ffb_RequestReset() ? 1 : 0; }
extern "C" int __cdecl NfsmwFfb_Test() { return s_active && Ffb_RequestTest() ? 1 : 0; }
extern "C" int __cdecl NfsmwFfb_DeviceName(int index, char* out, int len) { return s_active ? Ffb_DeviceName(index, out, len) : 0; }
extern "C" int __cdecl NfsmwFfb_Status(char* out, int len) {
    if (!out || len <= 0) return 0;
    if (!s_active || !g_cfg.enabled) { snprintf(out, len, "Off: [FFB] Enabled=0"); return 0; }
    if (g_cfg.nativeFfb) { snprintf(out, len, "Off: the game's own force feedback (NativeFFB=1)"); return 0; }
    return Ffb_Status(out, len);
}
extern "C" void __cdecl NfsmwFfb_MenuOpen(int open) { if (s_active) Ffb_SetMenuOpen(open != 0); }

// Live settings for the gameplay mod (its F4 menu and [Controls]), by [FFB] key name. Null if
// unknown. GameController, MenuController and MenuAxes hide controller input from the game;
// SoftLockAt is the steering lock's share of the wheel's rotation.
extern "C" float* __cdecl NfsmwFfb_Setting(const char* name) {
    if (!name) return nullptr;
    static const struct { const char* key; float* value; } kSettings[] = {
        {"Gain", &g_cfg.gain}, {"MaxForce", &g_cfg.maxForce}, {"Aligning", &g_cfg.aligning},
        {"Centering", &g_cfg.centering}, {"Damping", &g_cfg.damping}, {"Friction", &g_cfg.friction},
        {"SoftLock", &g_cfg.softLock}, {"SlewRate", &g_cfg.slewRate}, {"SmoothingHz", &g_cfg.smoothingHz},
        {"Reverse", &g_cfg.reverse}, {"Device", &g_cfg.device}, {"SoftLockAt", &g_cfg.softLockAt},
        {"GameController", &g_cfg.gameController}, {"MenuController", &g_cfg.menuController},
        {"MenuAxes", &g_cfg.menuAxes}, {"CentreWeight", &g_cfg.centreWeight}, {"EngineRumble", &g_cfg.engineRumble},
        {"Kerbs", &g_cfg.kerbs}, {"SurfaceRumble", &g_cfg.surfaceRumble}, {"Crashes", &g_cfg.crashes},
        {"Scrapes", &g_cfg.scrapes}, {"Breakables", &g_cfg.breakables},
        {"Landing", &g_cfg.landing},
        {"CarHits", &g_cfg.carHits},
        {"HardImpactJerk", &g_cfg.hardImpactJerk},
        {"DamageHits", &g_cfg.damageHits}, {"DamageHitStrength", &g_cfg.damageHitStrength},
        {"OffroadVibration", &g_cfg.offroadVibration},
        {"NosLighter", &g_cfg.nosLighter},
        {"SpinLighter", &g_cfg.spinLighter},
        {"ParkingFriction", &g_cfg.parkingFriction},
    };
    for (auto& s : kSettings)
        if (!strcmp(name, s.key)) return s.value;
    return nullptr;
}

static FARPROC RealProc(const char* name) {
    if (!s_realDll) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        s_realDll = LoadLibraryA(path);
        if (!s_realDll) Log("proxy: failed to load %s", path);
    }
    return s_realDll ? GetProcAddress(s_realDll, name) : nullptr;
}

static HRESULT CreateAndWrap(DirectInput8Create_t create, HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    if (!create) return DIERR_GENERIC;
    HRESULT hr = create(inst, ver, riid, out, outer);
    Log("DirectInput8Create(ver=0x%lX) -> 0x%08lX", ver, hr);
    if (SUCCEEDED(hr) && s_active && *out) WrapDirectInput(riid, out);
    return hr;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    return CreateAndWrap((DirectInput8Create_t)RealProc("DirectInput8Create"), inst, ver, riid, out, outer);
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto f = (HRESULT (WINAPI*)())RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o) {
    auto f = (HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*))RealProc("DllGetClassObject");
    return f ? f(c, i, o) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto f = (HRESULT (WINAPI*)())RealProc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto f = (HRESULT (WINAPI*)())RealProc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
LPCDIDATAFORMAT WINAPI Proxy_GetdfDIJoystick() {
    auto f = (LPCDIDATAFORMAT (WINAPI*)())RealProc("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    return CreateAndWrap(s_origCreate, inst, ver, riid, out, outer);
}

static bool Expect(uintptr_t at, std::initializer_list<uint8_t> bytes) {
    const uint8_t* p = (const uint8_t*)at;
    size_t i = 0;
    for (uint8_t b : bytes) {
        if (p[i] != b) {
            Log("unexpected code at 0x%06X+%u: %02X (want %02X)", (unsigned)at, (unsigned)i, p[i], b);
            return false;
        }
        i++;
    }
    return true;
}

// speed.exe v1.3: image base 0x400000, SizeOfImage 0x5C7000 (larger for v1.3 exes with extra
// appended sections, e.g. widescreen/no-CD patches), identified by the bytes we patch or rely on.
static bool IsSupportedExe() {
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (base != (uint8_t*)0x400000 || nt->OptionalHeader.SizeOfImage < 0x5C7000) {
        Log("host is not speed.exe (base %p, size 0x%X)", base, (unsigned)nt->OptionalHeader.SizeOfImage);
        return false;
    }
    return Expect(0x6F22C6, {0xE8, 0xC5, 0xC1, 0xFF, 0xFF})               // call LocalPlayer::DoFFB
        && Expect(0x6EE490, {0x6A, 0xFF, 0x68, 0x71, 0xE9, 0x87, 0x00})   // LocalPlayer::DoFFB
        && Expect(0x6EE400, {0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x4C})   // LocalPlayer::CanDoFFB
        && Expect(0x6E8D20, {0xA1, 0xE0, 0x85, 0x98, 0x00, 0x85, 0xC0, 0x75, 0x01, 0xC3, 0x8B, 0x40, 0x2C, 0xC3}) // Sim::GetState
        && Expect(0x468380, {0x8B, 0x0D, 0x90, 0x5E, 0x92, 0x00, 0x33, 0xC0, 0x83, 0xF9, 0x06, 0x0F, 0x94, 0xC0, 0xC3}) // in-world predicate
        && Expect(0x525B20, {0x8B, 0x0D, 0xE0, 0xCA, 0x91, 0x00, 0x80, 0x39, 0x00}) // FEManager::ShouldPauseSimulation
        // DINPUT8.dll import lookup entry backing IAT slot 0x89001C -> hint/name 0x4E8784
        // ("DirectInput8Create"); the IAT slot itself holds the resolved address at runtime.
        && Expect(0x8E7E38, {0x84, 0x87, 0x4E, 0x00, 0x00, 0x00, 0x00, 0x00})
        && Expect(0x8E8786, {'D', 'i', 'r', 'e', 'c', 't', 'I', 'n', 'p', 'u', 't', '8', 'C', 'r', 'e', 'a', 't', 'e', 0});
}

static bool LoadedAsProxy() {
    char path[MAX_PATH];
    GetModuleFileNameA(s_self, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    return _stricmp(name ? name + 1 : path, "dinput8.dll") == 0;
}

// .asi mode: redirect the game's DINPUT8.dll!DirectInput8Create IAT slot.
static void PatchImport() {
    void** slot = (void**)0x89001C;
    s_origCreate = (DirectInput8Create_t)*slot;
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (void*)&Hooked_DirectInput8Create;
    VirtualProtect(slot, 4, old, &old);
    Log("asi mode: redirected DirectInput8Create import");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    s_self = inst;
    DisableThreadLibraryCalls(inst);
    LogInit();
    Log("NFSMW force feedback mod, built " __DATE__ " " __TIME__);
    LoadConfig();
    InitializeCriticalSection(&g_diLock);
    if (!g_cfg.enabled) {
        Log("disabled in ini, passthrough only");
        return TRUE;
    }
    if (!IsSupportedExe()) {
        Log("speed.exe is not v1.3 - mod inactive, passthrough only");
        return TRUE;
    }
    s_active = true;
    if (!LoadedAsProxy()) PatchImport();
    if (g_cfg.nativeFfb) {
        Log("NativeFFB=1: game's own force feedback passes through, mod effects off");
        return TRUE;
    }
    Telemetry_Install();
    Ffb_Start();
    return TRUE;
}
