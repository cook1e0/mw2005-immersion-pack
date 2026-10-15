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
// Logging (nfsmw_gameplay.log next to speed.exe)
// ---------------------------------------------------------------------------
void LogInit();
void Log(const char* fmt, ...);

// Directory containing the host exe (with trailing backslash).
const char* GameDir();
// Folder of this DLL and the mods' .ini files (see common.cpp).
const char* ModDir();
void ConfigFile(const char* name, char* out, bool migrate);

// Seconds since an arbitrary epoch (QPC).
double Now();

template <class T> static inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Configuration (nfsmw_gameplay.ini next to speed.exe)
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

    // [Controls] driving controls read by this mod and fed to the game as its own actions, so the
    // game's controller settings aren't needed (controls.cpp)
    std::string steer;              // steering axis
    std::string throttle, brake;    // pedal axes, buttons or keys
    int   throttleRest = -1;        // raw axis value with the pedal up (-1 = the end opposite its travel)
    int   brakeRest = -1;
    std::string button[6];          // Button order (input.h): handbrake, nitrous, speedbreaker, reset, camera, look back
    float wheelRotation = 900.0f;   // the wheel's rotation set in its driver, degrees lock to lock
    float steeringLock = 540.0f;    // wheel rotation (lock to lock) that gives full steering in game
    float steerDeadzone = 0.0f;     // ignored travel around the centre, fraction of full steering
    float steerLinearity = 1.0f;    // steering response curve exponent: 1 = linear, >1 = finer near the centre
    float steerSensitivity = 1.0f;  // multiplies the steering: 2 = full lock at half the SteeringLock
    bool  steerSpeedSensitive = false; // bound wheel: less front wheel lock the faster you go
    float steerSpeedSensitivity = 0.85f; // how much less: the share of the lock taken away at high speed
                                    // instead of direct steering (full lock at any speed, no steering ramp)
    float pedalDeadzone = 0.03f;    // ignored travel at each end of the pedals
    bool  gameController = true;    // the game's own controller input (driving and menus); keyboard always works
    bool  menuController = true;    // controller buttons and hats in the game's menus (needs FFB mod)
    bool  menuAxes = false;         // analog axes in menus; off prevents pedal scrolling
    std::string recenterVr;         // recentres the VR view (the VR mod's recentre key), also outside the F4 menu
    std::string seat[7];            // VR seat (the cockpit camera), held: Seat order (input.h)

    // [Shifter]
    std::string gear[kMaxGears + 1];// [0] = reverse, [1..7] = gears 1-7
    std::string neutral;            // only used with Latching=1
    bool  latching = false;         // gear stays selected after the button is released
    bool  sequential = false;       // sequential shifting (ShiftUp / ShiftDown) instead of the H-pattern
    std::string shiftUp = "key:LSHIFT";
    std::string shiftDown = "key:LCTRL";
    bool  requireClutch = true;     // need the clutch pressed to put it in gear
    float shiftPedal = 0.60f;       // pedal travel that counts as "clutch pressed"
    float revMatchRpm = 0.0f;       // >0: allow clutchless shifts within this many rpm
    bool  blockWrongDirection = true; // no reverse while rolling forward (and vice versa)
    bool  reselectAfterMiss = true; // a gear refused for lack of clutch needs the stick out of its gate and back
    bool  keepShiftDelay = false;   // keep the game's shift delay even when the clutch was used
    bool  grind = true;             // a refused shift (no clutch) crunches like a missed drag race shift
    uint32_t grindSound = 0xBC2DFA2F; // ... with this sound moment (the totaled car's engine failure), 0 = none
    bool  sequentialNeedsClutch = false; // without the H-shifter: shift up/down only with the clutch in
    float misshiftChance = 0.5f;    // chance that a shift refused for lack of clutch damages the car
    float misshiftDamage = 0.05f;   // ... and how much car damage it does (needs [Damage] Damage=1)

    // [Engine] heat, over-rev and blow-ups (the drag race engine model, in every mode)
    bool  engineWear = true;
    bool  revLimiter = false;       // keep the rev limiter (heat then builds while bouncing off it)
    float heatUp = 0.05f;           // heat per second at the redline (1 = blown)
    float heatUpMax = 0.05f;        // ... at max rpm (over-revving without the limiter)
    float coolDown = 0.01f;         // heat lost per second below the redline
    float powerLossStart = 0.9f;    // heat where the engine starts losing power
    float powerLoss = 0.5f;         // fraction of torque lost at full heat
    bool  totaled = true;           // a blown engine totals the car (crash camera, "Totaled")
    std::string debugBlow;          // binding that blows the engine (testing)
    std::string debugRepair;        // binding that repairs it (testing)
    std::string debugDamage;        // binding that adds 10% car damage (testing)

    // [Damage] collision damage for the player's car; 100% totals it like a blown engine
    bool  damage = true;
    float damageMinImpact = 8.0f;   // impacts below this speed change (m/s) do no damage
    float damageStrength = 1.0f;    // damage per m/s above that, in % (1.0 = 1% per m/s)
    bool  damageSmoke = true;       // engine smoke (the cop cars' damage smoke) on a damaged car
    float smokeFrom = 0.75f;        // ... from this much damage
    bool  noSuvCops = true;         // no SUV cops (heavy support, SUV patrols, roadblocks)
    bool  normalDragRaces = true;   // the player drives drag races normally (no auto lane steering, normal engine and damage)
    float heatSmokeFrom = 0.5f;     // engine smoke from this much engine heat (1 = never)
    float smokeTrail = 0.8f;        // fraction of the car's velocity its smoke particles keep
    float damageLossStart = 0.3f;   // damage where the engine starts losing power
    float damageLoss = 0.4f;        // fraction of torque lost at full damage

    // [Tires] spike strips pop tires like in NFS Carbon: the car stays drivable, with less grip
    // and some drag, instead of the game's locked-up wheels
    bool  poppedTires = true;
    float dropHeight = 5.0f;        // [Tires] DropHeight: testing, how high "Drop from air" lifts the car (m)
    float tireHandicap = 0.35f;     // multiplies the grip loss and drag of a popped tire
    std::string debugPopTires;      // binding that pops all four tires (testing)

    // [HUD] the speedbreaker meter shows car damage, with engine heat overlaid in red
    bool  hudMeter = true;
    bool  speedbreaker = false;     // the game's speedbreaker (its meter is taken over)
    std::string hudIcon = "INGAME_ICON_PLAYERCAR"; // texture replacing the speedbreaker clock
    float hudHeatFrom = 0.5f;       // heat where the red overlay starts filling
    float hudWarn = 0.75f;          // heat or damage at which the meter and car icon flash
    bool  hudFillUp = true;         // the meter fills from the bottom up (the game's arc fills downwards)
    float hudDepth = -1.0f;         // overlay z offset from the arc; - is nearer (in front)

    // [Physics] the player car's driving aids, grip and gravity (1 = the game's own). A car with
    // its own [Car.<name>] section uses the values there (see physics.h).
    bool  physics = true;
    float tractionControl = 0.25f;  // traction control strength
    float stability = 0.25f;        // yaw control and drift stabiliser strength
    float driftAssist = 0.5f;       // how much rear grip is lowered in a slide
    float grip = 1.0f;              // overall tire grip
    float torqueSplit = 1.0f;       // multiplies the share of torque sent to the front wheels
    float drivetrain = 1.0f;        // exaggerates the driven axle's character (RWD oversteer, FWD understeer)
    float gravity = 1.0f;
    bool  gravityAll = false;       // gravity applies to every car, not only the player's

    // [UI] heat gauge and tuning menu, drawn with the HUD (on the VR mod's HUD panel in VR)
    bool  gauge = false;            // placeholder look, off until it matches the game's HUD
    float gaugeX = 0.85f;           // gauge centre, fraction of the screen
    float gaugeY = 0.64f;
    float uiScale = 1.0f;
    bool  classicMenu = false;      // the old flat menu instead of the ImGui one
    std::string menu = "key:F4";    // opens / closes the tuning menu
    std::string menuUp = "key:UP,key:NUMPAD8";
    std::string menuDown = "key:DOWN,key:NUMPAD2";
    std::string menuLeft = "key:LEFT,key:NUMPAD4";
    std::string menuRight = "key:RIGHT,key:NUMPAD6";

    // [General]
    std::string toggle;             // binding that switches between H-pattern and sequential shifting
    bool  logInputs = false;
};
extern Config g_cfg;
// [Controls] ini keys of Config::button.
extern const char* const kButtonKeys[6];
// [Controls] ini keys of Config::seat.
extern const char* const kSeatKeys[7];
void LoadConfig();
const char* IniPath();
