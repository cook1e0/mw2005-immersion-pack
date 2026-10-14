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
    bool  enabled = true;           // false = pure passthrough (game's own FFB untouched)
    bool  nativeFfb = false;        // true = let the game's own effects through, disable ours
    float gain = 1.0f;              // master strength
    float satGain = 0.75f;          // self-aligning torque (main steering weight)
    float satPeakAngle = 6.0f;      // front slip angle (deg) where aligning torque peaks
    float driftAlign = 0.25f;       // residual pull toward the direction of travel while sliding
    float damperLow = 0.35f;        // damper at standstill
    float damperHigh = 0.12f;       // damper at speed
    float friction = 0.06f;         // constant friction
    float standstillFriction = 0.35f; // extra friction (tyre scrub) when parked
    float roadGain = 0.5f;          // kicks from front suspension movement (bumps, kerbs)
    float collisionGain = 0.8f;     // jolt on impacts
    float engineVibration = 0.02f;  // idle/engine buzz
    float brakeLockVibration = 0.12f;
    float menuSpring = 0.25f;       // centring spring when not driving (menus, pause, cutscenes)
    float centerSpring = 0.5f;      // centring spring while driving at CenterSpringSpeed and above
    float centerSpringParked = 0.3f; // fraction of it at standstill (rises linearly with speed)
    float centerSpringSpeed = 150.0f; // km/h where the centring spring reaches full strength
    float softLock = 0.8f;          // push-back past the steering lock
    // Synthetic effects on top of the tyre model
    float speedCentering = 0.35f;   // aligning torque -K * v * sin(steering angle), at SpeedCenteringRef
    float speedCenteringRef = 120.0f; // km/h where it reaches its full strength (keeps growing to 1.5x)
    float understeerLightening = 0.5f; // steering force lost when the front tyres are past their grip
    float surfaceRumble = 0.35f;    // rumble on rough surfaces (grass, dirt, gravel), the game's own per surface
    float curbs = 0.5f;             // kicks from kerbs and rumble strips (body jerk in the car's frame)
    float curbJerk = 300.0f;        // jerk (m/s^3) that counts as a kerb strike; crests and slopes stay below
    float scrapes = 0.4f;           // wall scrapes and taps: pull toward the contact and a fast rumble
    float landing = 0.6f;           // jolt on landing a jump (the wheel goes limp in the air)
    float airborneForce = 0.1f;     // share of the steering forces left while airborne
    int   forceDirection = 0;       // how a negative force is sent: 0 = negative magnitude, 1 = negative
                                    // direction (for drivers that ignore negative magnitudes)
    float gameController = 1.0f;    // 0 = the game reads controllers as untouched (gameplay mod's
                                    // [Controls] GameController; live from its F4 menu)
    float menuController = 1.0f;    // controller buttons and hats in the game's menus
    float menuAxes = 0.0f;          // analog axes in menus (off: pedals cannot scroll)
    float softLockAt = 1.0f;        // wheel position (fraction of full travel) of the steering lock;
                                    // set by the gameplay mod's [Controls] SteeringLock, 1 = off
    float minForce = 0.0f;          // wheel deadband compensation (0..0.2)
    float invertForce = 0.0f;       // 1 = flip constant-force direction (a 0/1 float: live from the F4 menu)
    int   steerAxis = 0;            // 0=X 1=Y 2=Z 3=RX 4=RY 5=RZ 6=Slider0
    float device = 0;               // FFB device by Windows' order (joy.cpl), -1 = the first the game opens
                                    // (a float: live from the F4 menu)
    int   updateHz = 200;
    int   keyReset = VK_F10;        // restart mod effects without restarting the game; 0 = off
    // [Calibration] - written by the mod, 0 = unknown (re-detect)
    int   forceSign = 0;            // constant-force sign that turns the wheel right
    int   slipSign = 0;             // game slip-angle sign vs. physical right
    int   rightSign = 0;            // game body "right guess" vs. physical right
    bool  logTelemetry = false;
};
extern Config g_cfg;
void LoadConfig();
void SaveCalibration(int slipSign, int rightSign, int forceSign);
void SaveForceDirection(int mode);

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
