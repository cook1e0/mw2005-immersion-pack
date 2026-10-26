// The FFB loop, at [FFB] UpdateHz (400 by default):
//
//   1. the latest car sample (telemetry, about 60 Hz), its steering angles carried on to now at
//      the rate they last changed, and the wheel's position and speed
//   2. the force model: aligning torque, rear slide, centring, damping, friction, soft lock; and the effects:
//      engine, crashes, scrapes, breakables, landings, vehicle hits, off-road, tyre scrub
//   3. the safety chain:
//        the aligning torque (driven by the game's physics steps) through a low-pass
//        the oscillation guard: more damping and less gain while the wheel shakes by itself
//        master gain, soft limiter (no clipping) scaled to MaxForce, slew-rate limit
//        the effects added after the slew limit (their vibrations are quicker than it allows),
//        through their own soft limit
//        fade-in after the force (re)starts, a final clamp and a guard against invalid numbers
//   4. the device sends it as one constant force
#include "ffb.h"
#include "ffb_device.h"
#include "ffb_effects.h"
#include "ffb_filters.h"
#include "ffb_model.h"
#include "telemetry.h"
#include <math.h>
#include <mmsystem.h>
#include <algorithm>

namespace ffb {

// Everything the engine computes in one step, for the tests and the telemetry log.
struct Step {
    ForceParts parts;
    EffectParts effects;
    float aligningSmoothed = 0, rearSmoothed = 0, guard = 0, raw = 0, limited = 0, out = 0;
    float hitPriority = 0;          // how far a vehicle hit or damaging crash ducks the other effects (0..1)
};

// The effects added up, the others ducked under a vehicle hit while it plays so it comes through.
float PrioritizedEffectSum(const EffectParts& effects, float* priorityOut = nullptr) {
    // A traffic hit and a crash that damaged the car are what the player has to feel: while they
    // last, the other transient effects step back.
    float priority = Clampf(fmaxf(effects.carHitLevel, effects.crashLevel) * 4.0f, 0.0f, 1.0f);
    if (priorityOut) *priorityOut = priority;
    float loud = effects.carHit + (effects.crashLevel > 0.0f ? effects.crash : 0.0f);
    return loud + (effects.Sum() - loud) * (1.0f - priority);
}

// The engine's state. One instance runs on the FFB thread; the tests make their own.
class Engine {
public:
    // One update: the car, the wheel's position (if known) and the time since the last update.
    // Returns the torque to send (+ = right, before the device's direction).
    Step Update(const Telemetry& t, bool wheelKnown, float wheelPos, double now, float dt, double startTime) {
        const Config& c = g_cfg;
        Step s;
        dt = Clampf(dt, 0.0005f, 0.05f);

        // The wheel's speed, from its position (lightly smoothed: the readings are quantised).
        WheelState w;
        w.known = wheelKnown;
        if (wheelKnown) {
            if (haveWheel_) velRaw_ = (wheelPos - lastPos_) / dt;
            lastPos_ = wheelPos;
            haveWheel_ = true;
            wheelVel_ = velFilter_.Step(velRaw_, dt, 30.0f);
            w.pos = wheelPos;
            w.vel = wheelVel_;
            w.velSmooth = velSmoothFilter_.Step(velRaw_, dt, 15.0f);
        } else {
            haveWheel_ = false;
        }

        // Driving (the player has the car) vs. menus, eased so switching doesn't jolt the wheel.
        bool driving = t.driving && t.valid && now - t.time < 0.25;
        drive_ = Clampf(drive_ + (driving ? dt / 0.3f : -dt / 0.15f), 0.0f, 1.0f);

        ModelParams p;
        p.aligning = c.aligning;
        p.gripPeakDeg = c.gripPeakDeg;
        p.driftAlign = c.driftAlign;
        p.loadSensitivity = c.loadSensitivity;
        p.carWeight = c.carWeight;
        p.caster = c.caster;
        p.frontGripLoss = c.frontGripLoss;
        p.centering = c.centering;
        p.damping = c.damping;
        p.friction = c.friction;
        p.parkingFriction = c.parkingFriction;
        p.softLock = c.softLock;
        p.lockShare = c.softLockAt;
        p.menuCentering = c.menuCentering;
        p.rearSlide = c.rearSlide;
        s.parts = model_.Compute(Predict(t, now), w, p, drive_, dt);

        EffectParams ep;
        ep.engine = c.engineRumble;
        ep.crashes = c.crashes;
        ep.scrapes = c.scrapes;
        ep.breakables = c.breakables;
        ep.landing = c.landing;
        ep.carHits = c.carHits;
        ep.offroad = c.offroadVibration;
        ep.kerbs = c.kerbs;
        ep.surface = c.surfaceRumble;
        ep.tyreScrub = c.tyreScrub;
        s.effects = effects_.Step(t, drive_, now, dt, ep);
        float fx = PrioritizedEffectSum(s.effects, &s.hitPriority);

        // The aligning torque follows the game's physics steps: smooth it. The other parts follow
        // the wheel itself at full rate.
        s.aligningSmoothed = alignFilter_.Step(s.parts.aligning, dt, c.smoothingHz);
        s.rearSmoothed = rearFilter_.Step(s.parts.rearSlide, dt, c.smoothingHz);

        // A wheel shaking by itself: up to triple the damping and half the gain until it settles.
        // (The effects shake the wheel on purpose: the guard doesn't watch it while they do.)
        s.guard = c.oscillationGuard ? guard_.Step(fabsf(fx) > 0.03f ? 0.0f : w.vel, dt) : 0.0f;
        if (guard_.tripped) Log("ffb: wheel oscillating - damping raised until it settles");
        float steering = s.aligningSmoothed + s.rearSmoothed + s.parts.caster + s.parts.centering + s.parts.damping * (1.0f + 2.0f * s.guard) + s.parts.friction;
        s.raw = steering * c.gain * (1.0f - 0.5f * s.guard) + s.parts.softLock;

        // No clipping: peaks are compressed above the knee, never cut flat, then scaled to MaxForce.
        s.limited = SoftLimit(s.raw, c.limitKnee) * c.maxForce;
        float slewed = slew_.Step(s.limited, dt, c.slewRate);
        float fade = Clampf((float)(now - startTime) / 0.5f, 0.0f, 1.0f);
        float total = slewed + SoftLimit(fx * c.gain, c.limitKnee) * c.maxForce;
        s.out = isfinite(total) ? Clampf(total * fade, -c.maxForce, c.maxForce) : 0.0f;
        if (!isfinite(slewed)) slew_.Reset(0);
        // Minimum force: a gear or belt wheel doesn't move at all below a few percent of its torque
        // (its own friction), so light forces, the wheel going light and the force changing sides
        // are lost in a dead zone. Every force is lifted above it, scaled so MaxForce stays the
        // top, and eased in over the first 2% so the change of side stays smooth.
        float minForce = Clampf(c.minForce, 0.0f, 0.5f * c.maxForce);
        if (minForce > 0.0f) {
            float a = fabsf(s.out);
            s.out = Sign(s.out) * (minForce * Clampf(a / 0.02f, 0.0f, 1.0f) + a * (1.0f - minForce / c.maxForce));
        }

        // Statistics for the log: how often the soft limiter is working.
        ticks_++;
        if (fabsf(s.raw) > c.limitKnee) limiting_++;
        return s;
    }

    // The car sample carried on to `now`: the front wheels' angle and the front tyres' slip angles
    // (where the front axle travels) at the rate they changed between the last two samples, for up
    // to one sample's time. The game steps its physics at about 60 Hz; held still in between, the
    // tyres' pull moved in steps and needed more smoothing, and with it more lag.
    Telemetry Predict(const Telemetry& t, double now) {
        if (t.seq != pred_.seq) {
            double gap = t.time - pred_.time;
            bool continuous = pred_.seq && t.seq == pred_.seq + 1 && gap > 0.002 && gap < 0.1 && t.valid &&
                              t.frontSlipKnown && pred_.slipKnown && t.frontEachKnown == pred_.eachKnown;
            auto rate = [&](float cur, float prev) {
                return continuous ? Clampf(cur - prev, -8.0f, 8.0f) / (float)gap : 0.0f;
            };
            pred_.steerRate = rate(t.steerDeg, pred_.steer);
            pred_.slipRate = rate(t.frontSlipDeg, pred_.slip);
            for (int i = 0; i < 2; ++i) pred_.eachRate[i] = rate(t.frontSlipEachDeg[i], pred_.each[i]);
            pred_.horizon = continuous ? fminf((float)gap, 0.033f) : 0.0f;
            pred_.seq = t.seq;
            pred_.time = t.time;
            pred_.steer = t.steerDeg;
            pred_.slip = t.frontSlipDeg;
            pred_.each[0] = t.frontSlipEachDeg[0];
            pred_.each[1] = t.frontSlipEachDeg[1];
            pred_.slipKnown = t.frontSlipKnown;
            pred_.eachKnown = t.frontEachKnown;
        }
        Telemetry p = t;
        float h = Clampf((float)(now - t.time), 0.0f, pred_.horizon);
        p.steerDeg += pred_.steerRate * h;
        p.frontSlipDeg += pred_.slipRate * h;
        for (int i = 0; i < 2; ++i) p.frontSlipEachDeg[i] += pred_.eachRate[i] * h;
        return p;
    }

    // Called when no force can be sent (no device, focus lost): start from zero next time.
    void Idle() {
        slew_.Reset(0);
        haveWheel_ = false;
    }

    int Ticks() const { return ticks_; }
    int Limiting() const { return limiting_; }
    void ResetStats() { ticks_ = limiting_ = 0; }
    float Drive() const { return drive_; }
    float WheelVel() const { return wheelVel_; }
    float RoadLock() const { return model_.RoadLock(); }
    float LoadRef() const { return model_.LoadRef(); }
    float FrontGrip() const { return model_.FrontGrip(); }
    float GripBase() const { return model_.GripBase(); }
    float GripLight() const { return model_.GripLight(); }

private:
    Model model_;
    Effects effects_;
    LowPass2 alignFilter_, rearFilter_, velFilter_, velSmoothFilter_;
    SlewLimiter slew_;
    OscillationGuard guard_;
    struct {
        uint32_t seq = 0;
        double time = 0;
        float steer = 0, slip = 0, each[2] = {}, horizon = 0;
        float steerRate = 0, slipRate = 0, eachRate[2] = {};
        bool slipKnown = false, eachKnown = false;
    } pred_;
    float drive_ = 0, lastPos_ = 0, velRaw_ = 0, wheelVel_ = 0;
    bool haveWheel_ = false;
    int ticks_ = 0, limiting_ = 0;
};

}  // namespace ffb

namespace {

volatile LONG s_started, s_resetRequested, s_testRequested, s_recheckRequested;

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI FfbThread(LPVOID) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ffb::Engine engine;
    const double period = 1.0 / g_cfg.updateHz;
    double next = Now(), last = Now(), statTime = Now(), logTime = 0, testUntil = -1;
    bool keyDown = false;
    for (;;) {
        double now = Now();
        if (now < next) {
            Sleep(next - now > 0.002 ? 1 : 0);
            continue;
        }
        next += period;
        if (next < now) next = now + period;
        float dt = (float)(now - last);
        last = now;

        Telemetry t;
        Telemetry_Get(&t);
        bool focused = GameFocused();
        bool down = g_cfg.keyReset && (GetAsyncKeyState(g_cfg.keyReset) & 0x8000);
        if (focused && down && !keyDown) Ffb_RequestReset();
        keyDown = down;
        bool manual = focused && InterlockedExchange(&s_resetRequested, 0) != 0;
        bool recheck = InterlockedExchange(&s_recheckRequested, 0) != 0;

        EnterCriticalSection(&g_diLock);
        if (recheck) ffb::Device_Recheck();
        DeviceWrap* dev = ffb::Device_Service(now, focused, manual);
        ffb::Device_Fallback(now, manual);
        float pos = 0;
        bool known = dev && ffb::Device_ReadWheel(dev, now, &pos);
        double startTime = ffb::Device_StartTime();
        LeaveCriticalSection(&g_diLock);

        if (!dev) {
            engine.Idle();
            continue;
        }
        ffb::Step s = engine.Update(t, known, pos, now, dt, startTime);
        float out = s.out;
        if (InterlockedExchange(&s_testRequested, 0)) {
            testUntil = now + 0.5;
            Log("ffb: test push to the right at wheel position %.3f", pos);
        }
        if (now < testUntil) out = 0.3f * g_cfg.maxForce;

        EnterCriticalSection(&g_diLock);
        ffb::Device_Send(out, now);
        LeaveCriticalSection(&g_diLock);

        if (g_cfg.logTelemetry && now - logTime > 0.1) {
            logTime = now;
            Log("tel: drv=%.2f v=%.1f vlong=%.1f vlatF=%.2f vlatR=%.2f yaw=%.2f loadF=%.0f(%.0f/%.0f) slipF=%.1f/%.1f onF=%d wheel=%.3f/%.2f roadlock=%.1f(%.1f/%.1f) travel=%.2f "
                "rpm=%.0f loadRef=%.0f gripF=%.2f(%.2f) light=%.2f | align=%.3f(%.3f) rear=%.3f caster=%.3f centre=%.3f damp=%.3f fric=%.3f lock=%.3f "
                "fx: eng=%.3f crash=%.3f scrape=%.3f break=%.3f car=%.3f scrub=%.3f kerb=%.3f road=%.3f priority=%.2f | guard=%.2f raw=%.3f out=%.3f",
                engine.Drive(), t.speed, t.vLong, t.vLatFront, t.vLatRear, t.yawRate, t.frontLoad,
                t.frontLoadEach[0], t.frontLoadEach[1], t.frontSlipEachDeg[0], t.frontSlipEachDeg[1], t.frontOnGround, pos, engine.WheelVel(),
                engine.RoadLock(), t.lockDeg, t.steerDeg, ffb::FrontTravel(t) * 57.29578f, t.rpm, engine.LoadRef(), engine.FrontGrip(), engine.GripBase(), engine.GripLight(),
                s.parts.aligning, s.aligningSmoothed, s.rearSmoothed, s.parts.caster, s.parts.centering, s.parts.damping, s.parts.friction, s.parts.softLock,
                s.effects.engine, s.effects.crash, s.effects.scrape, s.effects.breakable,
                s.effects.carHit, s.effects.scrub, s.effects.kerb, s.effects.surface, s.hitPriority, s.guard, s.raw, s.out);
        }
        if (now - statTime > 30.0) {
            if (engine.Limiting() > engine.Ticks() / 10)
                Log("ffb: the soft limiter was compressing %d%% of the time - consider lowering Gain",
                    engine.Limiting() * 100 / std::max(engine.Ticks(), 1));
            engine.ResetStats();
            statTime = now;
        }
    }
    return 0;
}

}  // namespace

void Ffb_Start() {
    HANDLE thread = CreateThread(nullptr, 0, FfbThread, nullptr, 0, nullptr);
    if (!thread) {
        Log("ffb: cannot start the force feedback thread (%lu)", GetLastError());
        return;
    }
    InterlockedExchange(&s_started, 1);
    CloseHandle(thread);
    Log("ffb: engine started at %d Hz", g_cfg.updateHz);
}

bool Ffb_RequestReset() {
    if (!g_cfg.enabled || g_cfg.nativeFfb || !InterlockedCompareExchange(&s_started, 0, 0)) return false;
    InterlockedExchange(&s_resetRequested, 1);
    return true;
}

bool Ffb_RequestTest() {
    if (!g_cfg.enabled || g_cfg.nativeFfb || !InterlockedCompareExchange(&s_started, 0, 0)) return false;
    InterlockedExchange(&s_testRequested, 1);
    return true;
}

void Ffb_SetMenuOpen(bool) {
    if (!g_cfg.enabled || g_cfg.nativeFfb) return;
    InterlockedExchange(&s_recheckRequested, 1);
}
