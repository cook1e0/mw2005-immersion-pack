#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

Config g_cfg;

static char s_gameDir[MAX_PATH];
static FILE* s_log;
static CRITICAL_SECTION s_logLock;

const char* GameDir() {
    if (!s_gameDir[0]) {
        GetModuleFileNameA(nullptr, s_gameDir, MAX_PATH);
        char* slash = strrchr(s_gameDir, '\\');
        if (slash) slash[1] = 0;
    }
    return s_gameDir;
}

// Directory of this DLL: scripts\ when loaded as an .asi, the game folder as a proxy DLL.
const char* ModDir() {
    static char dir[MAX_PATH];
    if (!dir[0]) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&ModDir, &self);
        GetModuleFileNameA(self, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\');
        if (slash) slash[1] = 0;
    }
    return dir;
}

static bool FileExists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

// Finds one of the mods' .ini files. While this DLL is in scripts\, one next to speed.exe (where
// they were before v0.10.0) holds the player's settings and wins over the default one a release
// zip puts in scripts\; with migrate it is moved over that one. Otherwise: next to this DLL, else
// in scripts\, else (missing) next to this DLL.
void ConfigFile(const char* name, char* out, bool migrate) {
    char other[MAX_PATH];
    snprintf(out, MAX_PATH, "%s%s", ModDir(), name);
    snprintf(other, MAX_PATH, "%s%s", GameDir(), name);
    if (_stricmp(ModDir(), GameDir()) != 0 && FileExists(other)) {
        if (migrate && MoveFileExA(other, out, MOVEFILE_REPLACE_EXISTING)) {
            Log("settings: moved %s next to the mod (%s)", name, ModDir());
            return;
        }
        strcpy(out, other);
        return;
    }
    if (FileExists(out)) return;
    snprintf(other, MAX_PATH, "%sscripts\\%s", GameDir(), name);
    if (FileExists(other)) strcpy(out, other);
}

void LogInit(const char* fileName) {
    InitializeCriticalSection(&s_logLock);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", GameDir(), fileName);
    s_log = fopen(path, "w");
}

void Log(const char* fmt, ...) {
    if (!s_log) return;
    EnterCriticalSection(&s_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
    LeaveCriticalSection(&s_logLock);
}

static float ReadFloat(const char* ini, const char* key, float def) {
    char buf[64], defs[64];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA("VR", key, defs, buf, sizeof(buf), ini);
    return (float)atof(buf);
}

static int ReadInt(const char* ini, const char* key, int def) {
    return (int)GetPrivateProfileIntA("VR", key, def, ini);
}

void LoadConfig() {
    char ini[MAX_PATH];
    ConfigFile("nfsmw_vr.ini", ini, true);
    Config& c = g_cfg;
    c.enabled = ReadInt(ini, "Enabled", c.enabled) != 0;
    c.crashDumps = ReadInt(ini, "CrashDumps", c.crashDumps) != 0;
    c.forceWindowed = ReadInt(ini, "ForceWindowed", c.forceWindowed) != 0;
    c.disableVSync = ReadInt(ini, "DisableVSync", c.disableVSync) != 0;
    c.worldScale = ReadFloat(ini, "WorldScale", c.worldScale);
    c.fovScale = ReadFloat(ini, "FovScale", c.fovScale);
    GetPrivateProfileStringA("VR", "Resolution", c.resolution, c.resolution, sizeof(c.resolution), ini);
    c.resolutionScale = ReadFloat(ini, "ResolutionScale", c.resolutionScale);
    c.levelRoll = ReadFloat(ini, "LevelRoll", c.levelRoll);
    c.levelPitch = ReadFloat(ini, "LevelPitch", c.levelPitch);
    c.screenDistance = ReadFloat(ini, "ScreenDistance", c.screenDistance);
    c.screenWidth = ReadFloat(ini, "ScreenWidth", c.screenWidth);
    c.hudDistance = ReadFloat(ini, "HudDistance", c.hudDistance);
    c.hudWidth = ReadFloat(ini, "HudWidth", c.hudWidth);
    c.nearZ = ReadFloat(ini, "NearZ", c.nearZ);
    if (!(c.nearZ >= 0.0f)) c.nearZ = 0.0f;
    c.hudHeight = ReadFloat(ini, "HudHeight", c.hudHeight);
    c.hudRight = ReadFloat(ini, "HudRight", c.hudRight);
    char bridge[16] = "auto";
    GetPrivateProfileStringA("VR", "Bridge", bridge, bridge, sizeof(bridge), ini);
    c.bridge = _stricmp(bridge, "auto") == 0 ? -1 : atoi(bridge) != 0;
    c.fakeHmd = ReadInt(ini, "FakeHmd", c.fakeHmd) != 0;
    c.fakeYaw = ReadFloat(ini, "FakeYaw", c.fakeYaw);
    c.fakePitch = ReadFloat(ini, "FakePitch", c.fakePitch);
    c.fakeIpd = ReadFloat(ini, "FakeIpd", c.fakeIpd);
    c.dumpFrames = ReadInt(ini, "DumpFrames", c.dumpFrames);
    c.keyRecenter = ReadInt(ini, "KeyRecenter", c.keyRecenter);
    c.keyDump = ReadInt(ini, "KeyDump", c.keyDump);
    c.keyToggle = ReadInt(ini, "KeyToggle", c.keyToggle);
    c.keyLevel = ReadInt(ini, "KeyLevel", c.keyLevel);
    c.keySpectator = ReadInt(ini, "KeySpectator", c.keySpectator);
    GetPrivateProfileStringA("VR", "SpectatorMode", c.spectatorMode, c.spectatorMode, sizeof(c.spectatorMode), ini);
    c.spectatorEye = ReadInt(ini, "SpectatorEye", c.spectatorEye) != 0 ? 1 : 0;
    c.spectatorFov = ReadFloat(ini, "SpectatorFov", c.spectatorFov);
    c.spectatorAspect = ReadFloat(ini, "SpectatorAspect", c.spectatorAspect);
    c.spectatorSmoothing = ReadFloat(ini, "SpectatorSmoothing", c.spectatorSmoothing);
    c.spectatorFollowHead = ReadInt(ini, "SpectatorFollowHead", c.spectatorFollowHead) != 0;
    c.spectatorWidth = ReadInt(ini, "SpectatorWidth", c.spectatorWidth);
    c.seat = ReadInt(ini, "Seat", c.seat) != 0;
    c.hideHead = ReadInt(ini, "HideHead", c.hideHead) != 0;
    c.hideArms = ReadInt(ini, "HideArms", 1) != 0 ? 1.0f : 0.0f;
    c.wheelOverHud = ReadInt(ini, "WheelOverHud", 0) != 0 ? 1.0f : 0.0f;
    c.hudPerCar = ReadInt(ini, "HudPerCar", 0) != 0 ? 1.0f : 0.0f;
    c.dash = ReadInt(ini, "Dash", 1) != 0 ? 1.0f : 0.0f;
    c.dashRight = ReadFloat(ini, "DashRight", c.dashRight);
    c.dashUp = ReadFloat(ini, "DashUp", c.dashUp);
    c.dashZoom = ReadFloat(ini, "DashZoom", c.dashZoom);
    c.headSize = ReadFloat(ini, "HeadSize", c.headSize);
    c.steeringWheel = ReadInt(ini, "SteeringWheel", c.steeringWheel) != 0;
    c.screenRain = ReadInt(ini, "ScreenRain", c.screenRain) != 0;
    c.sharedViewsOnce = ReadInt(ini, "SharedViewsOnce", c.sharedViewsOnce) != 0;
    c.stableReflections = ReadInt(ini, "StableReflections", c.stableReflections) != 0;
    c.mirrors = ReadInt(ini, "Mirrors", c.mirrors) != 0;
    c.gameMirrors = ReadInt(ini, "GameMirrors", c.gameMirrors) != 0;
    GetPrivateProfileStringA("VR", "GameMirrorsSkip", "", c.gameMirrorsSkip, sizeof(c.gameMirrorsSkip), ini);
    c.mirrorFlip = ReadInt(ini, "MirrorFlip", c.mirrorFlip) != 0;
    c.sideMirrorWidth = ReadFloat(ini, "SideMirrorWidth", c.sideMirrorWidth);
    if (!(c.sideMirrorWidth >= 0.3f)) c.sideMirrorWidth = 0.3f;
    if (c.sideMirrorWidth > 1.0f) c.sideMirrorWidth = 1.0f;
    c.mirrorScale = ReadFloat(ini, "MirrorScale", c.mirrorScale);
    c.mirrorContrast = ReadFloat(ini, "MirrorContrast", c.mirrorContrast);
    c.mirrorBrightness = ReadFloat(ini, "MirrorBrightness", c.mirrorBrightness);
    c.keySeatForward = ReadInt(ini, "KeySeatForward", c.keySeatForward);
    c.keySeatBack = ReadInt(ini, "KeySeatBack", c.keySeatBack);
    c.keySeatLeft = ReadInt(ini, "KeySeatLeft", c.keySeatLeft);
    c.keySeatRight = ReadInt(ini, "KeySeatRight", c.keySeatRight);
    c.keySeatUp = ReadInt(ini, "KeySeatUp", c.keySeatUp);
    c.keySeatDown = ReadInt(ini, "KeySeatDown", c.keySeatDown);
    c.keySeatReset = ReadInt(ini, "KeySeatReset", c.keySeatReset);
    Log("config: enabled=%d windowed=%d novsync=%d worldScale=%.3f fovScale=%.3f levelRoll=%.2f levelPitch=%.2f nearZ=%.3f bridge=%d fakeHmd=%d",
        c.enabled, c.forceWindowed, c.disableVSync, c.worldScale, c.fovScale, c.levelRoll, c.levelPitch, c.nearZ, c.bridge,
        c.fakeHmd);
}
