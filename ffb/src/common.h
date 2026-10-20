// Shared declarations for the NFSMW force feedback mod.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Logging (nfsmw_ffb.log next to speed.exe)
// ---------------------------------------------------------------------------
void LogInit();
void Log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Configuration (nfsmw_ffb.ini next to speed.exe)
// ---------------------------------------------------------------------------
struct Config {
    bool  enabled = true;           // false = pure passthrough (the game's own FFB untouched)
    bool  nativeFfb = false;        // true = let the game's own effects through, no forces from this mod

    // Output and safety
    float gain = 1.0f;              // master strength
    float maxForce = 0.8f;          // ceiling of the torque sent, share of the wheel's maximum
    float limitKnee = 0.7f;         // above this share of MaxForce the soft limiter compresses peaks
    float slewRate = 20.0f;         // fastest torque change, full scale per second
    float smoothingHz = 25.0f;      // low-pass of the forces from the game's physics (about 60 Hz steps)
    bool  oscillationGuard = true;  // calm the wheel when it starts shaking by itself

    // Steering forces
    float aligning = 0.8f;          // the front tyres' self-aligning torque
    float gripPeakDeg = 7.0f;       // front slip angle where the wheel is heaviest
    float driftAlign = 0.3f;        // pull toward the direction of travel past the grip limit
    float loadSensitivity = 0.5f;   // how much front tyre load changes the aligning torque
    float centering = 0.3f;         // spring toward centre at CenteringSpeed and above
    float centeringParked = 0.25f;  // share of it at standstill
    float centeringSpeed = 120.0f;  // km/h
    float damping = 0.3f;           // weight against fast wheel movement
    float friction = 0.05f;
    float parkingFriction = 0.15f;  // extra friction near standstill (tyre scrub)
    float softLock = 0.8f;          // end stop at the game's steering lock
    float menuCentering = 0.2f;     // spring in menus, pause and cutscenes
    float centreWeight = 0.3f;      // firm hold around straight ahead while moving

    // Effects
    float engineRumble = 0.05f;     // engine vibration (mostly standing still)
    float kerbs = 0.5f;             // kicks and shakes from kerbs and bumps
    float surfaceRumble = 0.3f;     // the game's rumble on grass, gravel, dirt, rumble strips
    float crashes = 0.6f;           // jolts from hitting cars, walls and props
    float scrapes = 0.4f;           // grinding along walls
    float breakables = 0.6f;        // knocks from smashing breakable props (benches, sign posts...)
    float landing = 0.8f;           // kick and shake when landing after a jump
    float carHits = 0.8f;           // the same when hitting another vehicle
    float offroadVibration = 0.4f;  // rumble on grass, dirt, mud, gravel
    float nosLighter = 0.3f;        // steering this much lighter while the nitrous is on (0..0.9)

    // Wheel
    float device = 0;               // FFB device by Windows' order (joy.cpl), -1 = the first the game opens
                                    // (a float: live from the F4 menu)
    int   forceSign = 1;            // 1 = a positive force turns the wheel right (DirectInput), -1 = left
    float reverse = 0.0f;           // 1 = flip every force (F4's Reverse force direction, live)
    int   forceDirection = 0;       // a force to the left as 0 = negative magnitude, 1 = negative direction
    bool  restartEffects = true;    // send every change with DIEP_START (Fanatec ignores changes otherwise)
    int   steerAxis = 0;            // 0=X 1=Y 2=Z 3=RX 4=RY 5=RZ 6=Slider0
    int   updateHz = 400;

    // Shared with the gameplay mod (live from its F4 menu and [Controls])
    float gameController = 1.0f;    // 0 = the game reads controllers as untouched
    float menuController = 1.0f;    // controller buttons and hats in the game's menus
    float menuAxes = 0.0f;          // analog axes in menus (off: pedals cannot scroll)
    float softLockAt = 1.0f;        // wheel position (share of rotation) of the steering lock, 1 = off

    int   keyReset = VK_F10;        // restart the wheel's force without restarting the game; 0 = off
    bool  logTelemetry = false;
};
extern Config g_cfg;
void LoadConfig();

// Directory containing speed.exe (with trailing backslash).
const char* GameDir();
// Folder of this DLL and the mods' .ini files (see common.cpp).
const char* ModDir();
void ConfigFile(const char* name, char* out, bool migrate);

// Seconds since an arbitrary epoch (QPC).
double Now();

template <class T> static inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float Smoothstep(float a, float b, float x) {
    float t = Clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline float Sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
