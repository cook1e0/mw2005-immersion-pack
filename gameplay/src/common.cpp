#include "common.h"
#include <stdio.h>
#include <stdarg.h>

Config g_cfg;
const char* const kButtonKeys[6] = {"Handbrake", "Nitrous", "Speedbreaker", "ResetCar", "Camera", "LookBack"};
const char* const kSeatKeys[7] = {"SeatForward", "SeatBack", "SeatLeft", "SeatRight", "SeatUp", "SeatDown", "SeatReset"};

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
// in scripts\, else (missing) next to this DLL. A missing one is made from <name>.default next to
// this DLL: a release ships its settings that way, so unpacking it never overwrites anyone's.
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
    if (FileExists(other)) {
        strcpy(out, other);
        return;
    }
    snprintf(other, MAX_PATH, "%s.default", out);
    if (FileExists(other) && CopyFileA(other, out, TRUE)) Log("settings: %s made from %s.default", name, name);
}

const char* IniPath() {
    if (!s_ini[0]) ConfigFile("nfsmw_gameplay.ini", s_ini, true);
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
    snprintf(path, sizeof(path), "%snfsmw_gameplay.log", GameDir());
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

bool LegacyGripBalance(const char* section, const char* path, float* grip, float* balance) {
    char buf[64];
    if (GetPrivateProfileStringA(section, "GripBalance", "", buf, sizeof(buf), path) > 0) return false;
    float f = 1.0f, r = 1.0f;
    bool any = false;
    if (GetPrivateProfileStringA(section, "FrontGrip", "", buf, sizeof(buf), path) > 0)
        f = Clamp((float)atof(buf), 0.5f, 1.5f), any = true;
    if (GetPrivateProfileStringA(section, "RearGrip", "", buf, sizeof(buf), path) > 0)
        r = Clamp((float)atof(buf), 0.5f, 1.5f), any = true;
    if (!any) return false;
    *balance = Clamp((f - r) / (f + r), -0.5f, 0.5f);
    *grip = Clamp(*grip * (f + r) * 0.5f, 0.3f, 2.0f);
    return true;
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
    // The widescreen fix's ini ships as NFSMostWanted.WidescreenFix.ini.default too (so unpacking
    // a release never overwrites anyone's); made from it here if missing. The fix loads before
    // this mod, so on a fresh install it may have run on its own defaults this once.
    char wsfix[MAX_PATH];
    ConfigFile("NFSMostWanted.WidescreenFix.ini", wsfix, false);
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

    c.steer = ReadStr("Controls", "Steering", "");
    c.throttle = ReadStr("Controls", "Accelerate", "");
    c.brake = ReadStr("Controls", "Brake", "");
    c.throttleRest = ReadInt("Controls", "AccelerateRest", -1);
    c.brakeRest = ReadInt("Controls", "BrakeRest", -1);
    for (int i = 0; i < 6; i++) c.button[i] = ReadStr("Controls", kButtonKeys[i], "");
    c.recenterVr = ReadStr("Controls", "RecenterVr", "");
    c.toggleAbs = ReadStr("Controls", "ToggleAbs", "");
    c.toggleTc = ReadStr("Controls", "ToggleTc", "");
    c.toggleTelemetry = ReadStr("Controls", "ToggleTelemetry", c.toggleTelemetry.c_str());
    for (int i = 0; i < 7; i++) c.seat[i] = ReadStr("Controls", kSeatKeys[i], "");
    c.wheelRotation = Clamp(ReadFloat("Controls", "WheelRotation", c.wheelRotation), 90.0f, 1800.0f);
    c.steeringLock = Clamp(ReadFloat("Controls", "SteeringLock", c.steeringLock), 30.0f, 1800.0f);
    c.steerDeadzone = Clamp(ReadFloat("Controls", "SteeringDeadzone", c.steerDeadzone), 0.0f, 0.5f);
    c.steerLinearity = Clamp(ReadFloat("Controls", "SteeringLinearity", c.steerLinearity), 0.5f, 3.0f);
    c.steerSpeedSensitivity = Clamp(ReadFloat("Controls", "SpeedSensitivity", c.steerSpeedSensitivity), 0.0f, 0.95f);
    if (ReadInt("Controls", "SpeedSensitiveSteering", 1) == 0) c.steerSpeedSensitivity = 0.0f;   // an older file's switch
    c.steerSensitivity = Clamp(ReadFloat("Controls", "SteeringSensitivity", c.steerSensitivity), 0.25f, 4.0f);
    c.pedalDeadzone = Clamp(ReadFloat("Controls", "PedalDeadzone", c.pedalDeadzone), 0.0f, 0.4f);
    c.gameController = ReadInt("Controls", "GameController", c.gameController) != 0;
    c.classicInput = ReadInt("Controls", "ClassicInput", 0) != 0;
    c.menuController = ReadInt("Controls", "MenuController", c.menuController) != 0;
    c.menuAxes = ReadInt("Controls", "MenuAxes", c.menuAxes) != 0;

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
    c.reselectAfterMiss = ReadInt("Shifter", "ReselectAfterMiss", 1) != 0;
    c.keepShiftDelay = ReadInt("Shifter", "KeepShiftDelay", 0) != 0;
    c.grind = ReadInt("Shifter", "Grind", 1) != 0;
    c.grindSound = (uint32_t)strtoul(ReadStr("Shifter", "GrindSound", "0xBC2DFA2F").c_str(), nullptr, 0);
    c.sequentialNeedsClutch = ReadInt("Shifter", "SequentialNeedsClutch", 0) != 0;
    c.misshiftChance = Clamp(ReadFloat("Shifter", "MisshiftChance", c.misshiftChance), 0.0f, 1.0f);
    c.misshiftDamage = Clamp(ReadFloat("Shifter", "MisshiftDamage", c.misshiftDamage), 0.0f, 1.0f);

    c.engineWear = ReadInt("Engine", "EngineWear", 1) != 0;
    c.revLimiter = ReadInt("Engine", "RevLimiter", 0) != 0;
    c.heatUp = Clamp(ReadFloat("Engine", "HeatUp", c.heatUp), 0.0f, 100.0f);
    c.heatUpMax = Clamp(ReadFloat("Engine", "HeatUpMaxRPM", c.heatUpMax), 0.0f, 100.0f);
    c.coolDown = Clamp(ReadFloat("Engine", "CoolDown", c.coolDown), 0.0f, 100.0f);
    c.powerLossStart = Clamp(ReadFloat("Engine", "PowerLossStart", c.powerLossStart), 0.0f, 0.99f);
    c.powerLoss = Clamp(ReadFloat("Engine", "PowerLoss", c.powerLoss), 0.0f, 1.0f);
    c.engineBrakeFade = Clamp(ReadFloat("Engine", "EngineBrakingFade", c.engineBrakeFade), 0.02f, 1.0f);
    c.totaled = ReadInt("Engine", "Totaled", 1) != 0;
    c.debugBlow = ReadStr("Engine", "DebugBlow", "");
    c.debugRepair = ReadStr("Engine", "DebugRepair", "");
    c.debugDamage = ReadStr("Damage", "DebugDamage", "");

    c.damage = ReadInt("Damage", "Damage", 1) != 0;
    c.damageMinImpact = Clamp(ReadFloat("Damage", "MinImpact", c.damageMinImpact), 0.0f, 100.0f);
    c.damageStrength = Clamp(ReadFloat("Damage", "Strength", c.damageStrength), 0.0f, 10.0f);
    c.damageSmoke = ReadInt("Damage", "Smoke", 1) != 0;
    c.noHeavySupport = ReadInt("Traffic", "NoHeavySupport", 1) != 0;   // replaces NoSuvCops (which also removed SUV patrols)
    c.normalDragRaces = ReadInt("Races", "NormalDragRaces", 1) != 0;
    c.normalDragHud = ReadInt("Races", "NormalDragHud", 1) != 0;
    c.gameTuning = ReadInt("Physics", "GameTuning", 1) != 0;
    c.heatSmokeFrom = Clamp(ReadFloat("Engine", "SmokeFrom", c.heatSmokeFrom), 0.1f, 1.0f);
    c.smokeTrail = Clamp(ReadFloat("Damage", "SmokeTrail", c.smokeTrail), 0.0f, 1.0f);
    c.smokeFrom = Clamp(ReadFloat("Damage", "SmokeFrom", c.smokeFrom), 0.5f, 1.0f);   // the game smokes at <= 50% health
    c.damageLossStart = Clamp(ReadFloat("Damage", "PowerLossStart", c.damageLossStart), 0.0f, 0.99f);
    c.damageLoss = Clamp(ReadFloat("Damage", "PowerLoss", c.damageLoss), 0.0f, 1.0f);

    c.poppedTires = ReadInt("Tires", "PoppedTires", 1) != 0;
    c.dropHeight = Clamp(ReadFloat("Tires", "DropHeight", c.dropHeight), 0.5f, 50.0f);
    c.tireHandicap = Clamp(ReadFloat("Tires", "Handicap", c.tireHandicap), 0.0f, 3.0f);
    c.debugPopTires = ReadStr("Tires", "DebugPopTires", "");

    c.hudMeter = ReadInt("HUD", "Meter", 1) != 0;
    c.speedbreaker = ReadInt("HUD", "Speedbreaker", 0) != 0;
    c.hudIcon = ReadStr("HUD", "Icon", c.hudIcon.c_str());
    c.hudHeatIcon = ReadStr("HUD", "HeatIcon", c.hudHeatIcon.c_str());
    c.hudAbsIcon = ReadStr("HUD", "AbsIcon", c.hudAbsIcon.c_str());
    c.hudTcIcon = ReadStr("HUD", "TcIcon", c.hudTcIcon.c_str());
    c.hideInfractions = ReadInt("HUD", "Infractions", 0) == 0;
    c.hideCostToState = ReadInt("HUD", "CostToState", 0) == 0;
    c.hideLocationNames = ReadInt("HUD", "LocationNames", 0) == 0;
    c.hudHeatIconFrom = Clamp(ReadFloat("HUD", "HeatIconFrom", c.hudHeatIconFrom), 0.0f, 1.0f);
    c.hudHeatFrom = Clamp(ReadFloat("HUD", "HeatFrom", c.hudHeatFrom), 0.0f, 0.95f);
    c.hudWarn = Clamp(ReadFloat("HUD", "Warn", c.hudWarn), 0.0f, 1.0f);
    c.hudIconShift = Clamp(ReadFloat("HUD", "IconShift", c.hudIconShift), -20.0f, 20.0f);
    c.hudAidRight = Clamp(ReadFloat("HUD", "AidIconRight", c.hudAidRight), -1000.0f, 1000.0f);
    c.hudAidUp = Clamp(ReadFloat("HUD", "AidIconUp", c.hudAidUp), -1000.0f, 1000.0f);
    c.hudDepth = ReadFloat("HUD", "Depth", c.hudDepth);
    c.hudFillUp = ReadInt("HUD", "FillUp", 1) != 0;

    c.physics = ReadInt("Physics", "Physics", 1) != 0;
    c.tractionControl = Clamp(ReadFloat("Physics", "TractionControl", c.tractionControl), 0.0f, 1.0f);
    c.stability = Clamp(ReadFloat("Physics", "Stability", c.stability), 0.0f, 1.0f);
    c.driftAssist = Clamp(ReadFloat("Physics", "DriftAssist", c.driftAssist), 0.0f, 1.0f);
    c.grip = Clamp(ReadFloat("Physics", "Grip", c.grip), 0.3f, 2.0f);
    c.gripBalance = Clamp(ReadFloat("Physics", "GripBalance", c.gripBalance), -0.5f, 0.5f);
    LegacyGripBalance("Physics", IniPath(), &c.grip, &c.gripBalance);
    c.abs = ReadInt("Physics", "ABS", c.abs) != 0;
    c.tc = ReadInt("Physics", "TC", 0) != 0;
    c.offroadGrip = Clamp(ReadFloat("Physics", "OffroadGrip", c.offroadGrip), 0.2f, 1.0f);
    c.rainGrip = Clamp(ReadFloat("Physics", "RainGrip", c.rainGrip), 0.5f, 1.0f);
    c.downforce = Clamp(ReadFloat("Physics", "Downforce", c.downforce), -0.25f, 2.0f);
    c.downforceMatch = Clamp(ReadFloat("Physics", "DownforceMatchSpeed", c.downforceMatch), 50.0f, 400.0f);
    c.logPhysics = ReadInt("Physics", "LogPhysics", 0);
    c.wallBounce = Clamp(ReadFloat("Physics", "WallBounce", c.wallBounce), 0.0f, 1.0f);
    c.loadSensitivity = Clamp(ReadFloat("Physics", "LoadSensitivity", c.loadSensitivity), 0.0f, 0.5f);
    c.tyreSlideGrip = Clamp(ReadFloat("Physics", "TyreSlideGrip", c.tyreSlideGrip), 0.5f, 1.0f);
    c.tyrePeakAngle = Clamp(ReadFloat("Physics", "TyrePeakAngle", c.tyrePeakAngle), 3.0f, 20.0f);
    c.suspFirmness = Clamp(ReadFloat("Physics", "SuspensionFirmness", c.suspFirmness), 0.5f, 2.0f);
    c.rollBalance = Clamp(ReadFloat("Physics", "RollBalance", c.rollBalance), -0.5f, 0.5f);
    c.character = Clamp(ReadFloat("Physics", "Character", c.character), 0.0f, 1.0f);
    c.realWeight = ReadInt("Physics", "RealWeight", c.realWeight > 0.5f) != 0 ? 1.0f : 0.0f;
    c.slideWheelspin = Clamp(ReadFloat("Physics", "WheelspinInSlides", c.slideWheelspin), 0.0f, 1.0f);
    c.frictionCircle = Clamp(ReadFloat("Physics", "FrictionCircle", c.frictionCircle), 0.0f, 3.0f);
    c.gravity = Clamp(ReadFloat("Physics", "Gravity", c.gravity), 0.1f, 3.0f);
    c.gravityAll = ReadInt("Physics", "GravityAllCars", 0) != 0;
    c.maxSpin = Clamp(ReadFloat("Physics", "MaxSpin", c.maxSpin), 0.0f, 30.0f);

    c.telemetry = ReadInt("Telemetry", "Show", c.telemetry) != 0;
    c.telX = Clamp(ReadFloat("Telemetry", "X", c.telX), 0.0f, 1.0f);
    c.telY = Clamp(ReadFloat("Telemetry", "Y", c.telY), 0.0f, 1.0f);
    c.telSize = Clamp(ReadFloat("Telemetry", "Size", c.telSize), 0.3f, 2.5f);
    c.telOpacity = Clamp(ReadFloat("Telemetry", "Opacity", c.telOpacity), 0.1f, 1.0f);
    c.telSmooth = Clamp(ReadFloat("Telemetry", "Smoothing", c.telSmooth), 0.0f, 1.0f);
    c.telTrail = Clamp(ReadFloat("Telemetry", "Trail", c.telTrail), 0.0f, 2.0f);
    c.telShowG = ReadInt("Telemetry", "GForce", c.telShowG) != 0;
    c.telShowTyres = ReadInt("Telemetry", "Tyres", c.telShowTyres) != 0;
    c.telShowSusp = ReadInt("Telemetry", "Suspension", c.telShowSusp) != 0;
    c.telShowAero = ReadInt("Telemetry", "Downforce", c.telShowAero) != 0;

    c.gauge = ReadInt("UI", "Gauge", 0) != 0;
    c.gaugeX = Clamp(ReadFloat("UI", "GaugeX", c.gaugeX), 0.0f, 1.0f);
    c.gaugeY = Clamp(ReadFloat("UI", "GaugeY", c.gaugeY), 0.0f, 1.0f);
    c.uiScale = Clamp(ReadFloat("UI", "Scale", c.uiScale), 0.5f, 3.0f);
    c.classicMenu = ReadInt("UI", "ClassicMenu", 0) != 0;
    c.menu = ReadStr("UI", "Menu", c.menu.c_str());
    c.menuUp = ReadStr("UI", "MenuUp", c.menuUp.c_str());
    c.menuDown = ReadStr("UI", "MenuDown", c.menuDown.c_str());
    c.menuLeft = ReadStr("UI", "MenuLeft", c.menuLeft.c_str());
    c.menuRight = ReadStr("UI", "MenuRight", c.menuRight.c_str());
}
