// The player car's state for force feedback, sampled from speed.exe v1.3 once per physics tick
// (about 60 Hz) on the game thread and read by the FFB thread.
//
// Everything is in the car's own frame, + = right / forward / up. Forward is rear axle -> front
// axle, right is front left wheel (0) -> front right wheel (1), square to forward: taken from the
// car itself, since the sim's world (where ISuspension reports wheels) is Y-up and left-handed,
// unlike the renderer's Z-up (bConvertFromBond: render = (z, -x, y)). Wheel 1 is the right one:
// in a right turn it is the inside wheel and steers more (Ackermann).
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
    float steerDeg;        // the front wheels' angle now, degrees (either sign convention)
    // Degrees of front wheel angle at full steering input right now: the front wheels' angle over
    // the input the game steered them with this tick (speed-sensitive, widened in a slide, whatever
    // the steering type). Held while the input is near centre; 0 = not known yet.
    float lockDeg;
    // The front tyres' slip angle as the game's tyre model has it (SuspensionRacer::Tire +0x118),
    // degrees, + = the tyre travels right of where it points; the mean of the front tyres on the
    // ground. The front axle travels at steerDeg + frontSlipDeg from the car's nose: yaw, a slide
    // and all. Only when frontSlipKnown.
    float frontSlipDeg;
    bool  frontSlipKnown;

    float rpm;             // engine
    float rpmNorm;         // 0 idle .. 1 redline

    // Tyre state from the game's ISuspension, [0] = front left, [1] = front right, [2] = rear
    // left, [3] = rear right. Only when tireStateValid (SuspensionRacer: GetWheelTraction is not
    // part of every ISuspension implementation).
    bool  tireStateValid;
    bool  tireOnGround[4];    // this wheel is touching the ground
    float tireGrip[4];        // GetWheelTraction: 1 = full grip, lower = past the friction limit

    // Front wheels off the road ([0] left, [1] right): the surface's own name is grass, dirt, mud,
    // gravel, sand and the like (not asphalt, concrete, cobbles, metal, wood, rumble strips).
    bool  offroad[2];
    // Front suspension, [0] = left, [1] = right: compression speed with the slow part (body roll,
    // pitch, hills) taken out, m/s, + = compressing. Kerbs and bumps show up here.
    float bump[2];
    // Road surface under the front tyres (not off-road ones: OffroadVibration has those): the
    // game's own rumble for it (0..1: rumble strips, cobbles...) and its frequency in Hz.
    float surfaceRumble;
    float surfaceHz;

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
