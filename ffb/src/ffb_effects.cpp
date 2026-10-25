#include "ffb_effects.h"
#include "ffb_filters.h"
#include <math.h>

namespace ffb {

namespace {
const double kTwoPi = 6.283185307179586;
double Advance(double phase, float hz, float dt) { return fmod(phase + kTwoPi * hz * dt, kTwoPi); }
// Share of a knock's full strength at the car's speed (m/s): full from 150 km/h, a tenth at least.
float SpeedShare(float speed) { return Clampf(speed / (150.0f / 3.6f), 0.1f, 1.0f); }
const JoltShape kCrashJolt = {0.12f, 0.6f, 0.15f, 20.0f, 0.55f};   // a firm shove, then the shake
const JoltShape kBreakJolt = {0.05f, 0.5f, 0.06f, 28.0f, 0.25f};   // short and sharp, a quick rattle
const JoltShape kLandJolt = {0.07f, 0.7f, 0.10f, 18.0f, 0.35f};    // landings and vehicle hits
}

float Jolt::Step(double now, const JoltShape& s, float* level) {
    if (start < 0) return 0.0f;
    float tau = (float)(now - start);
    if (tau > s.length || tau < 0) {
        start = -1;
        return 0.0f;
    }
    float push = tau < s.push ? sinf(3.14159265f * tau / s.push) : 0.0f;
    float decay = expf(-tau / s.shakeTau);
    if (level) *level = amp * decay;
    return amp * (dir * push + s.shake * decay * sinf((float)(kTwoPi * s.shakeHz) * tau));
}

EffectParts Effects::Step(const Telemetry& t, float drive, double now, float dt, const EffectParams& p) {
    EffectParts e;
    drive = Clampf(drive, 0.0f, 1.0f);
    float speed = t.valid ? t.speed : 0.0f;

    // --- engine: firing frequency of a four-stroke (two per revolution), kept where a wheel can
    // render it; strongest standing still, under the road at speed ---
    if (t.valid && t.rpm > 100.0f && p.engine > 0.0f) {
        float hz = Clampf(t.rpm / 30.0f, 12.0f, 70.0f);
        enginePhase_ = Advance(enginePhase_, hz, dt);
        float amp = p.engine * (0.5f + 0.5f * t.rpmNorm) * (1.0f - 0.6f * Smoothstep(3.0f, 25.0f, speed));
        e.engine = amp * (float)sin(enginePhase_) * drive;
    }

    // --- kerbs and bumps: a front wheel's suspension compressing quickly (the slow part, roll and
    // pitch, taken out): a kick toward the side hit harder and a short shake, read once per
    // physics step ---
    if (t.seq != lastSeq_) {
        lastSeq_ = t.seq;
        if (t.valid && p.kerbs > 0.0f) {
            const float ref = 0.5f;   // m/s of quick compression that is a solid hit
            float gate = Smoothstep(1.0f, 5.0f, speed);
            float kick = p.kerbs * 0.6f * tanhf((t.bump[1] - t.bump[0]) / ref) * gate;
            if (fabsf(kick) > fabsf(kerbKick_)) kerbKick_ = kick;
            float hit = fmaxf(fabsf(t.bump[0]), fabsf(t.bump[1]));
            float shake = p.kerbs * 0.4f * tanhf(fmaxf(hit - 0.15f, 0.0f) / ref) * gate;
            if (shake > kerbShake_) kerbShake_ = shake;
        }
    }
    kerbKick_ *= expf(-dt / 0.05f);
    kerbShake_ *= expf(-dt / 0.08f);
    kerbPhase_ = Advance(kerbPhase_, 30.0f, dt);
    e.kerb = (kerbKick_ + kerbShake_ * (float)sin(kerbPhase_)) * drive;

    // --- tyre scrub: a fine vibration once any tyre is past its grip limit ---
    if (t.valid && t.tireStateValid && p.tyreScrub > 0.0f && t.wheelsOnGround > 0 && speed > 3.0f) {
        float grip = 1.0f;
        for (int i = 0; i < 4; ++i) {
            if (!t.tireOnGround[i]) continue;
            grip = fminf(grip, Clampf(t.tireGrip[i], 0.0f, 1.0f));
        }
        float level = p.tyreScrub * Smoothstep(0.25f, 0.9f, 1.0f - grip) * Smoothstep(3.0f, 15.0f, speed);
        if (level > 0.0f) {
            scrubPhase_ = Advance(scrubPhase_, 28.0f + 0.4f * speed, dt);
            e.scrub = level * (float)sin(scrubPhase_) * drive;
        }
    }

    // --- crashes ---
    if (!haveHitSeq_) {
        haveHitSeq_ = true;
        lastHitSeq_ = t.hitSeq;
        lastBreakSeq_ = t.breakSeq;
        lastCarHitSeq_ = t.carHitSeq;
    }

    // --- breakable props: a short, sharp knock, harder the faster the car hits ---
    if (t.breakSeq != lastBreakSeq_) {
        lastBreakSeq_ = t.breakSeq;
        float amp = p.breakables * SpeedShare(t.breakSpeed);
        // Toward the prop's side (the front wheel on that side takes it); straight ahead only rattles.
        if (amp > 0.01f) break_.Start(now, amp, fabsf(t.breakLat) > 0.2f ? (t.breakLat > 0 ? 1.0f : -1.0f) : 0.0f);
    }
    e.breakable = break_.Step(now, kBreakJolt) * drive;
    if (t.hitSeq != lastHitSeq_) {
        lastHitSeq_ = t.hitSeq;
        // Closing speed says how hard the hit was; the game's impulse says how heavy the thing was.
        // Light props (benches, posts) still give a clear knock from the closing speed alone.
        float amp = p.crashes * Clampf(0.45f * tanhf(t.hitSpeed / 8.0f) + 0.55f * tanhf(t.hitImpulse / 20.0f), 0.0f, 1.0f);
        if (amp > 0.02f) {
            if (!crash_.Fresh(now)) crashDamaged_ = false;
            crash_.Raise(now, amp, Clampf(t.hitLat, -1.0f, 1.0f));
        }
    }

    // --- off-road: an uneven rumble while front wheels are on grass, dirt, mud, gravel: two rough
    // tones and random bumps, half with one wheel off, growing with speed, eased in and out ---
    {
        float share = t.valid ? ((t.offroad[0] ? 0.5f : 0.0f) + (t.offroad[1] ? 0.5f : 0.0f)) : 0.0f;
        float target = p.offroad * share * Smoothstep(1.0f, 12.0f, speed) * (0.7f + 0.3f * Smoothstep(12.0f, 40.0f, speed));
        offroadLevel_ += (target - offroadLevel_) * Clampf(dt / 0.1f, 0.0f, 1.0f);
        roughPhase_[0] = Advance(roughPhase_[0], 17.0f, dt);
        roughPhase_[1] = Advance(roughPhase_[1], 29.0f, dt);
        bumpWait_ -= dt;
        if (bumpWait_ <= 0.0f) {
            rand_ = rand_ * 1664525u + 1013904223u;
            float r = (rand_ >> 8) / 16777216.0f;                  // 0..1
            bump_ = (r < 0.5f ? -1.0f : 1.0f) * (0.5f + r);
            bumpWait_ = 0.05f + 0.15f * (1.0f - Smoothstep(5.0f, 30.0f, speed)) + 0.08f * r;
        }
        bump_ *= expf(-dt / 0.035f);
        float rough = 0.45f * (float)sin(roughPhase_[0]) + 0.25f * (float)sin(roughPhase_[1]) + 0.45f * bump_;
        e.offroad = offroadLevel_ * 0.5f * rough * drive;
    }

    // --- landing: all four wheels off the ground for 0.15 s or more, then touching down: a hard
    // kick (toward the side the front is sliding, else alternating) and a short shake ---
    if (t.valid && t.wheelsOnGround == 0) {
        airTime_ += dt;
    } else {
        if (t.valid && airTime_ >= 0.15f && p.landing > 0.0f) {
            landDir_ = fabsf(t.vLatFront) > 0.3f ? (t.vLatFront > 0 ? 1.0f : -1.0f) : -landDir_;
            land_.Start(now, p.landing * Clampf(0.55f + airTime_ * 0.6f, 0.55f, 1.2f), landDir_);
        }
        airTime_ = 0.0f;
    }
    e.landing = land_.Step(now, kLandJolt) * drive;

    // --- another vehicle hit: the landing's kick and shake, toward the side the car was pushed;
    // nose to tail (no clear side) the shake alone ---
    if (t.carHitSeq != lastCarHitSeq_) {
        lastCarHitSeq_ = t.carHitSeq;
        float dir = fabsf(t.carHitLat) > 0.2f ? (t.carHitLat > 0 ? 1.0f : -1.0f) : 0.0f;
        if (p.carHits > 0.0f) carHit_.Start(now, p.carHits * SpeedShare(t.carHitSpeed), dir);
    }

    // --- an impact that damaged the car (the gameplay mod counts them): the crash or vehicle-hit
    // jolt it came with is raised to the damage's strength (a third of full at the threshold, all
    // of it at twice it), else a crash jolt starts away from the side of the latest contact (the
    // push on the car), or the shake alone. It ducks the other effects while it plays ---
    if (t.damageSeq != lastDamageSeq_) {
        lastDamageSeq_ = t.damageSeq;
        bool recent = t.contactTime > 0 && fabs(t.time - t.contactTime) < 0.3;
        float lat = recent ? t.contactLat : 0.0f;
        float amp = p.crashes * Clampf(0.35f + 0.65f * (t.damageStrength - 1.0f), 0.35f, 1.25f);
        float dir = fabsf(lat) > 0.05f ? (lat > 0.0f ? 1.0f : -1.0f) : 0.0f;
        if (amp > 0.0f) {
            if (carHit_.Fresh(now)) {
                carHit_.Raise(now, amp, dir);
            } else {
                crash_.Raise(now, amp, dir);
                crashDamaged_ = true;
            }
        }
    }
    float crashLevel = 0.0f, carLevel = 0.0f;
    e.crash = crash_.Step(now, kCrashJolt, &crashLevel) * drive;
    e.crashLevel = crashDamaged_ ? crashLevel * drive : 0.0f;
    e.carHit = carHit_.Step(now, kLandJolt, &carLevel) * drive;
    e.carHitLevel = carLevel * drive;

    // --- scraping along a wall: the contact stays fresh while the reports keep coming, and fades
    // over 0.1 s after the last one, so a short brush is felt too ---
    float since = (float)(now - t.scrapeTime);
    if (t.scrapeTime > 0 && since >= 0 && since < 0.2f && p.scrapes > 0.0f && speed > 1.0f) {
        float env = Clampf((0.2f - since) / 0.1f, 0.0f, 1.0f);
        float amp = p.scrapes * tanhf(t.scrapeSpeed / 12.0f) * env;
        double tt = now;
        float grind = 0.6f * (float)sin(kTwoPi * 41.0 * tt) + 0.4f * (float)sin(kTwoPi * 67.0 * tt + 1.0);
        e.scrape = (amp * 0.5f * grind - Clampf(t.scrapeLat, -1.0f, 1.0f) * amp * 0.4f) * drive;
    }
    return e;
}

}  // namespace ffb
