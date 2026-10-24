// Entry point. Loaded by speed.exe as dsound.dll (forwarding every export to the system
// DirectSound), or as an .asi by an ASI loader.
#include "common.h"
#include "game.h"
#include "ini_migration.h"

// --- dsound.dll proxy ------------------------------------------------------------------------
static const char* const kExports[12] = {
    "DirectSoundCreate", "DirectSoundEnumerateA", "DirectSoundEnumerateW", "DllCanUnloadNow",
    "DllGetClassObject", "DirectSoundCaptureCreate", "DirectSoundCaptureEnumerateA",
    "DirectSoundCaptureEnumerateW", "GetDeviceID", "DirectSoundFullDuplexCreate",
    "DirectSoundCreate8", "DirectSoundCaptureCreate8",
};
extern "C" FARPROC g_dsound[12];
FARPROC g_dsound[12];

// Each export jumps straight to the real function, whatever its signature.
#define FORWARD(i, name) \
    __asm__(".text\n.globl _Proxy_" #name "\n_Proxy_" #name ":\n\tjmp *(_g_dsound+" #i "*4)\n");
FORWARD(0, DirectSoundCreate)
FORWARD(1, DirectSoundEnumerateA)
FORWARD(2, DirectSoundEnumerateW)
FORWARD(3, DllCanUnloadNow)
FORWARD(4, DllGetClassObject)
FORWARD(5, DirectSoundCaptureCreate)
FORWARD(6, DirectSoundCaptureEnumerateA)
FORWARD(7, DirectSoundCaptureEnumerateW)
FORWARD(8, GetDeviceID)
FORWARD(9, DirectSoundFullDuplexCreate)
FORWARD(10, DirectSoundCreate8)
FORWARD(11, DirectSoundCaptureCreate8)

// Marker: lets an .asi copy of this mod see that the dsound.dll proxy is installed too.
extern "C" int __cdecl NfsmwGameplay_Loaded() { return 1; }

static bool LoadedAs(HINSTANCE self, const char* file) {
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    return _stricmp(name ? name + 1 : path, file) == 0;
}

static bool LoadRealDsound() {
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\dsound.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) return false;
    for (int i = 0; i < 12; i++) g_dsound[i] = GetProcAddress(real, kExports[i]);
    return g_dsound[0] != nullptr;
}

// Up to v0.6.0 this was the H-shifter mod with nfsmw_hshifter.ini: an existing one becomes
// nfsmw_gameplay.ini, keeping the player's settings and bindings.
static void MigrateOldIni() {
    char oldPath[MAX_PATH];
    snprintf(oldPath, sizeof(oldPath), "%snfsmw_hshifter.ini", GameDir());
    MigrateLegacyIni(oldPath, IniPath());
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    // Installed both as dsound.dll and as an .asi: the proxy runs, this copy stays out of the
    // way (and out of the log the proxy already has open).
    HMODULE proxy = GetModuleHandleA("dsound.dll");
    if (!LoadedAs(inst, "dsound.dll") && proxy && GetProcAddress(proxy, "NfsmwGameplay_Loaded")) return TRUE;
    LogInit();
    Log("NFSMW gameplay mod " MOD_VERSION ", built " __DATE__ " " __TIME__);
    if (LoadedAs(inst, "dsound.dll") && !LoadRealDsound()) {
        Log("cannot load the system dsound.dll - game audio will fail");
        MessageBoxA(nullptr, "nfsmw_gameplay: cannot load the system dsound.dll", "NFSMW gameplay mod", MB_ICONERROR);
        return FALSE;
    }
    MigrateOldIni();
    LoadConfig();
    if (!g_cfg.enabled) {
        Log("disabled in ini");
        return TRUE;
    }
    if (!Game_Install()) Log("speed.exe is not v1.3 - mod inactive");
    return TRUE;
}
