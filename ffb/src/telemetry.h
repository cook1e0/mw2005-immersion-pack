// Player car telemetry sampled from speed.exe v1.3 once per simulation tick.
#pragma once
#include "common.h"

struct Telemetry {
    uint32_t seq;          // increments every sample
    double   time;         // Now() at sample
    float    dt;           // seconds since previous sample
    bool     driving;      // player has control (not paused / AI / cutscene)
    bool     valid;        // vehicle data below is meaningful

    float speed;           // m/s, horizontal-ish body speed
    float fwdSpeed;        // m/s along car forward (negative = reversing)
    float yawRate;         // rad/s, positive = turning toward the "right guess" vector

    // Front axle (wheels 0 and 1). Wheel 1 sits on the +rightGuess side.
    float slipAngle[2];    // game tyre slip angle (rad, game sign convention)
    float load[2];         // tyre normal load
    float compVel[2];      // suspension compression rate (units/s)
    bool  onGround[2];
    bool  brakeLocked[2];
    float steer;           // average front road-wheel angle (rad, game sign)
    float maxSteer;        // game's current steering lock (deg)
    float rearSlipAngle;   // average rear tyre slip angle (rad, game sign)
    int   wheelsOnGround;

    float rpmNorm;         // 0 idle .. 1 redline
    float rpm;

    // Body motion along the car's own up axis (so slopes and hills don't count)
    float vertVel;         // m/s
    float vertAccel;       // m/s^2, smoothed over a few ticks
    float vertJerk;        // m/s^3, change of vertAccel
    float compAccel[2];    // front suspension compression acceleration (units/s^2)
    float wheelbase;       // m, front to rear axle

    // Road surface under each front tyre (the game's simsurface attributes)
    float surfaceRumble[2];   // WheelEffectIntensity / 255 (the game's own per-surface rumble)
    float surfaceHz[2];       // WheelEffectFrequency
    float rollingResist[2];   // ROLLING_RESISTANCE (high on grass, dirt)

    // Airborne: no wheel on the ground. Landings counted by seq with the speed they hit at.
    bool     airborne;
    float    airTime;      // s since the last wheel left the ground
    uint32_t landSeq;
    float    landSpeed;    // m/s downward along the car's up axis just before touching down

    // Collision impulses (accumulated by the game thread, consumed by FFB thread by seq)
    uint32_t hitSeq;
    float    hitMag;       // m/s velocity change
    float    hitLat;       // lateral share of the velocity change (-1..1, +rightGuess)
    float    hitLon;       // forward share of the velocity change (-1..1, + = pushed forward)
    // Lighter contacts (wall scrapes, taps): acceleration spikes above the smoothed acceleration
    uint32_t scrapeSeq;
    float    scrapeLat;    // lateral share of that spike (-1..1, +rightGuess)
    float    scrapeMag;    // m/s^2 above the smoothed acceleration
};

bool Telemetry_Install();
// Copy of the most recent sample. Thread-safe.
void Telemetry_Get(Telemetry* out);
