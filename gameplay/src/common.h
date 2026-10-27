// Shared declarations for the NFSMW H-pattern shifter + clutch mod.
#pragma once

// The release this build comes from, set by the build script (git describe).
#ifndef MOD_VERSION
#define MOD_VERSION "dev"
#endif

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
// 0 at or below a, 1 at or above b, a smooth S-curve between.
static inline float Smoothstep(float a, float b, float x) {
    float t = Clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

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
    float biteStart = 0.7f;        // pedal travel where the clutch starts to grip (releasing)
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
    float steerSpeedSensitivity = 0.0f; // bound wheel: the share of the front wheel lock taken away at high
                                    // speed; 0 = direct steering (full lock at any speed, no steering ramp)
    float pedalDeadzone = 0.03f;    // ignored travel at each end of the pedals
    bool  gameController = true;    // the game's own controller input (driving and menus); keyboard always works
    bool  classicInput = false;     // the game's own steering, throttle and brake, untouched (no bindings, curves or wheel steering)
    bool  menuController = true;    // controller buttons and hats in the game's menus (needs FFB mod)
    bool  menuAxes = false;         // analog axes in menus; off prevents pedal scrolling
    std::string recenterVr;         // recentres the VR view (the VR mod's recentre key), also outside the F4 menu
    std::string toggleAbs;          // [Controls] ToggleAbs: switches ABS on / off while driving
    std::string toggleTc;           // [Controls] ToggleTc: switches traction control on / off while driving
    std::string toggleTelemetry = "key:RSHIFT";  // [Controls] ToggleTelemetry: shows / hides the telemetry panel
    std::string seat[7];            // VR seat (the cockpit camera), held: Seat order (input.h)

    // [Shifter]
    std::string gear[kMaxGears + 1];// [0] = reverse, [1..7] = gears 1-7
    std::string neutral;            // only used with Latching=1
    bool  latching = false;         // gear stays selected after the button is released
    bool  sequential = false;       // sequential shifting (ShiftUp / ShiftDown) instead of the H-pattern
    std::string shiftUp = "key:LSHIFT";
    std::string shiftDown = "key:LCTRL";
    bool  requireClutch = true;     // need the clutch pressed to put it in gear
    float shiftPedal = 0.6f;       // pedal travel that counts as "clutch pressed"
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
    float engineBrakeFade = 0.1f;   // throttle at which the player's engine braking is gone (1 = the game's)
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
    bool  noHeavySupport = true;    // no heavy support: the ramming SUVs sent head-on at you (SUV patrols stay)
    bool  normalDragRaces = true;   // the player drives drag races normally (no auto lane steering, normal engine and damage)
    bool  gameTuning = true;        // the pause menu's Performance Tuning acts on the player's car (as in the game)
    bool  normalDragHud = true;     // drag races show the normal race HUD, not the drag HUD (from the next race)
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
    bool  hideInfractions = true;   // no infractions popup on the HUD
    bool  hideCostToState = true;   // no cost to state popup on the HUD
    bool  hideLocationNames = true; // no area name ("Ocean View") on entering a neighbourhood
    float hudHeatIconFrom = 0.5f;   // engine heat from which the engine warning icon flashes
    std::string hudAbsIcon = "PERFORMANCE_BRAKES";       // ABS icon above the map: first loaded texture of these
    std::string hudTcIcon = "MAIN_ICON_QUICK_RACE,MODE_ICON_QUICK_RACE";   // TC icon
    std::string hudHeatIcon = "ENGINE_HEAT_ICON,HEAT_ICON"; // engine warning icon: first loaded texture of these
    float hudHeatFrom = 0.5f;       // heat where the red overlay starts filling
    float hudWarn = 0.75f;          // heat or damage at which the meter and car icon flash
    bool  hudFillUp = true;         // the meter fills from the bottom up (the game's arc fills downwards)
    float hudIconShift = 2.0f;      // the car icon (and the spike / engine icons with it) moved right, HUD units
    float hudAidRight = 0, hudAidUp = 0; // the ABS and TC icons moved right / up from their place (HUD units, 640 wide)
    float hudDepth = -1.0f;         // overlay z offset from the arc; - is nearer (in front)

    // [Physics] the player car's driving aids, grip and gravity (1 = the game's own). A car with
    // its own [Car.<name>] section uses the values there (see physics.h).
    bool  physics = true;
    float tractionControl = 0.0f;  // traction control strength
    float stability = 0.0f;        // yaw control and drift stabiliser strength
    float driftAssist = 0.0f;       // how much rear grip is lowered in a slide
    float grip = 0.75f;             // overall tire grip
    float character = 0.5f;         // each car's own springs, bars, grip, aero and torque spread from the others' (cars.h)
    bool  realWeight = true;        // the real car's front / rear weight instead of the game's 51-55% front
    float gripBalance = 0.0f;       // front tyres' grip 1 + this, rear 1 - this (above 0: oversteer)
    bool  abs = true;               // ABS: a locking wheel's brake is eased off and reapplied
    bool  tc = false;               // traction control: 100% the game's own, plus throttle trimmed to the
                                    // driven wheels' slip, with launch control from a standstill
    float offroadGrip = 0.95f;      // tyre grip on grass, dirt, mud, gravel, sand
    float rainGrip = 0.9f;          // tyre grip in the rain
    float downforce = 0.7f;         // times the game's own aerodynamic downforce (below 0: lift), per car
    float downforceMatch = 250.0f;  // km/h where the squared downforce equals the game's (times Downforce)
    float loadSensitivity = 0.2f;   // a tyre's grip limit grows with load^(1 - this), around its static load (0 = the game's)
    float tyreSlideGrip = 0.85f;    // side grip of a tyre sliding well past its peak, compared with the peak (1 = the game's tyres), per car
    float tyrePeakAngle = 8.0f;     // slip angle (degrees) where the side grip peaks; it falls to TyreSlideGrip by 3 times it
    float rollBalance = 0.0f;       // anti-roll bar stiffness moved to the front (+, understeer) or rear (-), per car
    float suspFirmness = 1.0f;      // springs and anti-roll bars times this, dampers by its square root (per car)
    float wallBounce = 0.0f;        // share of the game's wall rebound the player's car keeps (0 = none)
    int   logPhysics = 0;           // 1: a physics line every second while driving (aero, grip factors)
    float slideWheelspin = 0.5f;    // driven wheels past the limit spin up: the game's tie to the road ^(1 + 15 * this) (0 = the game's)
    float frictionCircle = 1.0f;    // a driven tyre's side force kept within the grip its drive torque leaves (0 = off, above 1 exaggerated)
    float gravity = 1.0f;
    bool  gravityAll = false;       // gravity applies to every car, not only the player's
    float maxSpin = 8.0f;           // the player's car never spins faster (rad/s); 0 = the game's 30

    // [Telemetry] the panel of forces, tyres, suspension and downforce while driving (telemetry.h)
    bool  telemetry = false;        // shown ([Controls] ToggleTelemetry switches it)
    float telX = 0.13f;             // the panel's centre, fractions of the screen from the left
    float telY = 0.42f;             // ... and from the top
    float telSize = 0.8f;
    float telOpacity = 1.0f;
    float telSmooth = 0.12f;        // the needles' time constant (s)
    float telTrail = 0.5f;          // how long the G-force dot's trail is (s)
    bool  telShowG = true;          // blocks: the G-force circle,
    bool  telShowTyres = true;      // the tyres' grip,
    bool  telShowSusp = true;       // the suspension's travel,
    bool  telShowAero = true;       // and the downforce

    // [UI] heat gauge and tuning menu, drawn with the HUD (on the VR mod's HUD panel in VR)
    bool  gauge = false;            // placeholder look, off until it matches the game's HUD
    float gaugeX = 0.85f;           // gauge centre, fraction of the screen
    float gaugeY = 0.64f;
    float uiScale = 1.0f;
    bool  classicMenu = false;      // the old flat menu instead of the ImGui one
    std::string menu = "key:F4";    // opens / closes the menu
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
// Grip and GripBalance from an older file's FrontGrip and RearGrip in [section] (each was times
// Grip): the same front and rear grip. False when the section has GripBalance or neither key.
bool LegacyGripBalance(const char* section, const char* path, float* grip, float* balance);
