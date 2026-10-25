// Road and event effects layered over the steering forces: things you feel through the wheel that
// don't come from the tyres' grip.
//
//   engine    a vibration at the engine's firing frequency, mostly felt standing still
//   kerbs     a kick when one front wheel is hit from below (kerbs, bumps, potholes) toward that
//             side, and a short shake with it (off by default)
//   crashes   a jolt when the car hits something: walls and props such as benches, sign posts and
//             fences; sideways from the side that was hit, then a short shake. An impact that
//             damages the car (as the gameplay mod counts them: Telemetry::damageSeq) raises the
//             crash or vehicle-hit jolt it came with, harder for harder hits, or starts one
//   scrapes   grinding while the car slides along a wall, and a pull toward it
//   offroad   a rough, uneven vibration while front wheels are on grass, dirt, mud, gravel...
//   carHits   the same kick and shake when the car hits another vehicle (traffic, cops, racers),
//             toward the side it was pushed; full strength from 150 km/h (as breakables)
//   landing   a hard kick and a short shake when the car lands after a jump (stronger with airtime)
//   breakables a sharp knock when the car smashes a breakable prop (bench, sign post, fence, bin),
//             toward the side it was on: the game's own Smackable objects, not any light contact
//   scrub     a vibration while any tyre is past its grip limit (the game's own traction)
//
// All in shares of full scale, + = turns the wheel to the right. Pure: no DirectInput or globals.
#pragma once
#include "telemetry.h"

namespace ffb {

struct EffectParams {
    float engine = 0.05f;
    float crashes = 0.6f;
    float scrapes = 0.4f;
    float breakables = 0.6f;
    float landing = 0.8f;
    float offroad = 0.4f;
    float carHits = 0.8f;
    float tyreScrub = 0.2f;  // vibration while any tyre is past its grip limit
    float kerbs = 0.0f;      // kick and shake when a front wheel is hit from below (kerbs, bumps)
};

struct EffectParts {
    float engine = 0, crash = 0, scrape = 0, breakable = 0, offroad = 0, landing = 0, carHit = 0;
    float scrub = 0, kerb = 0;
    float carHitLevel = 0;
    float crashLevel = 0;           // the crash jolt's strength, only for an impact that damaged the car
    float Sum() const { return engine + crash + scrape + breakable + offroad + landing + carHit + scrub + kerb; }
};

// How a jolt runs: a half-sine push toward its side lasting `push` s, under a shake of `shake`
// times its strength at `shakeHz` dying away with `shakeTau` s, all over after `length` s.
struct JoltShape { float push, shake, shakeTau, shakeHz, length; };

// One running jolt (crashes, breakable props, landings, vehicle hits).
struct Jolt {
    double start = -1;
    float amp = 0, dir = 0;         // strength, side (+1 right, -1 left, 0 shake only)
    void Start(double now, float a, float d) { start = now, amp = a, dir = d; }
    bool Fresh(double now) const { return start >= 0 && now - start < 0.3; }
    // The same impact reported again (the collision and the damage count arrive apart): a jolt
    // started in the last 0.3 s keeps going, as strong as the stronger report, else a new one.
    void Raise(double now, float a, float d) {
        if (!Fresh(now)) return Start(now, a, d);
        amp = amp > a ? amp : a;
        if (dir == 0) dir = d;
    }
    // The force now (0 once over); `level`: the strength as the shake dies away.
    float Step(double now, const JoltShape& s, float* level = nullptr);
};

class Effects {
public:
    // `drive` 0 (menus) .. 1 (driving), as for the model.
    EffectParts Step(const Telemetry& t, float drive, double now, float dt, const EffectParams& p);
private:
    double enginePhase_ = 0, roughPhase_[2] = {0, 0}, scrubPhase_ = 0, kerbPhase_ = 0;
    uint32_t lastSeq_ = 0;          // the telemetry sample the kerbs last read (one per physics step)
    float kerbKick_ = 0, kerbShake_ = 0;
    uint32_t lastHitSeq_ = 0, lastBreakSeq_ = 0, lastCarHitSeq_ = 0;
    bool haveHitSeq_ = false;
    float offroadLevel_ = 0, bump_ = 0, bumpWait_ = 0;
    uint32_t rand_ = 12345;
    float airTime_ = 0, landDir_ = 1;
    uint32_t lastDamageSeq_ = 0;
    bool crashDamaged_ = false;     // the running crash jolt is for an impact that damaged the car
    Jolt crash_, break_, land_, carHit_;
};

}  // namespace ffb
