// The FFB loop, at [FFB] UpdateHz (400 by default):
//
//   1. the latest car sample (telemetry, about 60 Hz) and the wheel's position and speed
//   2. the force model: aligning torque, centring, damping, friction, soft lock
//   3. the safety chain:
//        the aligning torque (driven by the game's physics steps) through a low-pass
//        the oscillation guard: more damping and less gain while the wheel shakes by itself
//        master gain, soft limiter (no clipping) scaled to MaxForce, slew-rate limit
//        fade-in after the force (re)starts, a final clamp and a guard against invalid numbers
//   4. the device sends it as one constant force
#include "ffb.h"
#include "ffb_device.h"
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
    float aligningSmoothed = 0, guard = 0, raw = 0, limited = 0, out = 0;
};

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
        p.centering = c.centering;
        p.centeringParked = c.centeringParked;
        p.centeringSpeed = c.centeringSpeed;
        p.damping = c.damping;
        p.friction = c.friction;
        p.parkingFriction = c.parkingFriction;
        p.softLock = c.softLock;
        p.lockShare = c.softLockAt;
        p.menuCentering = c.menuCentering;
        s.parts = model_.Compute(t, w, p, drive_, dt);

        // The aligning torque follows the game's physics steps: smooth it. The other parts follow
        // the wheel itself at full rate.
        s.aligningSmoothed = alignFilter_.Step(s.parts.aligning, dt, c.smoothingHz);

        // A wheel shaking by itself: up to triple the damping and half the gain until it settles.
        s.guard = c.oscillationGuard ? guard_.Step(w.vel, dt) : 0.0f;
        if (guard_.tripped) Log("ffb: wheel oscillating - damping raised until it settles");
        float steering = s.aligningSmoothed + s.parts.centering + s.parts.damping * (1.0f + 2.0f * s.guard) + s.parts.friction;
        s.raw = steering * c.gain * (1.0f - 0.5f * s.guard) + s.parts.softLock;

        // No clipping: peaks are compressed above the knee, never cut flat, then scaled to MaxForce.
        s.limited = SoftLimit(s.raw, c.limitKnee) * c.maxForce;
        float slewed = slew_.Step(s.limited, dt, c.slewRate);
        float fade = Clampf((float)(now - startTime) / 0.5f, 0.0f, 1.0f);
        s.out = isfinite(slewed) ? Clampf(slewed * fade, -c.maxForce, c.maxForce) : 0.0f;
        if (!isfinite(slewed)) slew_.Reset(0);

        // Statistics for the log: how often the soft limiter is working.
        ticks_++;
        if (fabsf(s.raw) > c.limitKnee) limiting_++;
        return s;
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

private:
    Model model_;
    LowPass2 alignFilter_, velFilter_;
    SlewLimiter slew_;
    OscillationGuard guard_;
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
            Log("tel: drv=%.2f v=%.1f vlong=%.1f vlatF=%.2f yaw=%.2f loadF=%.0f(ref) onF=%d wheel=%.3f/%.2f | align=%.3f(%.3f) "
                "centre=%.3f damp=%.3f fric=%.3f lock=%.3f guard=%.2f raw=%.3f out=%.3f",
                engine.Drive(), t.speed, t.vLong, t.vLatFront, t.yawRate, t.frontLoad, t.frontOnGround, pos, engine.WheelVel(),
                s.parts.aligning, s.aligningSmoothed, s.parts.centering, s.parts.damping, s.parts.friction, s.parts.softLock,
                s.guard, s.raw, s.out);
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
