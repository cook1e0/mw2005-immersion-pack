// Signal conditioning between the force model and the wheel motor.
//
// A direct drive base follows whatever it is told within a millisecond. The game's physics run at
// about 60 Hz, so its forces arrive as steps; sent as they are, every step is a knock in your hands,
// and a strong force that reacts to the wheel's own movement can start a feedback loop that shakes
// the wheel by itself (the "death rattle"). These filters sit between the model and the motor:
//
//   LowPass2          smooths the game-rate physics forces into a continuous torque
//   SoftLimit         compresses peaks instead of flat-topping them at the motor's maximum (clipping)
//   SlewLimiter       caps how fast the torque may change, so no update is a hammer blow
//   OscillationGuard  spots the wheel shaking left-right on its own and calms it down
//
// Header-only and free of Windows calls, so the tests can drive them directly.
#pragma once
#include <math.h>

namespace ffb {

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Critically damped second-order low-pass. Removes the steps of the game's physics rate with less
// lag than a one-pole filter of the same smoothness, and without overshoot.
struct LowPass2 {
    float y = 0, v = 0;
    bool primed = false;
    void Reset(float x) { y = x; v = 0; primed = true; }
    float Step(float x, float dt, float hz) {
        if (!primed) { Reset(x); return y; }
        if (!(dt > 0)) return y;
        float w = 2.0f * 3.14159265f * hz;
        // y'' = w^2 (x - y) - 2 w y', integrated in sub-steps small enough to stay stable
        int n = (int)ceilf(dt * w / 0.25f);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        float h = dt / n;
        for (int i = 0; i < n; i++) {
            v += h * (w * w * (x - y) - 2.0f * w * v);
            y += h * v;
        }
        return y;
    }
};

// Soft limiter: linear up to `knee`, then bending smoothly toward 1 without ever reaching it.
// Strong forces keep their shape and stay distinguishable instead of all sitting at the limit.
inline float SoftLimit(float x, float knee) {
    float a = fabsf(x);
    if (a <= knee) return x;
    float room = 1.0f - knee;
    float out = knee + room * tanhf((a - knee) / room);
    return x < 0 ? -out : out;
}

// Caps the rate of change of the output, in full-scale units per second.
struct SlewLimiter {
    float y = 0;
    void Reset(float x) { y = x; }
    float Step(float x, float dt, float rate) {
        float d = rate * dt;
        y += Clampf(x - y, -d, d);
        return y;
    }
};

// A wheel oscillating on its own: its velocity keeps reversing quickly while it moves fast. Each
// reversal above `minVel` within the last `window` seconds counts; from `flips` reversals on, the
// guard is active for `hold` seconds. Step returns 0 (calm) .. 1 (oscillating), easing back down,
// for the engine to add damping and lower the gain.
struct OscillationGuard {
    float minVel = 1.0f;        // wheel speed (share of rotation per second) that counts
    float window = 0.5f;
    int   flips = 4;
    float hold = 1.0f;
    // state
    float times[16] = {};
    int   count = 0;
    int   lastSign = 0;
    float clock = 0, activeUntil = -1, level = 0;
    bool  tripped = false;      // set on the step it becomes active (for logging)

    float Step(float wheelVel, float dt) {
        clock += dt;
        tripped = false;
        int sign = wheelVel > minVel ? 1 : (wheelVel < -minVel ? -1 : 0);
        if (sign && lastSign && sign != lastSign) {
            if (count < 16) times[count++] = clock;
            else { for (int i = 1; i < 16; i++) times[i - 1] = times[i]; times[15] = clock; }
        }
        if (sign) lastSign = sign;
        int recent = 0;
        for (int i = 0; i < count; i++) recent += clock - times[i] <= window;
        if (recent >= flips) {
            if (clock > activeUntil) tripped = true;
            activeUntil = clock + hold;
        }
        float target = clock <= activeUntil ? 1.0f : 0.0f;
        level += (target - level) * Clampf(dt / (target > level ? 0.02f : 0.4f), 0.0f, 1.0f);
        return level;
    }
};

}  // namespace ffb
