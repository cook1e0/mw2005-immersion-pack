// Shared declarations for the NFSMW H-pattern shifter + clutch mod.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Logging (nfsmw_hshifter.log next to speed.exe)
// ---------------------------------------------------------------------------
void LogInit();
void Log(const char* fmt, ...);

// Directory containing the host exe (with trailing backslash).
const char* GameDir();

// Seconds since an arbitrary epoch (QPC).
double Now();

template <class T> static inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Configuration (nfsmw_hshifter.ini next to speed.exe)
// ---------------------------------------------------------------------------
constexpr int kMaxJoys = 8;
constexpr int kMaxGears = 7;    // forward gears on the shifter

struct Config {
    bool enabled = true;

    // [Devices] JoyN = name substring; empty = Nth attached game controller
    std::string joyName[kMaxJoys];

    // [Clutch]
    std::string clutch;             // bindings, comma separated (axis, buttons or keys)
    int   clutchReleased = -1;      // raw axis value with the pedal up   (-1 = learn at startup)
    int   clutchPressed = -1;       // raw axis value with the pedal floored (-1 = opposite end)
    float deadzone = 0.04f;         // ignored travel at each end of the pedal
    float biteStart = 0.70f;        // pedal travel where the clutch starts to grip (releasing)
    float biteEnd = 0.20f;          // pedal travel where it grips fully
    float strength = 1.0f;          // clutch friction relative to the game's own clutch
    float lockRpm = 150.0f;         // slip below which a released clutch locks up
    float buttonPressTime = 0.08f;  // digital clutch (button/key): seconds to floor it
    float buttonReleaseTime = 0.45f;// ... and to let it out

    // [Shifter]
    std::string gear[kMaxGears + 1];// [0] = reverse, [1..7] = gears 1-7
    std::string neutral;            // only used with Latching=1
    bool  latching = false;         // gear stays selected after the button is released
    bool  requireClutch = true;     // need the clutch pressed to put it in gear
    float shiftPedal = 0.60f;       // pedal travel that counts as "clutch pressed"
    float revMatchRpm = 0.0f;       // >0: allow clutchless shifts within this many rpm
    bool  blockWrongDirection = true; // no reverse while rolling forward (and vice versa)
    bool  keepShiftDelay = false;   // keep the game's shift delay even when the clutch was used
    bool  sequentialNeedsClutch = false; // without the H-shifter: shift up/down only with the clutch in

    // [Engine] heat, over-rev and blow-ups (the drag race engine model, in every mode)
    bool  engineWear = true;
    bool  revLimiter = false;       // keep the rev limiter (heat then builds while bouncing off it)
    float heatUp = 0.20f;           // heat per second at the redline (1 = blown)
    float heatUpMax = 0.50f;        // ... at max rpm (over-revving without the limiter)
    float coolDown = 0.10f;         // heat lost per second below the redline
    float powerLossStart = 0.5f;    // heat where the engine starts losing power
    float powerLoss = 0.5f;         // fraction of torque lost at full heat
    bool  totaled = true;           // a blown engine totals the car (crash camera, "Totaled")
    std::string debugBlow;          // binding that blows the engine (testing)
    std::string debugRepair;        // binding that repairs it (testing)

    // [UI] heat gauge and tuning menu, drawn with the HUD (on the VR mod's HUD panel in VR)
    bool  gauge = true;
    float gaugeX = 0.85f;           // gauge centre, fraction of the screen
    float gaugeY = 0.64f;
    float uiScale = 1.0f;
    std::string menu = "key:F4";    // opens / closes the tuning menu
    std::string menuUp = "key:UP,key:NUMPAD8";
    std::string menuDown = "key:DOWN,key:NUMPAD2";
    std::string menuLeft = "key:LEFT,key:NUMPAD4";
    std::string menuRight = "key:RIGHT,key:NUMPAD6";

    // [General]
    std::string toggle;             // binding that switches between H-pattern and the game's sequential shifting
    bool  logInputs = false;
};
extern Config g_cfg;
void LoadConfig();
const char* IniPath();
