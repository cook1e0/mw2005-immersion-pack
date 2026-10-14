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
    c.gain = ReadFloat(s, "Gain", c.gain);
    c.satGain = ReadFloat(s, "SelfAligningTorque", c.satGain);
    c.satPeakAngle = ReadFloat(s, "SlipAnglePeak", c.satPeakAngle);
    c.driftAlign = ReadFloat(s, "DriftAlign", c.driftAlign);
    c.damperLow = ReadFloat(s, "DamperLowSpeed", c.damperLow);
    c.damperHigh = ReadFloat(s, "DamperHighSpeed", c.damperHigh);
    c.friction = ReadFloat(s, "Friction", c.friction);
    c.standstillFriction = ReadFloat(s, "StandstillFriction", c.standstillFriction);
    c.roadGain = ReadFloat(s, "RoadFeel", c.roadGain);
    c.collisionGain = ReadFloat(s, "Collisions", c.collisionGain);
    c.engineVibration = ReadFloat(s, "EngineVibration", c.engineVibration);
    c.brakeLockVibration = ReadFloat(s, "BrakeLockVibration", c.brakeLockVibration);
    c.menuSpring = ReadFloat(s, "MenuSpring", c.menuSpring);
    c.centerSpring = ReadFloat(s, "CenterSpring", c.centerSpring);
    c.centerSpringParked = Clamp(ReadFloat(s, "CenterSpringParked", c.centerSpringParked), 0.0f, 1.0f);
    c.centerSpringSpeed = Clamp(ReadFloat(s, "CenterSpringSpeed", c.centerSpringSpeed), 10.0f, 400.0f);
    c.softLock = ReadFloat(s, "SoftLock", c.softLock);
    c.speedCentering = ReadFloat(s, "SpeedCentering", c.speedCentering);
    c.speedCenteringRef = Clamp(ReadFloat(s, "SpeedCenteringRef", c.speedCenteringRef), 10.0f, 400.0f);
    c.understeerLightening = Clamp(ReadFloat(s, "UndersteerLightening", c.understeerLightening), 0.0f, 1.0f);
    c.surfaceRumble = ReadFloat(s, "SurfaceRumble", c.surfaceRumble);
    c.curbs = ReadFloat(s, "Curbs", c.curbs);
    c.curbJerk = Clamp(ReadFloat(s, "CurbJerk", c.curbJerk), 20.0f, 10000.0f);
    c.scrapes = ReadFloat(s, "Scrapes", c.scrapes);
    c.landing = ReadFloat(s, "Landing", c.landing);
    c.airborneForce = Clamp(ReadFloat(s, "AirborneForce", c.airborneForce), 0.0f, 1.0f);
    c.forceDirection = Clamp(ReadInt(s, "ForceDirection", c.forceDirection), 0, 1);
    c.minForce = ReadFloat(s, "MinForce", c.minForce);
    c.invertForce = ReadInt(s, "InvertForce", 0) != 0 ? 1.0f : 0.0f;
    c.steerAxis = Clamp(ReadInt(s, "SteerAxis", c.steerAxis), 0, 6);
    c.updateHz = Clamp(ReadInt(s, "UpdateHz", c.updateHz), 30, 1000);
    c.keyReset = Clamp(ReadInt(s, "KeyReset", c.keyReset), 0, 255);
    c.logTelemetry = ReadInt(s, "LogTelemetry", c.logTelemetry) != 0;
    char gameplayIni[MAX_PATH];
    ConfigFile("nfsmw_gameplay.ini", gameplayIni, false);
    c.gameController = GetPrivateProfileIntA("Controls", "GameController", 1, gameplayIni) ? 1.0f : 0.0f;
    c.menuController = GetPrivateProfileIntA("Controls", "MenuController", 1, gameplayIni) ? 1.0f : 0.0f;
    c.menuAxes = GetPrivateProfileIntA("Controls", "MenuAxes", 0, gameplayIni) ? 1.0f : 0.0f;
    c.forceSign = Clamp(ReadInt("Calibration", "ForceSign", 0), -1, 1);
    c.slipSign = Clamp(ReadInt("Calibration", "SlipSign", 0), -1, 1);
    c.rightSign = Clamp(ReadInt("Calibration", "RightSign", 0), -1, 1);
    Log("config: enabled=%d native=%d gain=%.2f sat=%.2f peak=%.1f drift=%.2f damper=%.2f/%.2f "
        "friction=%.2f/%.2f road=%.2f coll=%.2f eng=%.3f lock=%.2f menu=%.2f centre=%.2f (%.0f%% parked, full at %.0f km/h) softlock=%.2f min=%.2f invert=%d axis=%d hz=%d "
        "cal(force=%d slip=%d right=%d) game controller input %s",
        c.enabled, c.nativeFfb, c.gain, c.satGain, c.satPeakAngle, c.driftAlign, c.damperLow, c.damperHigh,
        c.friction, c.standstillFriction, c.roadGain, c.collisionGain, c.engineVibration, c.brakeLockVibration,
        c.menuSpring, c.centerSpring, c.centerSpringParked * 100.0f, c.centerSpringSpeed, c.softLock, c.minForce, c.invertForce != 0, c.steerAxis, c.updateHz, c.forceSign, c.slipSign, c.rightSign,
        c.gameController > 0.5f ? "on" : "hidden");
}

void SaveCalibration(int slipSign, int rightSign, int forceSign) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", forceSign);
    WritePrivateProfileStringA("Calibration", "ForceSign", buf, s_ini);
    snprintf(buf, sizeof(buf), "%d", slipSign);
    WritePrivateProfileStringA("Calibration", "SlipSign", buf, s_ini);
    snprintf(buf, sizeof(buf), "%d", rightSign);
    WritePrivateProfileStringA("Calibration", "RightSign", buf, s_ini);
}

void SaveForceDirection(int mode) {
    WritePrivateProfileStringA("FFB", "ForceDirection", mode ? "1" : "0", s_ini);
}
