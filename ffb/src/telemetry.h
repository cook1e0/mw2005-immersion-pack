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
};

bool Telemetry_Install();
// Copy of the most recent sample. Thread-safe.
void Telemetry_Get(Telemetry* out);
