#include "ffb_model.h"
#include "ffb_filters.h"
#include <math.h>

namespace ffb {

namespace {
const float kPi = 3.14159265f;
// Both front tyres' static load (N) of the median player car (1530 kg, 52% front): CarWeight
// makes a car whose front axle carries more steer heavier, one with less lighter.
const float kNominalFrontLoad = 7800.0f;
}

float FrontTravel(const Telemetry& t) {
    // The game's own front tyres: where they point plus their slip angle. It sees the car's yaw
    // and slide as the tyre model does, every physics step.
    return t.frontSlipKnown ? (t.steerDeg + t.frontSlipDeg) * kPi / 180.0f : 0.0f;
}

float FrontSlipAngle(const Telemetry& t, const WheelState& w, const ModelParams& p, float roadLock) {
    // Where the front wheels point: the wheel's share of the steering lock times the game's lock
    // (with speed-sensitive steering only a few degrees at speed). From the wheel itself, so the
    // force answers the wheel at once, not a physics step later.
    float share = Clampf(w.pos / Clampf(p.lockShare, 0.05f, 1.0f), -1.5f, 1.5f);
    float steer = share * (roadLock > 0.5f ? roadLock : p.maxRoadAngleDeg) * kPi / 180.0f;
    return steer - FrontTravel(t);
}

ForceParts Model::Compute(const Telemetry& t, const WheelState& w, const ModelParams& p, float drivingWeight, float dt) {
    ForceParts f;
    float lockShare = Clampf(p.lockShare, 0.05f, 1.0f);
    float speed = t.valid ? t.speed : 0.0f;
    float drive = Clampf(drivingWeight, 0.0f, 1.0f);

    // The game's front wheel angle at the steering lock: the lock it steered with this tick (it
    // follows speed-sensitive steering quickly); maxRoadAngleDeg until the first steering input.
    if (roadLock_ <= 0) roadLock_ = p.maxRoadAngleDeg;
    if (t.valid && t.lockDeg > 0.5f)
        roadLock_ += (Clampf(t.lockDeg, 2.0f, 60.0f) - roadLock_) * Clampf(dt / 0.1f, 0.0f, 1.0f);

    // --- self-aligning torque -------------------------------------------------------------
    if (t.valid && t.frontOnGround > 0 && w.known) {
        // Two references for the front tyres' load, learned with both on the ground. The static
        // one, slowly and only below 55 km/h where downforce adds little, is the car's weight on
        // its front axle (the gameplay mod's real weight distribution included): CarWeight. The
        // usual one, over a few seconds at any speed, takes in the downforce as speed builds (the
        // game's is far more than a road car's), so the weight follows what changes quickly:
        // braking, throttle, crests and dips.
        if (t.frontOnGround == 2 && t.frontLoad > 0) {
            if (loadRef_ <= 0) loadRef_ = t.frontLoad;
            if (dynRef_ <= 0) dynRef_ = t.frontLoad;
            if (speed > 3.0f && speed < 15.0f) loadRef_ += (t.frontLoad - loadRef_) * Clampf(dt / 8.0f, 0.0f, 1.0f);
            if (speed > 3.0f) dynRef_ += (t.frontLoad - dynRef_) * Clampf(dt / 5.0f, 0.0f, 1.0f);
        }
        float loadRatio = dynRef_ > 0 ? t.frontLoad / dynRef_ : 0.5f * t.frontOnGround;
        float car = loadRef_ > 0 ? powf(Clampf(loadRef_ / kNominalFrontLoad, 0.3f, 2.0f), p.carWeight) : 1.0f;
        float load = car * Clampf(1.0f + p.loadSensitivity * (loadRatio - 1.0f), 0.3f, 1.6f);
        // Brush-model shape: rises to 1 at the grip peak, then falls away (the wheel goes light
        // as the front lets go); a smaller pull toward the travel direction stays in a slide.
        // Each front tyre on its own when the game has both: its own slip angle and its share of
        // the load. In a corner the outside tyre carries more and grips to a larger slip angle (a
        // tyre's peak grows with its load), so the inside one lets go first; through a quick
        // left-right the weight, and the pull with it, swings from one tyre to the other.
        // LoadSensitivity sets how much (0 = both tyres alike).
        float peak = Clampf(p.gripPeakDeg, 1.0f, 30.0f) * kPi / 180.0f;
        float alpha = FrontSlipAngle(t, w, p, roadLock_);
        float shape = 0.0f, side = 0.0f;
        int tyres = t.frontEachKnown ? 2 : 1;
        for (int i = 0; i < tyres; ++i) {
            float share = 1.0f, a = alpha;
            if (tyres == 2) {
                float l = t.frontLoadEach[i] / (t.frontLoadEach[0] + t.frontLoadEach[1]);
                share = Clampf(1.0f + p.loadSensitivity * (2.0f * l - 1.0f), 0.0f, 2.0f);
                a -= (t.frontSlipEachDeg[i] - t.frontSlipDeg) * kPi / 180.0f;
            }
            float x = a / (peak * sqrtf(fmaxf(share, 0.25f)));
            shape += share / tyres * (x * expf(0.5f * (1.0f - x * x)) + p.driftAlign * tanhf(0.5f * x));
            // The tyre's side force for the caster: up to the grip peak, then easing off to 70% by
            // 3 times it (a sliding tyre's side force), not falling away like the tyre's own pull.
            side += share / tyres * tanhf(x) * (1.0f - 0.3f * Smoothstep(1.0f, 3.0f, fabsf(x)));
        }
        // None when parked (no slip angle to speak of), in by 20 km/h; weaker in reverse.
        float speedGain = Smoothstep(0.5f, 6.0f, speed);
        if (t.vLong < -0.5f) speedGain *= 0.3f;
        f.aligning = -p.aligning * shape * load * speedGain * drive;
        // Caster: the tyres' side force times the steering's mechanical trail, growing with the load.
        f.caster = -p.caster * side * load * speedGain * drive;

        // Front grip loss: the front tyres' traction as the game has it (lower = past the friction
        // limit: spinning, sliding wide, locked); the weaker one counts. The game's value is not 1
        // while the tyres grip: about 0.85 cruising at speed, and it wanders. Measured against 1,
        // the wheel was lighter more than half the time in normal driving and a real slide stood
        // out little. So it counts against the car's own usual traction, learned while the front
        // is well within its grip (over seconds; quicker up than down, so a lock-up or wheelspin
        // doesn't teach it): from 0.08 below it the wheel goes light, by 0.3 below it keeps
        // 1 - FrontGripLoss of its weight. A front drive car spinning its wheels, a car pushing
        // wide, a locked front: light within ~0.15 s (a shorter dip doesn't move the wheel), and
        // the weight comes back over ~0.3 s as the tyres grip again (no snap to the centre).
        float grip = 1.0f;
        if (t.tireStateValid)
            for (int i = 0; i < 2; ++i)
                if (t.tireOnGround[i]) grip = fminf(grip, Clampf(t.tireGrip[i], 0.0f, 1.0f));
        float tau = grip < frontGrip_ ? 0.15f : 0.3f;
        frontGrip_ += (grip - frontGrip_) * Clampf(dt / tau, 0.0f, 1.0f);
        if (t.frontOnGround == 2 && speed > 10.0f && fabsf(alpha) < 0.4f * peak) {
            float learn = grip > gripBase_ ? 1.0f : 5.0f;
            gripBase_ += (grip - gripBase_) * Clampf(dt / learn, 0.0f, 1.0f);
            gripBase_ = Clampf(gripBase_, 0.6f, 1.0f);
        }
        gripLight_ = 1.0f - Clampf(p.frontGripLoss, 0.0f, 1.0f) * Smoothstep(0.08f, 0.3f, gripBase_ - frontGrip_);
        f.aligning *= gripLight_;
        f.caster *= gripLight_;
    }

    // --- rear slide (seat of pants): a pull toward the countersteer once the rear axle slides
    // sideways past what normal cornering gives (about 3 degrees), full by 15; stronger as the
    // rear tyres lose traction. In a real car you feel the rear go in the seat before the front
    // tyres tell you; here the wheel tells you. In from 15 km/h, full by 30; none in reverse.
    // A rear wheel in the air (a jump, a bump mid-slide): the last pull fades out over 0.2 s and
    // comes back over 0.1 s once both are down (cut at once, it knocked the wheel).
    if (t.valid && p.rearSlide > 0.0f && t.vLong > 0.5f) {
        if (t.tireOnGround[2] && t.tireOnGround[3]) {
            float rearGrip = 1.0f;
            if (t.tireStateValid) rearGrip = fminf(Clampf(t.tireGrip[2], 0.0f, 1.0f), Clampf(t.tireGrip[3], 0.0f, 1.0f));
            float tau = rearGrip < rearGrip_ ? 0.05f : 0.3f;
            rearGrip_ += (rearGrip - rearGrip_) * Clampf(dt / tau, 0.0f, 1.0f);
            float slipDeg = atan2f(t.vLatRear, t.vLong) * 180.0f / kPi;      // + = the rear moving right
            float level = Smoothstep(3.0f, 15.0f, fabsf(slipDeg)) * (0.6f + 0.4f * Smoothstep(0.0f, 0.5f, 1.0f - rearGrip_));
            // The tail moving out to the right is caught by steering right: +.
            rearPull_ = p.rearSlide * (slipDeg > 0 ? level : -level) * Smoothstep(4.0f, 8.0f, t.vLong) * drive;
            rearContact_ = fminf(1.0f, rearContact_ + dt / 0.1f);
        } else {
            rearContact_ = fmaxf(0.0f, rearContact_ - dt / 0.2f);
        }
        f.rearSlide = rearPull_ * rearContact_;
    } else {
        rearGrip_ = rearContact_ = 1.0f;
        rearPull_ = 0.0f;
    }

    if (!w.known) return f;
    float share = w.pos / lockShare;          // -1..1 at the steering lock
    float shareVel = Clampf(w.vel / lockShare, -4.0f, 4.0f);

    // --- centring spring toward straight ahead: parked and slow, fading out as the tyres' own
    // aligning torque comes in; the menu spring otherwise ---
    float k = p.centering * (1.0f - Smoothstep(0.5f, 6.0f, speed)) * drive + p.menuCentering * (1.0f - drive);
    f.centering = -k * Clampf(share * 1.5f, -1.0f, 1.0f);

    // --- damping and friction ---------------------------------------------------------------
    float parked = 1.0f - Smoothstep(0.5f, 5.0f, speed);
    f.damping = -p.damping * (0.5f + 0.5f * parked) * 0.25f * shareVel;
    // Stiction (position based, so nothing lags the wheel): a stiff spring to an anchor that the
    // wheel drags along once it is turned past kStickDeflect; then the full friction holds against
    // the way it was turned. Held still, the wheel sits in the spring and feels no force flipping;
    // the velocity-sign friction it replaces pushed on after the wheel reversed (smoothed speed)
    // and wobbled. A little damping inside the spring keeps it from ringing.
    float fric = p.friction * (0.4f + 0.6f * parked) + p.parkingFriction * parked * drive;
    const float kStickDeflect = 0.006f;     // of the wheel's rotation: ~2.7 deg on a 900 deg wheel
    if (!anchorSet_) {
        anchor_ = w.pos;
        anchorSet_ = true;
    }
    float d = w.pos - anchor_;
    if (d > kStickDeflect) anchor_ = w.pos - kStickDeflect;
    else if (d < -kStickDeflect) anchor_ = w.pos + kStickDeflect;
    d = w.pos - anchor_;
    float stuck = 1.0f - Clampf(fabsf(d) / kStickDeflect - 0.8f, 0.0f, 0.2f) / 0.2f;   // 1 inside, 0 at the edge
    const float kStickDamping = 0.6f;       // per (rotation / s), times the friction
    f.friction = -fric * (d / kStickDeflect + kStickDamping * Clampf(w.vel, -4.0f, 4.0f) * stuck);

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
