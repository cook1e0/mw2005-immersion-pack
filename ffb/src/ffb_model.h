// The force model: what the steering should feel like, from the car's state and the wheel's.
//
// All forces are shares of full scale, + = turns the wheel to the right. Pure: no DirectInput, no
// clock, no globals, so the tests can check every sign and shape.
//
//   aligning   the front tyres' self-aligning torque. The front axle's direction of travel versus
//              where the front wheels point gives the tyres' slip angle; the tyre pulls the wheel
//              back toward the direction of travel, harder with grip and load, and lets go past
//              the grip limit (understeer goes light). In a slide the travel direction is
//              sideways, so the wheel pulls into the countersteer by itself.
//   centering  a speed-sensitive spring toward straight ahead, for the arcade side of the game,
//              and the centre weight: a firm hold around straight ahead once moving, so the wheel
//              doesn't just stay wherever it is left
//   damping    weight against fast wheel movement (more when parked)
//   friction   resistance to turning the wheel, tyre scrub when parked: a stiction spring to an
//              anchor the wheel drags along (position based: no flipping, no lag, no wobble)
//   softLock   the end stop at the game's steering lock
#pragma once
#include "telemetry.h"

namespace ffb {

struct WheelState {
    bool  known = false;
    float pos = 0;      // -1 full left .. +1 full right (of the wheel's rotation)
    float vel = 0;      // per second, same units
    float velSmooth = 0; // the same, smoothed more (for friction: quiet when the wheel is still)
};

struct ModelParams {
    float aligning = 0.8f;          // self-aligning torque strength
    float gripPeakDeg = 7.0f;       // front slip angle where the tyres grip hardest (heaviest wheel)
    float driftAlign = 0.3f;        // pull toward the direction of travel that stays past the grip limit
    float loadSensitivity = 0.5f;   // how much front tyre load scales the aligning torque
    float centering = 0.3f;         // spring toward centre at CenteringSpeed and above
    float centeringParked = 0.25f;  // share of it at standstill
    float centeringSpeed = 120.0f;  // km/h
    float centreWeight = 0.3f;      // firm hold around straight ahead while moving
    float damping = 0.3f;
    float friction = 0.05f;
    float parkingFriction = 0.15f;  // extra friction near standstill (tyre scrub)
    float softLock = 0.8f;
    float lockShare = 1.0f;         // wheel position of the game's full steering lock (0.05..1)
    float maxRoadAngleDeg = 45.0f;  // front wheel angle at the steering lock until the game's is known
    float menuCentering = 0.2f;     // spring in menus, pause and cutscenes
};

struct ForceParts {
    float aligning = 0, centering = 0, damping = 0, friction = 0, softLock = 0;
    float Sum() const { return aligning + centering + damping + friction + softLock; }
};

// Slip angle of the front tyres in radians (+ = the wheels point right of where the axle travels).
// `roadLock`: front wheel degrees at the steering lock (0 = ModelParams::maxRoadAngleDeg).
float FrontSlipAngle(const Telemetry& t, const WheelState& w, const ModelParams& p, float roadLock = 0);

class Model {
public:
    // `drivingWeight` 0 (menus) .. 1 (driving), eased by the engine for smooth transitions.
    ForceParts Compute(const Telemetry& t, const WheelState& w, const ModelParams& p, float drivingWeight, float dt);
    float LoadRef() const { return loadRef_; }
    float RoadLock() const { return roadLock_; }
private:
    float loadRef_ = 0;            // the front tyres' usual load, learned while driving
    float roadLock_ = 0;           // degrees of front wheel angle at the steering lock, measured
    float anchor_ = 0;             // friction: where the stiction spring holds the wheel
    bool anchorSet_ = false;
};

}  // namespace ffb
