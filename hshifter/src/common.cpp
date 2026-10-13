#include "common.h"
#include <stdio.h>
#include <stdarg.h>

Config g_cfg;

static char s_dir[MAX_PATH];
static char s_ini[MAX_PATH];
static FILE* s_log;
static CRITICAL_SECTION s_logLock;

const char* GameDir() {
    if (!s_dir[0]) {
        GetModuleFileNameA(nullptr, s_dir, MAX_PATH);
        char* slash = strrchr(s_dir, '\\');
        if (slash) slash[1] = 0;
    }
    return s_dir;
}

const char* IniPath() {
    if (!s_ini[0]) snprintf(s_ini, sizeof(s_ini), "%snfsmw_hshifter.ini", GameDir());
    return s_ini;
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
    snprintf(path, sizeof(path), "%snfsmw_hshifter.log", GameDir());
    s_log = fopen(path, "w");
}

void Log(const char* fmt, ...) {
    if (!s_log) return;
    EnterCriticalSection(&s_logLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(s_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
    LeaveCriticalSection(&s_logLock);
}

static std::string ReadStr(const char* sec, const char* key, const char* def) {
    char buf[512];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), IniPath());
    // strip trailing ";" comments and whitespace
    char* c = strchr(buf, ';');
    if (c) *c = 0;
    std::string s = buf;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

static float ReadFloat(const char* sec, const char* key, float def) {
    std::string s = ReadStr(sec, key, "");
    return s.empty() ? def : (float)atof(s.c_str());
}

static int ReadInt(const char* sec, const char* key, int def) {
    std::string s = ReadStr(sec, key, "");
    return s.empty() ? def : atoi(s.c_str());
}

void LoadConfig() {
    Config& c = g_cfg;
    c.enabled = ReadInt("General", "Enabled", 1) != 0;
    c.toggle = ReadStr("General", "Toggle", "");
    c.logInputs = ReadInt("General", "LogInputs", 0) != 0;

    for (int i = 0; i < kMaxJoys; i++) {
        char key[16];
        snprintf(key, sizeof(key), "Joy%d", i);
        c.joyName[i] = ReadStr("Devices", key, "");
    }

    c.clutch = ReadStr("Clutch", "Clutch", "");
    c.clutchReleased = ReadInt("Clutch", "Released", -1);
    c.clutchPressed = ReadInt("Clutch", "Pressed", -1);
    c.deadzone = Clamp(ReadFloat("Clutch", "Deadzone", c.deadzone), 0.0f, 0.4f);
    c.biteStart = Clamp(ReadFloat("Clutch", "BiteStart", c.biteStart), 0.0f, 1.0f);
    c.biteEnd = Clamp(ReadFloat("Clutch", "BiteEnd", c.biteEnd), 0.0f, 1.0f);
    if (c.biteStart < c.biteEnd + 0.02f) c.biteStart = c.biteEnd + 0.02f;
    c.strength = Clamp(ReadFloat("Clutch", "Strength", c.strength), 0.1f, 2.0f);
    c.lockRpm = Clamp(ReadFloat("Clutch", "LockRPM", c.lockRpm), 10.0f, 2000.0f);
    c.buttonPressTime = Clamp(ReadFloat("Clutch", "ButtonPressTime", c.buttonPressTime), 0.0f, 5.0f);
    c.buttonReleaseTime = Clamp(ReadFloat("Clutch", "ButtonReleaseTime", c.buttonReleaseTime), 0.0f, 5.0f);

    c.gear[0] = ReadStr("Shifter", "Reverse", "");
    for (int g = 1; g <= kMaxGears; g++) {
        char key[16];
        snprintf(key, sizeof(key), "Gear%d", g);
        c.gear[g] = ReadStr("Shifter", key, "");
    }
    c.neutral = ReadStr("Shifter", "Neutral", "");
    c.latching = ReadInt("Shifter", "Latching", 0) != 0;
    c.sequential = ReadInt("Shifter", "Sequential", 0) != 0;
    c.shiftUp = ReadStr("Shifter", "ShiftUp", c.shiftUp.c_str());
    c.shiftDown = ReadStr("Shifter", "ShiftDown", c.shiftDown.c_str());
    c.requireClutch = ReadInt("Shifter", "RequireClutch", 1) != 0;
    c.shiftPedal = Clamp(ReadFloat("Shifter", "ClutchToShift", c.shiftPedal), 0.0f, 1.0f);
    c.revMatchRpm = Clamp(ReadFloat("Shifter", "RevMatchRPM", 0.0f), 0.0f, 3000.0f);
    c.blockWrongDirection = ReadInt("Shifter", "BlockWrongDirection", 1) != 0;
    c.keepShiftDelay = ReadInt("Shifter", "KeepShiftDelay", 0) != 0;
    c.grind = ReadInt("Shifter", "Grind", 1) != 0;
    c.grindSound = (uint32_t)strtoul(ReadStr("Shifter", "GrindSound", "0xBC2DFA2F").c_str(), nullptr, 0);
    c.sequentialNeedsClutch = ReadInt("Shifter", "SequentialNeedsClutch", 0) != 0;

    c.engineWear = ReadInt("Engine", "EngineWear", 1) != 0;
    c.revLimiter = ReadInt("Engine", "RevLimiter", 0) != 0;
    c.heatUp = Clamp(ReadFloat("Engine", "HeatUp", c.heatUp), 0.0f, 100.0f);
    c.heatUpMax = Clamp(ReadFloat("Engine", "HeatUpMaxRPM", c.heatUpMax), 0.0f, 100.0f);
    c.coolDown = Clamp(ReadFloat("Engine", "CoolDown", c.coolDown), 0.0f, 100.0f);
    c.powerLossStart = Clamp(ReadFloat("Engine", "PowerLossStart", c.powerLossStart), 0.0f, 0.99f);
    c.powerLoss = Clamp(ReadFloat("Engine", "PowerLoss", c.powerLoss), 0.0f, 1.0f);
    c.totaled = ReadInt("Engine", "Totaled", 1) != 0;
    c.debugBlow = ReadStr("Engine", "DebugBlow", "");
    c.debugRepair = ReadStr("Engine", "DebugRepair", "");
    c.debugDamage = ReadStr("Damage", "DebugDamage", "");

    c.damage = ReadInt("Damage", "Damage", 1) != 0;
    c.damageMinImpact = Clamp(ReadFloat("Damage", "MinImpact", c.damageMinImpact), 0.0f, 100.0f);
    c.damageStrength = Clamp(ReadFloat("Damage", "Strength", c.damageStrength), 0.0f, 10.0f);
    c.damageSmoke = ReadInt("Damage", "Smoke", 1) != 0;
    c.trafficDensity = Clamp(ReadFloat("Traffic", "Density", c.trafficDensity), 0.0f, 3.0f);
    c.smokeTrail = Clamp(ReadFloat("Damage", "SmokeTrail", c.smokeTrail), 0.0f, 1.0f);
    c.smokeFrom = Clamp(ReadFloat("Damage", "SmokeFrom", c.smokeFrom), 0.5f, 1.0f);   // the game smokes at <= 50% health
    c.damageLossStart = Clamp(ReadFloat("Damage", "PowerLossStart", c.damageLossStart), 0.0f, 0.99f);
    c.damageLoss = Clamp(ReadFloat("Damage", "PowerLoss", c.damageLoss), 0.0f, 1.0f);

    c.poppedTires = ReadInt("Tires", "PoppedTires", 1) != 0;
    c.tireHandicap = Clamp(ReadFloat("Tires", "Handicap", c.tireHandicap), 0.0f, 3.0f);
    c.debugPopTires = ReadStr("Tires", "DebugPopTires", "");

    c.hudMeter = ReadInt("HUD", "Meter", 1) != 0;
    c.speedbreaker = ReadInt("HUD", "Speedbreaker", 0) != 0;
    c.hudIcon = ReadStr("HUD", "Icon", c.hudIcon.c_str());
    c.hudHeatFrom = Clamp(ReadFloat("HUD", "HeatFrom", c.hudHeatFrom), 0.0f, 0.95f);
    c.hudWarn = Clamp(ReadFloat("HUD", "Warn", c.hudWarn), 0.0f, 1.0f);
    c.hudDepth = ReadFloat("HUD", "Depth", c.hudDepth);
    c.hudFillUp = ReadInt("HUD", "FillUp", 1) != 0;

    c.physics = ReadInt("Physics", "Physics", 1) != 0;
    c.tractionControl = Clamp(ReadFloat("Physics", "TractionControl", c.tractionControl), 0.0f, 1.0f);
    c.stability = Clamp(ReadFloat("Physics", "Stability", c.stability), 0.0f, 1.0f);
    c.driftAssist = Clamp(ReadFloat("Physics", "DriftAssist", c.driftAssist), 0.0f, 1.0f);
    c.grip = Clamp(ReadFloat("Physics", "Grip", c.grip), 0.3f, 2.0f);
    c.torqueSplit = Clamp(ReadFloat("Physics", "TorqueSplit", c.torqueSplit), 0.0f, 3.0f);
    c.gravity = Clamp(ReadFloat("Physics", "Gravity", c.gravity), 0.1f, 3.0f);
    c.gravityAll = ReadInt("Physics", "GravityAllCars", 0) != 0;

    c.gauge = ReadInt("UI", "Gauge", 0) != 0;
    c.gaugeX = Clamp(ReadFloat("UI", "GaugeX", c.gaugeX), 0.0f, 1.0f);
    c.gaugeY = Clamp(ReadFloat("UI", "GaugeY", c.gaugeY), 0.0f, 1.0f);
    c.uiScale = Clamp(ReadFloat("UI", "Scale", c.uiScale), 0.5f, 3.0f);
    c.menu = ReadStr("UI", "Menu", c.menu.c_str());
    c.menuUp = ReadStr("UI", "MenuUp", c.menuUp.c_str());
    c.menuDown = ReadStr("UI", "MenuDown", c.menuDown.c_str());
    c.menuLeft = ReadStr("UI", "MenuLeft", c.menuLeft.c_str());
    c.menuRight = ReadStr("UI", "MenuRight", c.menuRight.c_str());
}
