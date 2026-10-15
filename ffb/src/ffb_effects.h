// Road and event effects layered over the steering forces: things you feel through the wheel that
// don't come from the tyres' grip.
//
//   engine    a vibration at the engine's firing frequency, mostly felt standing still
//   kerbs     a kick when one front wheel is hit from below (kerbs, bumps, potholes) toward that
//             side, and a short shake with it
//   surface   the game's own per-surface rumble (grass, gravel, dirt, rumble strips)
//   crashes   a jolt when the car hits something: other cars, walls, and props such as benches,
//             sign posts and fences; sideways from the side that was hit, then a short shake
//   scrapes   grinding while the car slides along a wall, and a pull toward it
//   offroad   a rough, uneven vibration while front wheels are on grass, dirt, mud, gravel...
//   carHits   the same kick and shake when the car hits another vehicle (traffic, cops, racers),
//             toward the side it was pushed, harder the faster they closed
//   landing   a hard kick and a short shake when the car lands after a jump (stronger with airtime)
//   breakables a sharp knock when the car smashes a breakable prop (bench, sign post, fence, bin),
//             toward the side it was on: the game's own Smackable objects, not any light contact
//
// All in shares of full scale, + = turns the wheel to the right. Pure: no DirectInput or globals.
#pragma once
#include "telemetry.h"

namespace ffb {

struct EffectParams {
    float engine = 0.05f;
    float kerbs = 0.5f;
    float surface = 0.3f;
    float crashes = 0.6f;
    float scrapes = 0.4f;
    float breakables = 0.6f;
    float landing = 0.8f;
    float offroad = 0.4f;
    float carHits = 0.8f;
};

struct EffectParts {
    float engine = 0, kerb = 0, surface = 0, crash = 0, scrape = 0, breakable = 0, offroad = 0, landing = 0, carHit = 0;
    float Sum() const { return engine + kerb + surface + crash + scrape + breakable + offroad + landing + carHit; }
};

class Effects {
public:
    // `drive` 0 (menus) .. 1 (driving), as for the model.
    EffectParts Step(const Telemetry& t, float drive, double now, float dt, const EffectParams& p);
private:
    double enginePhase_ = 0, surfacePhase_ = 0, shakePhase_ = 0, roughPhase_[2] = {0, 0};
    uint32_t lastSeq_ = 0, lastHitSeq_ = 0, lastBreakSeq_ = 0;
    bool haveHitSeq_ = false;
    double breakStart_ = -1;
    float offroadLevel_ = 0, bump_ = 0, bumpWait_ = 0;
    uint32_t rand_ = 12345;
    float breakAmp_ = 0, breakDir_ = 0;
    float airTime_ = 0, landAmp_ = 0, landDir_ = 1;
    double landStart_ = -1;
    uint32_t lastCarHitSeq_ = 0;
    float carAmp_ = 0, carDir_ = 0;
    double carStart_ = -1;
    float kerbKick_ = 0, kerbShake_ = 0;
    double hitStart_ = -1;
    float hitAmp_ = 0, hitDir_ = 0;
};

}  // namespace ffb
