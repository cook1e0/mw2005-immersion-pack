// Entry point. Works as a dinput8.dll proxy in the game folder, or as an .asi plugin
// (in which case speed.exe's DirectInput8Create import is redirected instead).
#include "common.h"
#include "dinput_wrap.h"
#include "ffb.h"
#include "telemetry.h"
#include <string.h>

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static HMODULE s_self;
static HMODULE s_realDll;
static DirectInput8Create_t s_origCreate;  // .asi mode: the game's original import
static bool s_active;

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

// speed.exe v1.3: image base 0x400000, SizeOfImage 0x5C7000.
static bool IsSupportedExe() {
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return base == (uint8_t*)0x400000 && nt->OptionalHeader.SizeOfImage == 0x5C7000;
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
        Log("speed.exe is not v1.3 (6,029,312 bytes) - mod inactive, passthrough only");
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
