#include "ffb_effects.h"
#include "ffb_filters.h"
#include <math.h>

namespace ffb {

namespace {
const double kTwoPi = 6.283185307179586;
float Ramp(float a, float b, float x) {
    float t = Clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
double Advance(double phase, float hz, float dt) { return fmod(phase + kTwoPi * hz * dt, kTwoPi); }
}

EffectParts Effects::Step(const Telemetry& t, float drive, double now, float dt, const EffectParams& p) {
    EffectParts e;
    drive = Clampf(drive, 0.0f, 1.0f);
    float speed = t.valid ? t.speed : 0.0f;
    shakePhase_ = Advance(shakePhase_, 30.0f, dt);

    // --- engine: firing frequency of a four-stroke (two per revolution), kept where a wheel can
    // render it; strongest standing still, under the road at speed ---
    if (t.valid && t.rpm > 100.0f && p.engine > 0.0f) {
        float hz = Clampf(t.rpm / 30.0f, 12.0f, 70.0f);
        enginePhase_ = Advance(enginePhase_, hz, dt);
        float amp = p.engine * (0.5f + 0.5f * t.rpmNorm) * (1.0f - 0.6f * Ramp(3.0f, 25.0f, speed));
        e.engine = amp * (float)sin(enginePhase_) * drive;
    }

    // --- surface rumble at the game's own frequency for the surface ---
    if (t.valid && t.frontOnGround > 0 && t.surfaceRumble > 0.0f && p.surface > 0.0f) {
        float hz = Clampf(t.surfaceHz > 0.0f ? t.surfaceHz : 25.0f, 8.0f, 60.0f);
        surfacePhase_ = Advance(surfacePhase_, hz, dt);
        float amp = p.surface * 0.5f * Clampf(t.surfaceRumble * 1.5f, 0.0f, 1.0f) * Ramp(1.0f, 8.0f, speed);
        e.surface = amp * (float)sin(surfacePhase_) * drive;
    }

    // --- kerbs and bumps: one new reading per physics tick ---
    if (t.seq != lastSeq_) {
        lastSeq_ = t.seq;
        if (t.valid && p.kerbs > 0.0f) {
            const float ref = 0.5f;   // m/s of quick suspension compression that is a solid hit
            float gate = Ramp(1.0f, 5.0f, speed);
            float kick = p.kerbs * 0.6f * tanhf((t.bump[1] - t.bump[0]) / ref) * gate;
            if (fabsf(kick) > fabsf(kerbKick_)) kerbKick_ = kick;
            float hit = fmaxf(fabsf(t.bump[0]), fabsf(t.bump[1]));
            float shake = p.kerbs * 0.4f * tanhf(fmaxf(hit - 0.15f, 0.0f) / ref) * gate;
            if (shake > kerbShake_) kerbShake_ = shake;
        }
    }
    kerbKick_ *= expf(-dt / 0.05f);
    kerbShake_ *= expf(-dt / 0.08f);
    e.kerb = (kerbKick_ + kerbShake_ * (float)sin(shakePhase_)) * drive;

    // --- crashes ---
    if (!haveHitSeq_) {
        haveHitSeq_ = true;
        lastHitSeq_ = t.hitSeq;
        lastBreakSeq_ = t.breakSeq;
    }

    // --- breakable props: a short, sharp knock, harder the faster the car hits ---
    if (t.breakSeq != lastBreakSeq_) {
        lastBreakSeq_ = t.breakSeq;
        float amp = p.breakables * Clampf(0.35f + 0.65f * tanhf(t.breakSpeed / 15.0f), 0.0f, 1.0f);
        if (amp > 0.01f) {
            breakStart_ = now;
            breakAmp_ = amp;
            breakDir_ = Clampf(t.breakLat, -1.0f, 1.0f);
        }
    }
    if (breakStart_ >= 0) {
        float tau = (float)(now - breakStart_);
        if (tau > 0.25f || tau < 0) {
            breakStart_ = -1;
        } else {
            // Toward the prop's side (the front wheel on that side takes it), then a quick rattle.
            float knock = tau < 0.05f ? sinf(3.14159265f * tau / 0.05f) : 0.0f;
            float rattle = 0.5f * expf(-tau / 0.06f) * sinf((float)(kTwoPi * 28.0) * tau);
            float dir = fabsf(breakDir_) > 0.2f ? (breakDir_ > 0 ? 1.0f : -1.0f) : 0.0f;
            e.breakable = breakAmp_ * (dir * knock + rattle) * drive;
        }
    }
    if (t.hitSeq != lastHitSeq_) {
        lastHitSeq_ = t.hitSeq;
        // Closing speed says how hard the hit was; the game's impulse says how heavy the thing was.
        // Light props (benches, posts) still give a clear knock from the closing speed alone.
        float amp = p.crashes * Clampf(0.45f * tanhf(t.hitSpeed / 8.0f) + 0.55f * tanhf(t.hitImpulse / 20.0f), 0.0f, 1.0f);
        if (amp > 0.02f) {
            hitStart_ = now;
            hitAmp_ = amp;
            hitDir_ = Clampf(t.hitLat, -1.0f, 1.0f);
        }
    }
    if (hitStart_ >= 0) {
        float tau = (float)(now - hitStart_);
        if (tau > 0.45f || tau < 0) {
            hitStart_ = -1;
        } else {
            float push = tau < 0.08f ? sinf(3.14159265f * tau / 0.08f) : 0.0f;
            float shake = 0.6f * expf(-tau / 0.12f) * sinf((float)(kTwoPi * 20.0) * tau);
            e.crash = hitAmp_ * (hitDir_ * push + shake) * drive;
        }
    }

    // --- scraping along a wall: the contact stays fresh while the reports keep coming ---
    float since = (float)(now - t.scrapeTime);
    if (t.scrapeTime > 0 && since >= 0 && since < 0.12f && p.scrapes > 0.0f && speed > 1.0f) {
        float env = Clampf((0.12f - since) / 0.05f, 0.0f, 1.0f);
        float amp = p.scrapes * tanhf(t.scrapeSpeed / 12.0f) * env;
        double tt = now;
        float grind = 0.6f * (float)sin(kTwoPi * 41.0 * tt) + 0.4f * (float)sin(kTwoPi * 67.0 * tt + 1.0);
        e.scrape = (amp * 0.5f * grind - Clampf(t.scrapeLat, -1.0f, 1.0f) * amp * 0.25f) * drive;
    }
    return e;
}

}  // namespace ffb
