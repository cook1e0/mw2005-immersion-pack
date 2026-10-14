#include "ffb_model.h"
#include "ffb_filters.h"
#include <math.h>

namespace ffb {

namespace {
const float kPi = 3.14159265f;
float Smoothstep(float a, float b, float x) {
    float t = Clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}

float FrontSlipAngle(const Telemetry& t, const WheelState& w, const ModelParams& p) {
    // Where the front wheels point: the wheel's share of the steering lock times the game's lock.
    float share = Clampf(w.pos / Clampf(p.lockShare, 0.05f, 1.0f), -1.5f, 1.5f);
    float steer = share * p.maxRoadAngleDeg * kPi / 180.0f;
    // Where the front axle travels, relative to the car's nose.
    float travel = atan2f(t.vLatFront, fmaxf(fabsf(t.vLong), 1.0f));
    return steer - travel;
}

ForceParts Model::Compute(const Telemetry& t, const WheelState& w, const ModelParams& p, float drivingWeight, float dt) {
    ForceParts f;
    float lockShare = Clampf(p.lockShare, 0.05f, 1.0f);
    float speed = t.valid ? t.speed : 0.0f;
    float drive = Clampf(drivingWeight, 0.0f, 1.0f);

    // --- self-aligning torque -------------------------------------------------------------
    if (t.valid && t.frontOnGround > 0 && w.known) {
        // The front tyres' usual load, learned slowly while driving with both on the ground.
        if (t.frontOnGround == 2 && speed > 3.0f && t.frontLoad > 0) {
            if (loadRef_ <= 0) loadRef_ = t.frontLoad;
            loadRef_ += (t.frontLoad - loadRef_) * Clampf(dt / 5.0f, 0.0f, 1.0f);
        }
        float loadRatio = loadRef_ > 0 ? t.frontLoad / loadRef_ : 0.5f * t.frontOnGround;
        float load = Clampf(1.0f + p.loadSensitivity * (loadRatio - 1.0f), 0.3f, 1.6f);
        // Brush-model shape: rises to 1 at the grip peak, then falls away (the wheel goes light
        // as the front lets go); a smaller pull toward the travel direction stays in a slide.
        float x = FrontSlipAngle(t, w, p) / (Clampf(p.gripPeakDeg, 1.0f, 30.0f) * kPi / 180.0f);
        float shape = x * expf(0.5f * (1.0f - x * x)) + p.driftAlign * tanhf(0.5f * x);
        // None when parked, building with speed; weaker in reverse.
        float speedGain = Smoothstep(0.5f, 6.0f, speed) * (0.55f + 0.45f * Clampf(speed / 45.0f, 0.0f, 1.0f));
        if (t.vLong < -0.5f) speedGain *= 0.3f;
        f.aligning = -p.aligning * shape * load * speedGain * drive;
    }

    if (!w.known) return f;
    float share = w.pos / lockShare;          // -1..1 at the steering lock
    float shareVel = Clampf(w.vel / lockShare, -4.0f, 4.0f);

    // --- centring spring: weak when parked, full from CenteringSpeed up; menu spring otherwise ---
    float speedShare = Clampf(speed * 3.6f / Clampf(p.centeringSpeed, 10.0f, 400.0f), 0.0f, 1.0f);
    float k = p.centering * (p.centeringParked + (1.0f - p.centeringParked) * speedShare);
    k = k * drive + p.menuCentering * (1.0f - drive);
    f.centering = -k * Clampf(share * 1.5f, -1.0f, 1.0f);

    // --- damping and friction ---------------------------------------------------------------
    float parked = 1.0f - Smoothstep(0.5f, 5.0f, speed);
    f.damping = -p.damping * (0.5f + 0.5f * parked) * 0.25f * shareVel;
    f.friction = -(p.friction + p.parkingFriction * parked * drive) * tanhf(w.vel / 0.03f);

    // --- soft lock: the end stop where the game's steering is at full lock ---------------------
    if (lockShare < 0.98f && p.softLock > 0.0f) {
        float over = fabsf(w.pos) - lockShare;
        if (over > 0.0f) {
            float s = w.pos > 0 ? 1.0f : -1.0f;
            float push = p.softLock * Clampf(over / 0.03f, 0.0f, 1.0f);
            float outward = Clampf(w.vel * s * 0.2f, 0.0f, 0.5f) * p.softLock;
            f.softLock = -s * (push + outward);
        }
    }
    return f;
}

}  // namespace ffb
