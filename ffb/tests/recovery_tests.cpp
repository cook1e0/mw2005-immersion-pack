// Production FFB engine and wrapper against an in-memory driver. No hardware,
// game process, foreground-window manipulation or FFB worker thread is used.
#include "../src/dinput_wrap.cpp"
#include "../src/ffb.cpp"
#include <assert.h>
#include <algorithm>
#include <stdio.h>
#include <vector>
Config g_cfg;
static double testTime = 1;
double Now() { return testTime; }
void Log(const char*, ...) {}
void SaveCalibration(int, int, int) {}
void SaveForceDirection(int) {}
void Telemetry_Get(Telemetry* out) { memset(out, 0, sizeof(*out)); }

struct Driver;
struct TestEffect : IDirectInputEffect {
    Driver* driver;
    bool playing = false, downloaded = true;
    HRESULT startResult = DI_OK, parameterResult = DI_OK, statusResult = DI_OK;
    LONG magnitude = 0;
    TestEffect(Driver* d) : driver(d) {}
    STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(GetEffectGuid)(GUID*) override { return DI_OK; }
    STDMETHOD(GetParameters)(DIEFFECT*, DWORD) override { return DI_OK; }
    STDMETHOD(SetParameters)(const DIEFFECT*, DWORD) override;
    STDMETHOD(Start)(DWORD, DWORD) override;
    STDMETHOD(Stop)() override { playing = false; return DI_OK; }
    STDMETHOD(GetEffectStatus)(DWORD* status) override {
        if (!downloaded) return DIERR_NOTDOWNLOADED;
        *status = playing ? DIEGES_PLAYING : 0;
        return statusResult;
    }
    STDMETHOD(Download)() override { downloaded = true; return DI_OK; }
    STDMETHOD(Unload)() override { downloaded = playing = false; return DI_OK; }
    STDMETHOD(Escape)(DIEFFESCAPE*) override { return DI_OK; }
};
struct Driver {
    void** vtable;
    bool acquired = false, constantOnly = false;
    HRESULT acquireResult = DI_OK, stateResult = DI_OK, pollResult = DI_OK;
    DWORD state = DIGFFS_ACTUATORSON | DIGFFS_POWERON | DIGFFS_SAFETYSWITCHON | DIGFFS_USERFFSWITCHON;
    int acquires = 0, unacquires = 0, creates = 0, releases = 0, starts = 0, enables = 0;
    std::vector<TestEffect*> effects;
    void StopAll() { for (auto* e : effects) e->playing = false; }
};
ULONG TestEffect::Release() {
    auto& list = driver->effects;
    list.erase(std::find(list.begin(), list.end(), this));
    ++driver->releases; delete this; return 0;
}
HRESULT TestEffect::SetParameters(const DIEFFECT* effect, DWORD) {
    if (!driver->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    if (parameterResult == DIERR_INPUTLOST) driver->acquired = false;
    if (parameterResult != DI_OK) return parameterResult;
    magnitude = *(const LONG*)effect->lpvTypeSpecificParams;
    downloaded = true;
    return DI_OK;
}
HRESULT TestEffect::Start(DWORD iterations, DWORD flags) {
    assert(iterations == 1 && flags == 0); // Must allow automatic download.
    ++driver->starts;
    assert(magnitude == 0); // Old driving torque must be cleared before restart.
    if (!driver->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    if (startResult == DIERR_INPUTLOST) driver->acquired = false;
    if (FAILED(startResult)) return startResult;
    downloaded = playing = true;
    return DI_OK;
}
static Driver* D(IDirectInputDevice8A* self) { return (Driver*)self; }
static ULONG WINAPI ReleaseDevice(IDirectInputDevice8A*) { return 0; }
static HRESULT WINAPI AcquireDevice(IDirectInputDevice8A* self) {
    Driver* d = D(self); ++d->acquires;
    if (SUCCEEDED(d->acquireResult)) d->acquired = true;
    return d->acquireResult;
}
static HRESULT WINAPI UnacquireDevice(IDirectInputDevice8A* self) {
    Driver* d = D(self); ++d->unacquires; d->acquired = false; d->StopAll(); return DI_OK;
}
static HRESULT WINAPI Property(IDirectInputDevice8A*, REFGUID, const DIPROPHEADER*) { return DI_OK; }
static HRESULT WINAPI Range(IDirectInputDevice8A*, REFGUID, DIPROPHEADER* p) {
    auto* range = (DIPROPRANGE*)p; range->lMin = -10000; range->lMax = 10000; return DI_OK;
}
static HRESULT WINAPI DeviceState(IDirectInputDevice8A* self, DWORD size, void* out) {
    if (!D(self)->acquired) return DIERR_NOTACQUIRED;
    memset(out, 0, size); return DI_OK;
}
static HRESULT WINAPI PollDevice(IDirectInputDevice8A* self) { return D(self)->pollResult; }
static HRESULT WINAPI ForceState(IDirectInputDevice8A* self, DWORD* out) {
    Driver* d = D(self);
    if (!d->acquired) return DIERR_NOTACQUIRED;
    *out = d->state; return d->stateResult;
}
static HRESULT WINAPI Command(IDirectInputDevice8A* self, DWORD command) {
    Driver* d = D(self);
    if (!d->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    if (command == DISFFC_SETACTUATORSON) {
        ++d->enables; d->state &= ~DIGFFS_ACTUATORSOFF; d->state |= DIGFFS_ACTUATORSON;
    }
    if (command == DISFFC_CONTINUE) d->state &= ~DIGFFS_PAUSED;
    return DI_OK;
}
static HRESULT WINAPI Caps(IDirectInputDevice8A*, LPDIDEVCAPS caps) {
    caps->dwAxes = 1; caps->dwButtons = 0; caps->dwFlags = DIDC_FORCEFEEDBACK;
    return DI_OK;
}
static HRESULT WINAPI Create(IDirectInputDevice8A* self, REFGUID kind, const DIEFFECT*, IDirectInputEffect** out, IUnknown*) {
    Driver* d = D(self);
    if (!d->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    if (d->constantOnly && kind != GUID_ConstantForce) return DIERR_UNSUPPORTED;
    ++d->creates;
    auto* fx = new TestEffect(d); d->effects.push_back(fx); *out = fx; return DI_OK;
}
static DeviceWrap* Open(Driver& driver) {
    auto* d = new DeviceWrap((IDirectInputDevice8A*)&driver);
    strcpy(d->name, "Test wheel");
    d->exclusive = d->acquireWanted = true;
    d->axisOfs = 0; d->dataSize = sizeof(LONG);
    Ffb_RegisterDevice(d);
    return d;
}
static DeviceWrap* Service(double now, bool focus = true, bool manual = false) {
    testTime = now; return ServiceEffects(now, focus, manual);
}
// The force model on synthetic telemetry (driving straight at speed, wheel turned right).
static Telemetry Driving(double now, uint32_t seq) {
    Telemetry t;
    memset(&t, 0, sizeof(t));
    t.seq = seq; t.time = now; t.driving = t.valid = true;
    t.speed = t.fwdSpeed = 30.0f;
    t.onGround[0] = t.onGround[1] = true;
    t.load[0] = t.load[1] = 4000;
    t.wheelsOnGround = 4;
    t.wheelbase = 2.6f;
    t.rpm = 4000; t.rpmNorm = 0.5f;
    return t;
}

static float Settle(Telemetry t, double& now, uint32_t& seq, int ticks = 60) {
    Out o;
    for (int i = 0; i < ticks; i++) {
        now += 0.005; t.seq = ++seq; t.time = now;
        Compute(t, now, 0.005f, o);
    }
    return o.constant;
}

static void TestForceModel() {
    g_cfg = Config();
    g_cfg.satGain = 0; g_cfg.roadGain = 0; g_cfg.centerSpring = 0; g_cfg.surfaceRumble = 0;
    S = State();
    S.forceSign = 1; S.slip.sign = 1; S.right.sign = 1;
    S.haveAxis = true; S.axis = 0.4f;
    double now = 100;
    uint32_t seq = 0;
    Telemetry t = Driving(now, seq);

    // Speed-sensitive centring pulls a wheel turned right back to the left, more at speed.
    float fast = Settle(t, now, seq);
    assert(fast < -0.05f);
    t.speed = t.fwdSpeed = 8.0f;
    float slow = Settle(t, now, seq);
    assert(slow < 0 && slow > fast);
    t.speed = t.fwdSpeed = 30.0f;

    // Understeer: front slip far past the peak and the car not turning: lighter.
    t.slipAngle[0] = t.slipAngle[1] = 0.2f;     // ~11 degrees, peak 6
    t.steer = 0.1f; t.yawRate = 0.1f;
    float pushing = Settle(t, now, seq, 120);
    assert(fabsf(pushing) < fabsf(fast) * 0.7f);
    t.slipAngle[0] = t.slipAngle[1] = 0; t.steer = 0; t.yawRate = 0;
    Settle(t, now, seq, 120);

    // Airborne: the wheel goes limp; touching down kicks it.
    t.airborne = true; t.wheelsOnGround = 0;
    float air = Settle(t, now, seq);
    assert(fabsf(air) < fabsf(fast) * 0.2f);
    t.airborne = false; t.wheelsOnGround = 4; t.landSeq = 1; t.landSpeed = 5.0f;
    Out o;
    now += 0.005; t.seq = ++seq; t.time = now;
    Compute(t, now, 0.005f, o);
    assert(S.landTime == now && S.landAmp > 0.3f);
    S.landTime = -1;

    // A kerb under the right front wheel pushes the wheel left (away from it).
    S.axis = 0; Settle(t, now, seq);
    t.vertJerk = 2000; t.compAccel[1] = 50; t.compAccel[0] = 2;
    now += 0.005; t.seq = ++seq; t.time = now;
    Compute(t, now, 0.005f, o);
    t.vertJerk = 0; t.compAccel[0] = t.compAccel[1] = 0;
    now += 0.03; t.seq = ++seq; t.time = now;
    Compute(t, now, 0.005f, o);
    assert(S.curbSide == 1 && o.constant < -0.05f);

    g_cfg = Config();
}

// The direction calibration against simulated drivers: how the wheel accelerates for a force
// `v` sent the calibration's way (0 = signed magnitude, 1 = direction + size).
enum SimDriver { STANDARD, REVERSED, SIZE_ONLY_LEFT, DEAD };
static float Physical(SimDriver d, float v, int mode) {
    switch (d) {
    case STANDARD: return v;
    case REVERSED: return -v;
    case SIZE_ONLY_LEFT: return mode ? -v : -fabsf(v);   // ignores a negative magnitude; + pushes left
    default: return 0;
    }
}

static int Calibrate(SimDriver driver, int* mode) {
    S = State();
    S.haveAxis = true; S.axis = 0.1f;
    g_cfg.forceDirection = 0;
    IDirectInputEffect* savedConstant = s_fx.constant;
    s_fx.constant = (IDirectInputEffect*)1;
    s_fx.recoveryTime = 0;
    double now = 50;
    float vel = 0;
    Out o;
    for (int i = 0; i < 2000 && (S.pulsePhase || !S.forceSign) && S.pulseTries < 2; i++) {
        now += 0.005;
        bool owns = PulseTest(now, false, o);
        float v = owns ? o.constant : 0.0f;
        int sent = s_calSendMode >= 0 ? s_calSendMode : 0;
        // a light, damped wheel with a slow drift
        vel += (Physical(driver, v, sent) * 6.0f - vel * 4.0f) * 0.005f;
        S.axis += (vel + 0.01f) * 0.005f;
        S.axisVel = vel;
    }
    s_fx.constant = savedConstant;
    *mode = g_cfg.forceDirection;
    return S.forceSign;
}

static void TestCalibration() {
    int mode = -1;
    s_started = 1;
    assert(Calibrate(STANDARD, &mode) == 1 && mode == 0);
    assert(Calibrate(REVERSED, &mode) == -1 && mode == 0);
    assert(Calibrate(SIZE_ONLY_LEFT, &mode) == -1 && mode == 1);
    assert(Calibrate(DEAD, &mode) == 0);

    // The centring spring is the mod's own and waits for the direction; the wheel's spring effect
    // is never used.
    g_cfg = Config();
    g_cfg.centerSpring = 0.5f;
    S = State();
    S.slip.sign = 1; S.right.sign = 1;
    S.haveAxis = true; S.axis = -0.25f;
    double now = 300;
    uint32_t seq = 0;
    Telemetry t = Driving(now, seq);
    t.speed = t.fwdSpeed = 83.0f;
    Out o;
    for (int i = 0; i < 60; i++) { now += 0.005; t.seq = ++seq; t.time = now; Compute(t, now, 0.005f, o); }
    float before = o.constant;   // no spring in it yet (sent as 0 anyway until the direction is known)
    assert(o.spring == 0 && o.damper == 0);
    S.forceSign = 1;
    for (int i = 0; i < 60; i++) { now += 0.005; t.seq = ++seq; t.time = now; Compute(t, now, 0.005f, o); }
    assert(o.spring == 0 && o.constant > before + 0.2f);   // wheel turned left at 300 km/h: pushed back right
    float moves[4] = {0.02f, 0.02f, -0.02f, 0.02f};
    assert(DecideForce(moves, &mode) == -1 && mode == 1);
    g_cfg = Config();
    S = State();
}

int main() {
    InitializeCriticalSection(&g_diLock);
    void* vt[32] = {};
    vt[2] = (void*)&ReleaseDevice; vt[3] = (void*)&Caps; vt[5] = (void*)&Range; vt[6] = (void*)&Property;
    vt[7] = (void*)&AcquireDevice; vt[8] = (void*)&UnacquireDevice; vt[9] = (void*)&DeviceState;
    vt[18] = (void*)&Create; vt[21] = (void*)&ForceState; vt[22] = (void*)&Command; vt[25] = (void*)&PollDevice;
    Driver driver = {}; driver.vtable = vt;
    DeviceWrap* d = Open(driver);
    assert(Service(1) == d && driver.acquires == 1 && driver.starts == 5 && driver.creates == 5);
    assert(Service(1.1) == d && Service(1.6) == d && driver.starts == 5);

    // Alt-Tab, including an explicit game Unacquire. No retries/forces in the background.
    assert(!Service(1.7, false));
    d->Unacquire();
    int acquired = driver.acquires;
    assert(!Service(1.9, false) && driver.acquires == acquired);
    driver.acquireResult = S_FALSE;
    LONG gen = d->acquireGen;
    assert(Service(2) == d && d->acquireGen == gen + 1 && driver.starts == 10 && driver.creates == 5);
    driver.acquireResult = DI_OK;

    // F4-like silent effect stop while input access still appears healthy.
    driver.StopAll();
    assert(Service(2.6) == d && driver.starts == 15 && driver.acquires == acquired + 1);
    auto* spring = (TestEffect*)s_fx.spring;
    SetCondition(spring, s_fx.springCoef, 0.5f);
    assert(s_fx.springCoef == 5000);
    spring->parameterResult = DIERR_INPUTLOST;
    SetCondition(spring, s_fx.springCoef, 0.6f);
    assert(s_fx.springCoef == 5000 && !d->acquired && !s_fx.ready);
    spring->parameterResult = DI_OK;
    assert(Service(3.2) == d && driver.starts == 20);

    // Failed Start must remain pending and be retried without a 200 Hz busy loop.
    driver.StopAll();
    auto* constant = (TestEffect*)s_fx.constant;
    constant->startResult = DIERR_INPUTLOST;
    assert(!Service(3.8) && !s_fx.ready && !d->acquired);
    int starts = driver.starts;
    for (int i = 0; i < 10; ++i) assert(!Service(3.9 + i * 0.01));
    assert(driver.starts == starts);
    constant->startResult = DI_OK;
    assert(Service(5.9) == d && s_fx.ready);

    // An intentional foreground Unacquire is respected until a new Acquire.
    d->Unacquire(); acquired = driver.acquires;
    assert(!Service(6) && driver.acquires == acquired);
    driver.acquireResult = DIERR_OTHERAPPHASPRIO;
    d->Acquire();
    assert(!Service(6.1));
    acquired = driver.acquires;
    for (int i = 0; i < 10; ++i) assert(!Service(6.12 + i * 0.01));
    assert(driver.acquires == acquired);
    driver.acquireResult = DI_OK;
    assert(Service(6.7) == d);

    // Explicit reset reconstructs effects, while preserving calibration.
    S.forceSign = 1; S.slip.sign = -1; S.right.sign = 1;
    s_started = 1;
    assert(Ffb_RequestReset() && s_resetRequested == 1);
    int creates = driver.creates;
    assert(Service(7, true, InterlockedExchange(&s_resetRequested, 0) != 0) == d);
    assert(driver.creates == creates + 5 && driver.releases == 5);
    assert(S.forceSign == 1 && S.slip.sign == -1 && S.right.sign == 1);
    Out forces; forces.constant = 0.5f; forces.spring = 0.3f;
    Apply(forces, 7);
    assert(((TestEffect*)s_fx.constant)->magnitude == 0);
    Apply(forces, 7.125);
    assert(((TestEffect*)s_fx.constant)->magnitude > 0 && ((TestEffect*)s_fx.constant)->magnitude < 5000);
    Apply(forces, 7.3);
    assert(((TestEffect*)s_fx.constant)->magnitude == 5000);
    // Disabled physical switches must not be overridden by software recovery.
    driver.state |= DIGFFS_SAFETYSWITCHOFF;
    int enabled = driver.enables; starts = driver.starts;
    assert(!Service(7.6) && !Service(7.8));
    assert(driver.enables == enabled && driver.starts == starts);
    driver.state &= ~DIGFFS_SAFETYSWITCHOFF;
    assert(Service(8.2) == d);

    // Unsupported health queries should not restart healthy effects.
    driver.stateResult = DIERR_UNSUPPORTED;
    for (auto* e : driver.effects) e->statusResult = DIERR_UNSUPPORTED;
    starts = driver.starts;
    assert(Service(8.8) == d && driver.starts == starts);
    driver.stateResult = DI_OK;
    for (auto* e : driver.effects) e->statusResult = DI_OK;

    // Lost downloads are restored even when the device handle remains acquired.
    for (auto* e : driver.effects) e->downloaded = e->playing = false;
    assert(Service(9.4) == d && driver.starts == starts + 5);
    auto* sine = (TestEffect*)s_fx.sine;
    sine->parameterResult = DI_DOWNLOADSKIPPED;
    DWORD oldMag = s_fx.sineMag;
    SetSine(0.3f, 30);
    assert(s_fx.sineMag == oldMag && !d->acquired && !s_fx.ready);
    sine->parameterResult = DI_OK;
    assert(Service(10) == d);

    // Poll loss uses the same recovery, and malformed axis offsets are rejected.
    float axis;
    d->axisOfs = 4096;
    assert(!ReadAxis(d, testTime, &axis));
    d->axisOfs = 0;
    driver.pollResult = DIERR_INPUTLOST;
    assert(!ReadAxis(d, testTime, &axis) && !d->acquired);
    driver.pollResult = DI_OK;
    assert(Service(10.6) == d);

    // F4 may temporarily unacquire the controller without losing OS focus.
    Ffb_SetMenuOpen(true);
    d->Unacquire();
    assert(Service(11.2) == d && d->acquireWanted);
    Ffb_SetMenuOpen(false);
    d->Unacquire();
    assert(Service(11.8) == d && s_menuOpen == 0);

    // Native/disabled configurations refuse our reset API.
    g_cfg.nativeFfb = true; assert(!Ffb_RequestReset()); g_cfg.nativeFfb = false;
    g_cfg.enabled = false; assert(!Ffb_RequestReset()); g_cfg.enabled = true;
    d->Release(); assert(driver.effects.empty() && s_numDevs == 0);

    // Wheels supporting only constant force still work without retry storms.
    Driver limited = {}; limited.vtable = vt; limited.constantOnly = true;
    d = Open(limited);
    assert(Service(12.4) == d && limited.starts == 1);
    assert(Service(13.4) == d && limited.starts == 1);
    d->Release(); assert(limited.effects.empty());
    TestForceModel();
    TestCalibration();
    puts("FFB recovery and force model tests passed");
}
