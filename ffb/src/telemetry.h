// The player car's state for force feedback, sampled from speed.exe v1.3 once per physics tick
// (about 60 Hz) on the game thread and read by the FFB thread.
//
// Everything is in the car's own frame, + = right / forward / up. "Right" is the car's forward
// direction (rear axle -> front axle) crossed with the world's up axis: NFSMW's world is
// right-handed and Z-up (checked against live camera matrices for the VR mod), so no sign needs
// calibrating against the wheel.
#pragma once
#include "common.h"

struct Telemetry {
    uint32_t seq;          // increments every sample
    double   time;         // Now() at sample
    bool     driving;      // the player controls the car (not paused, menus, AI or cutscene)
    bool     valid;        // the car data below is meaningful

    float speed;           // m/s
    float vLong;           // m/s along the car (negative = reversing)
    float vLatFront;       // m/s sideways at the front axle (+ = moving right)
    float vLatRear;        // m/s sideways at the rear axle
    float yawRate;         // rad/s (+ = turning right)
    float wheelbase;       // m

    float frontLoad;       // N, both front tyres
    int   frontOnGround;   // front wheels touching the ground (0..2)
    int   wheelsOnGround;  // 0..4
    float maxSteerDeg;     // the game's current front wheel lock, degrees (0 = unknown)
    float steerDeg;        // the front wheels' angle now, degrees (either sign convention)

    float rpm;             // engine
    float rpmNorm;         // 0 idle .. 1 redline
    bool  nos;             // nitrous engaged (IEngine::IsNOSEngaged)
    // The driven wheels' slip: their surface speed (EngineRacer::GetDifferentialAngularVelocity
    // times the wheel radius, learnt while cruising) over the car's speed, minus 1 (0 = gripping,
    // 1 = the wheels turn twice as fast as the car moves). 0 when unknown.
    float wheelSpin;

    // Front suspension, [0] = left, [1] = right: compression speed with the slow part (body roll,
    // pitch, hills) taken out, m/s, + = compressing. Kerbs and bumps show up here.
    float bump[2];
    // Road surface under the front tyres: the game's own rumble for it (0..1, grass, gravel, rumble
    // strips) and its frequency in Hz.
    float surfaceRumble;
    float surfaceHz;
    // Front wheels off the road ([0] left, [1] right): the surface's own name is grass, dirt, mud,
    // gravel, sand and the like (not asphalt, concrete, cobbles, metal, wood, rumble strips).
    bool  offroad[2];

    // Collisions, from the game's collision reports for the player car (LocalPlayer::OnCollision).
    // A hit counts up hitSeq; contact sliding along something stays fresh in scrapeTime.
    uint32_t hitSeq;
    float    hitSpeed;     // m/s, closing speed along the contact normal
    float    hitImpulse;   // the game's impulse on the car (light objects give little)
    float    hitLat;       // -1..1: the push on the car's nose to the right (+) or left (-)
    // Breakable props (benches, sign posts, fences, bins: the game's Smackables) knocked by the car,
    // told apart from other contacts by the object's class, counted up in breakSeq.
    uint32_t breakSeq;
    float    breakSpeed;   // m/s, the car's speed at the knock
    float    breakLat;     // -1..1: the prop is to the right (+) or left (-) of the car's nose
    // Hits on another vehicle (traffic, cops, racers: the other object's ISimable type is
    // SIMABLE_VEHICLE), counted up in carHitSeq; they don't count as generic hits.
    uint32_t carHitSeq;
    float    carHitSpeed;  // m/s, the car's speed at the hit
    float    carHitLat;    // -1..1: the hit pushes the car to the right (+) or left (-)
    float    carHitImpulse;  // the game's impulse on the car, like hitImpulse
    double   scrapeTime;   // Now() of the latest sliding contact
    float    scrapeSpeed;  // m/s along the wall
    float    scrapeLat;    // -1..1: the wall pushes the car to the right (+) or left (-)
    // The latest hit of either kind (hit or vehicle hit): when, and which way it pushed the car.
    double   contactTime;
    float    contactLat;
    // Impacts that damaged the car, counted by the gameplay mod (Config::damageHits), and the
    // latest one's strength: its speed change over the damage threshold (1 = just enough).
    uint32_t damageSeq;
    float    damageStrength;
};

bool Telemetry_Install();
// Copy of the most recent sample. Thread-safe.
void Telemetry_Get(Telemetry* out);
