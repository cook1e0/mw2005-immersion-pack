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
//   caster     the steering geometry's pull (caster trail times the tyres' side force): it grows
//              with the front tyres' load, so the wheel gets heavier under braking and lighter
//              under throttle; past the grip limit it eases off to what a sliding tyre keeps
//              (frontGripLoss lightens both when the front tyres lose traction: wheelspin,
//              understeer, a lock-up)
//   centering  a spring toward straight ahead while parked or slow, fading out as the aligning
//              torque comes in (by 20 km/h), and in menus, pause and cutscenes
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
    float loadSensitivity = 1.0f;   // how much the front tyres' load against their static load scales the weight
    float carWeight = 0.5f;         // how much the car's static front load (against kNominalFrontLoad) scales it
    float caster = 0.2f;            // steering geometry's pull, times the tyres' side force and load
    float frontGripLoss = 0.6f;     // share of the tyres' and the caster's weight gone with the front tyres' traction
    float centering = 0.3f;         // spring toward centre while parked or slow
    float damping = 0.3f;
    float friction = 0.05f;
    float parkingFriction = 0.15f;  // extra friction near standstill (tyre scrub)
    float softLock = 0.8f;
    float lockShare = 1.0f;         // wheel position of the game's full steering lock (0.05..1)
    float maxRoadAngleDeg = 45.0f;  // front wheel angle at the steering lock until the game steers
    float menuCentering = 0.2f;     // spring in menus, pause and cutscenes
};

struct ForceParts {
    float aligning = 0, caster = 0, centering = 0, damping = 0, friction = 0, softLock = 0;
    float Sum() const { return aligning + caster + centering + damping + friction + softLock; }
};

// Where the front axle travels, radians from the car's nose (+ = right).
float FrontTravel(const Telemetry& t);
// Slip angle of the front tyres in radians (+ = the wheels point right of where the axle travels).
// `roadLock`: front wheel degrees at the steering lock (0 = ModelParams::maxRoadAngleDeg).
float FrontSlipAngle(const Telemetry& t, const WheelState& w, const ModelParams& p, float roadLock = 0);

class Model {
public:
    // `drivingWeight` 0 (menus) .. 1 (driving), eased by the engine for smooth transitions.
    ForceParts Compute(const Telemetry& t, const WheelState& w, const ModelParams& p, float drivingWeight, float dt);
    float LoadRef() const { return loadRef_; }
    float RoadLock() const { return roadLock_; }
    float FrontGrip() const { return frontGrip_; }   // the front tyres' traction, smoothed (1 = gripping)
    float GripLight() const { return gripLight_; }   // the weight kept for it (1 = all)
private:
    float loadRef_ = 0;            // the front tyres' static load, learned while driving slowly
    float dynRef_ = 0;             // their usual load over the last few seconds (downforce included)
    float roadLock_ = 0;           // degrees of front wheel angle at the steering lock, the game's
    float frontGrip_ = 1;          // the front tyres' traction (the game's), smoothed
    float gripLight_ = 1;          // what frontGripLoss leaves of the weight
    float anchor_ = 0;             // friction: where the stiction spring holds the wheel
    bool anchorSet_ = false;
};

}  // namespace ffb
