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

double Now() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void LogInit() {
    InitializeCriticalSection(&s_logLock);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%snfsmw_ffb.log", GameDir());
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

static char s_ini[MAX_PATH];

static float ReadFloat(const char* sec, const char* key, float def) {
    char buf[64], defs[64];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA(sec, key, defs, buf, sizeof(buf), s_ini);
    return (float)atof(buf);
}

static int ReadInt(const char* sec, const char* key, int def) {
    return (int)GetPrivateProfileIntA(sec, key, def, s_ini);
}

void LoadConfig() {
    ConfigFile("nfsmw_ffb.ini", s_ini, true);
    Config& c = g_cfg;
    const char* s = "FFB";
    c.enabled = ReadInt(s, "Enabled", c.enabled) != 0;
    c.nativeFfb = ReadInt(s, "NativeFFB", c.nativeFfb) != 0;
    c.gain = Clamp(ReadFloat(s, "Gain", c.gain), 0.0f, 3.0f);
    c.maxForce = Clamp(ReadFloat(s, "MaxForce", c.maxForce), 0.05f, 1.0f);
    c.limitKnee = Clamp(ReadFloat(s, "LimitKnee", c.limitKnee), 0.3f, 0.95f);
    c.slewRate = Clamp(ReadFloat(s, "SlewRate", c.slewRate), 1.0f, 200.0f);
    c.smoothingHz = Clamp(ReadFloat(s, "SmoothingHz", c.smoothingHz), 2.0f, 100.0f);
    c.oscillationGuard = ReadInt(s, "OscillationGuard", c.oscillationGuard) != 0;
    c.aligning = Clamp(ReadFloat(s, "Aligning", c.aligning), 0.0f, 3.0f);
    c.gripPeakDeg = Clamp(ReadFloat(s, "GripPeakAngle", c.gripPeakDeg), 1.0f, 30.0f);
    c.driftAlign = Clamp(ReadFloat(s, "DriftAlign", c.driftAlign), 0.0f, 2.0f);
    c.loadSensitivity = Clamp(ReadFloat(s, "LoadSensitivity", c.loadSensitivity), 0.0f, 2.0f);
    c.centering = Clamp(ReadFloat(s, "Centering", c.centering), 0.0f, 2.0f);
    c.centeringParked = Clamp(ReadFloat(s, "CenteringParked", c.centeringParked), 0.0f, 1.0f);
    c.centeringSpeed = Clamp(ReadFloat(s, "CenteringSpeed", c.centeringSpeed), 10.0f, 400.0f);
    c.damping = Clamp(ReadFloat(s, "Damping", c.damping), 0.0f, 2.0f);
    c.friction = Clamp(ReadFloat(s, "Friction", c.friction), 0.0f, 1.0f);
    c.parkingFriction = Clamp(ReadFloat(s, "ParkingFriction", c.parkingFriction), 0.0f, 1.0f);
    c.softLock = Clamp(ReadFloat(s, "SoftLock", c.softLock), 0.0f, 1.0f);
    c.menuCentering = Clamp(ReadFloat(s, "MenuCentering", c.menuCentering), 0.0f, 1.0f);
    c.centreWeight = Clamp(ReadFloat(s, "CentreWeight", c.centreWeight), 0.0f, 1.0f);
    c.engineRumble = Clamp(ReadFloat(s, "EngineRumble", c.engineRumble), 0.0f, 1.0f);
    c.kerbs = Clamp(ReadFloat(s, "Kerbs", c.kerbs), 0.0f, 2.0f);
    c.surfaceRumble = Clamp(ReadFloat(s, "SurfaceRumble", c.surfaceRumble), 0.0f, 2.0f);
    c.crashes = Clamp(ReadFloat(s, "Crashes", c.crashes), 0.0f, 2.0f);
    c.scrapes = Clamp(ReadFloat(s, "Scrapes", c.scrapes), 0.0f, 2.0f);
    c.breakables = Clamp(ReadFloat(s, "Breakables", c.breakables), 0.0f, 2.0f);
    c.landing = Clamp(ReadFloat(s, "Landing", c.landing), 0.0f, 2.0f);
    c.offroadVibration = Clamp(ReadFloat(s, "OffroadVibration", c.offroadVibration), 0.0f, 2.0f);
    c.nosLighter = Clamp(ReadFloat(s, "NosLighter", c.nosLighter), 0.0f, 0.9f);
    c.device = (float)Clamp(ReadInt(s, "Device", 0), -1, 15);
    // ForceSign moved from [Calibration] (written by older versions) to [FFB].
    int legacySign = ReadInt("Calibration", "ForceSign", 0);
    c.forceSign = ReadInt(s, "ForceSign", legacySign ? legacySign : 1) < 0 ? -1 : 1;
    c.reverse = ReadInt(s, "Reverse", 0) != 0 ? 1.0f : 0.0f;
    c.forceDirection = Clamp(ReadInt(s, "ForceDirection", c.forceDirection), 0, 1);
    c.restartEffects = ReadInt(s, "RestartEffects", 1) != 0;
    c.steerAxis = Clamp(ReadInt(s, "SteerAxis", c.steerAxis), 0, 6);
    c.updateHz = Clamp(ReadInt(s, "UpdateHz", c.updateHz), 60, 1000);
    c.keyReset = Clamp(ReadInt(s, "KeyReset", c.keyReset), 0, 255);
    c.logTelemetry = ReadInt(s, "LogTelemetry", c.logTelemetry) != 0;
    char gameplayIni[MAX_PATH];
    ConfigFile("nfsmw_gameplay.ini", gameplayIni, false);
    c.gameController = GetPrivateProfileIntA("Controls", "GameController", 1, gameplayIni) ? 1.0f : 0.0f;
    c.menuController = GetPrivateProfileIntA("Controls", "MenuController", 1, gameplayIni) ? 1.0f : 0.0f;
    c.menuAxes = GetPrivateProfileIntA("Controls", "MenuAxes", 0, gameplayIni) ? 1.0f : 0.0f;
    Log("config: enabled=%d native=%d gain=%.2f max=%.2f knee=%.2f slew=%.0f/s smooth=%.0fHz guard=%d | aligning=%.2f "
        "peak=%.1f drift=%.2f load=%.2f centering=%.2f (%.0f%% parked, full at %.0f km/h) damping=%.2f friction=%.2f/%.2f "
        "softlock=%.2f menu=%.2f centre=%.2f | engine=%.2f kerbs=%.2f surface=%.2f crashes=%.2f scrapes=%.2f | device=%d sign=%d reverse=%d dir=%d restart=%d axis=%d hz=%d game controller input %s",
        c.enabled, c.nativeFfb, c.gain, c.maxForce, c.limitKnee, c.slewRate, c.smoothingHz, c.oscillationGuard, c.aligning,
        c.gripPeakDeg, c.driftAlign, c.loadSensitivity, c.centering, c.centeringParked * 100.0f, c.centeringSpeed, c.damping,
        c.friction, c.parkingFriction, c.softLock, c.menuCentering, c.centreWeight, c.engineRumble, c.kerbs,
        c.surfaceRumble, c.crashes, c.scrapes, (int)c.device, c.forceSign, c.reverse > 0.5f,
        c.forceDirection, c.restartEffects, c.steerAxis, c.updateHz, c.gameController > 0.5f ? "on" : "hidden");
}
