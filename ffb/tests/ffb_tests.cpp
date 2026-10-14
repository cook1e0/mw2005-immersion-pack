// The force feedback core without a game or a wheel: the filters, the force model's signs and
// shapes, the engine's safety chain, and the device layer against a fake DirectInput driver
// that behaves like the measured Fanatec base (changes only take effect with DIEP_START).
#include "../src/dinput_wrap.cpp"
#include "../src/ffb_device.cpp"
#include "../src/ffb_model.cpp"
#include "../src/ffb_engine.cpp"
#include <assert.h>
#include <stdio.h>
#include <algorithm>
#include <vector>

Config g_cfg;
static double testTime = 1;
double Now() { return testTime; }
void Log(const char*, ...) {}
static Telemetry s_tel;
void Telemetry_Get(Telemetry* out) { *out = s_tel; }

using namespace ffb;

// --- fake driver --------------------------------------------------------------------------------
struct Driver;
struct FakeEffect : IDirectInputEffect {
    Driver* driver;
    bool playing = false;
    LONG magnitude = 0, applied = 0, direction = 1;
    DWORD lastFlags = 0;
    FakeEffect(Driver* d) : driver(d) {}
    STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(GetEffectGuid)(GUID*) override { return DI_OK; }
    STDMETHOD(GetParameters)(DIEFFECT*, DWORD) override { return DI_OK; }
    STDMETHOD(SetParameters)(const DIEFFECT* e, DWORD flags) override;
    STDMETHOD(Start)(DWORD, DWORD) override;
    STDMETHOD(Stop)() override { playing = false; return DI_OK; }
    STDMETHOD(GetEffectStatus)(DWORD* status) override { *status = playing ? DIEGES_PLAYING : 0; return DI_OK; }
    STDMETHOD(Download)() override { return DI_OK; }
    STDMETHOD(Unload)() override { playing = false; return DI_OK; }
    STDMETHOD(Escape)(DIEFFESCAPE*) override { return DI_OK; }
};
struct Driver {
    void** vtable;
    bool acquired = false;
    DWORD state = DIGFFS_ACTUATORSON | DIGFFS_POWERON | DIGFFS_SAFETYSWITCHON;
    int acquires = 0, creates = 0, starts = 0;
    std::vector<FakeEffect*> effects;
    std::vector<GUID> kinds;
};
ULONG FakeEffect::Release() {
    auto& l = driver->effects;
    l.erase(std::find(l.begin(), l.end(), this));
    delete this;
    return 0;
}
// Like the measured Fanatec: a change only reaches the motor when it restarts the effect.
HRESULT FakeEffect::SetParameters(const DIEFFECT* e, DWORD flags) {
    if (!driver->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    magnitude = ((const DICONSTANTFORCE*)e->lpvTypeSpecificParams)->lMagnitude;
    if (flags & DIEP_DIRECTION) direction = e->rglDirection[0];
    lastFlags = flags;
    if (flags & DIEP_START) { playing = true; applied = magnitude * (direction < 0 ? -1 : 1); }
    return DI_OK;
}
HRESULT FakeEffect::Start(DWORD, DWORD) {
    if (!driver->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    ++driver->starts;
    playing = true;
    applied = magnitude * (direction < 0 ? -1 : 1);
    return DI_OK;
}
static Driver* D(IDirectInputDevice8A* self) { return (Driver*)self; }
static ULONG WINAPI ReleaseDevice(IDirectInputDevice8A*) { return 0; }
static HRESULT WINAPI AcquireDevice(IDirectInputDevice8A* self) { D(self)->acquires++; D(self)->acquired = true; return DI_OK; }
static HRESULT WINAPI UnacquireDevice(IDirectInputDevice8A* self) {
    D(self)->acquired = false;
    for (auto* e : D(self)->effects) e->playing = false;
    return DI_OK;
}
static HRESULT WINAPI FakeSetProp(IDirectInputDevice8A*, REFGUID, const DIPROPHEADER*) { return DI_OK; }
static HRESULT WINAPI FakeGetProp(IDirectInputDevice8A*, REFGUID, DIPROPHEADER* p) {
    auto* r = (DIPROPRANGE*)p; r->lMin = -10000; r->lMax = 10000; return DI_OK;
}
static LONG s_wheelRaw;     // the fake wheel's position, -10000..10000
static HRESULT WINAPI DeviceState(IDirectInputDevice8A* self, DWORD size, void* out) {
    if (!D(self)->acquired) return DIERR_NOTACQUIRED;
    memset(out, 0, size); *(LONG*)out = s_wheelRaw; return DI_OK;
}
static HRESULT WINAPI Poll(IDirectInputDevice8A*) { return DI_OK; }
static HRESULT WINAPI ForceState(IDirectInputDevice8A* self, DWORD* out) { *out = D(self)->state; return DI_OK; }
static HRESULT WINAPI Command(IDirectInputDevice8A*, DWORD) { return DI_OK; }
static HRESULT WINAPI Caps(IDirectInputDevice8A*, LPDIDEVCAPS c) { c->dwAxes = 1; c->dwButtons = 0; return DI_OK; }
static HRESULT WINAPI Create(IDirectInputDevice8A* self, REFGUID kind, const DIEFFECT*, IDirectInputEffect** out, IUnknown*) {
    Driver* d = D(self);
    if (!d->acquired) return DIERR_NOTEXCLUSIVEACQUIRED;
    d->creates++;
    d->kinds.push_back(kind);
    auto* fx = new FakeEffect(d);
    d->effects.push_back(fx);
    *out = fx;
    return DI_OK;
}

static float NearZero(float v) { return fabsf(v) < 1e-4f; }

// --- filters -------------------------------------------------------------------------------------
static void TestFilters() {
    // Low-pass: follows a step without overshoot and gets there.
    LowPass2 lp;
    lp.Reset(0);
    float peak = 0, y = 0;
    for (int i = 0; i < 400; i++) { y = lp.Step(1.0f, 0.0025f, 25.0f); peak = std::max(peak, y); }
    assert(peak <= 1.0001f && y > 0.99f);
    lp.Reset(0);
    assert(lp.Step(1.0f, 0.0025f, 25.0f) < 0.1f);              // a step is not passed straight through

    // Soft limiter: linear below the knee, monotonic and below 1 above it.
    assert(SoftLimit(0.5f, 0.7f) == 0.5f && SoftLimit(-0.5f, 0.7f) == -0.5f);
    float prev = 0.7f;
    for (float x = 0.71f; x < 5.0f; x += 0.05f) { float o = SoftLimit(x, 0.7f); assert(o >= prev && o <= 1.0f); prev = o; }
    assert(SoftLimit(1.0f, 0.7f) < 0.95f && SoftLimit(0.9f, 0.7f) > 0.8f);   // compressed, not cut flat
    assert(SoftLimit(-3.0f, 0.7f) < -0.95f);

    // Slew limiter: never more than rate * dt per step.
    SlewLimiter sl;
    float last = 0;
    for (int i = 0; i < 100; i++) { float o = sl.Step(1.0f, 0.0025f, 20.0f); assert(o - last <= 20.0f * 0.0025f + 1e-6f); last = o; }
    assert(fabsf(last - 1.0f) < 1e-4f);

    // Oscillation guard: a wheel shaking at 8 Hz trips it; turning into a corner and back does not.
    OscillationGuard g;
    float level = 0;
    for (int i = 0; i < 400; i++) level = g.Step(3.0f * sinf(2 * 3.14159265f * 8.0f * i * 0.0025f), 0.0025f);
    assert(level > 0.9f);
    OscillationGuard calm;
    for (int i = 0; i < 400; i++) level = calm.Step(2.0f * sinf(2 * 3.14159265f * 0.7f * i * 0.0025f), 0.0025f);
    assert(level < 0.01f);
}

// --- model ---------------------------------------------------------------------------------------
static Telemetry Car(float speed, float vLatFront = 0) {
    Telemetry t = {};
    t.driving = t.valid = true;
    t.speed = speed;
    t.vLong = speed;
    t.vLatFront = vLatFront;
    t.frontLoad = 8000;
    t.frontOnGround = 2;
    t.wheelsOnGround = 4;
    t.wheelbase = 2.6f;
    return t;
}
static WheelState Wheel(float pos, float vel = 0) { WheelState w; w.known = true; w.pos = pos; w.vel = vel; return w; }

static void TestModel() {
    ModelParams p;
    Model m;
    // Driving straight at 30 m/s with the wheel turned right: the tyres pull it back left.
    ForceParts f = m.Compute(Car(30), Wheel(0.1f), p, 1.0f, 0.0025f);
    assert(f.aligning < -0.1f && f.centering < 0);
    // Turned left: pulled back right.
    f = m.Compute(Car(30), Wheel(-0.1f), p, 1.0f, 0.0025f);
    assert(f.aligning > 0.1f && f.centering > 0);
    // Wheel straight, front axle sliding left (a slide, the car's nose pointing right of its path):
    // the wheel is pulled left into the countersteer.
    f = m.Compute(Car(25, -6), Wheel(0), p, 1.0f, 0.0025f);
    assert(f.aligning < -0.1f);
    // Past the grip limit the steering goes light: far less than at the peak.
    float peakDeg = p.gripPeakDeg;
    float atPeak = fabsf(m.Compute(Car(30), Wheel(peakDeg / 45.0f), p, 1.0f, 0.0025f).aligning);
    float beyond = fabsf(m.Compute(Car(30), Wheel(4 * peakDeg / 45.0f), p, 1.0f, 0.0025f).aligning);
    assert(beyond < atPeak * 0.6f);
    // Parked: no aligning torque, more friction against movement than at speed.
    f = m.Compute(Car(0), Wheel(0.1f, 1.0f), p, 1.0f, 0.0025f);
    assert(NearZero(f.aligning) && f.friction < -0.15f);
    float movingFriction = m.Compute(Car(30), Wheel(0.1f, 1.0f), p, 1.0f, 0.0025f).friction;
    assert(f.friction < movingFriction);
    // Front wheels off the ground: no aligning torque.
    Telemetry air = Car(30);
    air.frontOnGround = 0;
    assert(NearZero(m.Compute(air, Wheel(0.2f), p, 1.0f, 0.0025f).aligning));
    // Damping resists movement both ways.
    assert(m.Compute(Car(30), Wheel(0, 2.0f), p, 1.0f, 0.0025f).damping < 0);
    assert(m.Compute(Car(30), Wheel(0, -2.0f), p, 1.0f, 0.0025f).damping > 0);
    // Soft lock past the steering lock pushes back.
    p.lockShare = 0.5f;
    assert(m.Compute(Car(30), Wheel(0.55f), p, 1.0f, 0.0025f).softLock < -0.5f);
    assert(m.Compute(Car(30), Wheel(-0.55f), p, 1.0f, 0.0025f).softLock > 0.5f);
    p.lockShare = 1.0f;
    // Menus: only the menu spring.
    Telemetry menu = {};
    f = m.Compute(menu, Wheel(0.8f), p, 0.0f, 0.0025f);      // past 2/3 of the lock: full spring
    assert(NearZero(f.aligning) && fabsf(f.centering + p.menuCentering) < 1e-4f);
}

// --- engine safety chain ----------------------------------------------------------------------------
static void TestEngine() {
    g_cfg = Config();
    Engine e;
    double now = 10;
    float lastOut = 0;
    // A huge force: the output never exceeds MaxForce and never changes faster than the slew rate.
    g_cfg.gain = 3.0f;
    g_cfg.aligning = 3.0f;
    Telemetry t = Car(40);
    t.time = now;
    for (int i = 0; i < 800; i++) {
        now += 0.0025;
        t.time = now;
        Step s = e.Update(t, true, 0.6f, now, 0.0025f, 0.0);
        assert(fabsf(s.out) <= g_cfg.maxForce + 1e-5f);
        assert(fabsf(s.out - lastOut) <= g_cfg.slewRate * 0.0025f + 1e-5f);
        lastOut = s.out;
    }
    assert(lastOut < -0.5f * g_cfg.maxForce);           // pushing the wheel back left, hard
    // Invalid numbers from the game never reach the wheel.
    t.vLatFront = NAN;
    for (int i = 0; i < 20; i++) {
        now += 0.0025;
        t.time = now;
        Step s = e.Update(t, true, 0.6f, now, 0.0025f, 0.0);
        assert(isfinite(s.out));
    }
    // The force fades in after the effect (re)starts.
    g_cfg = Config();
    Engine fresh;
    t = Car(30);
    t.time = now;
    Step s = fresh.Update(t, true, 0.3f, now, 0.0025f, now);
    assert(NearZero(s.out));
    // A wheel shaking by itself trips the guard, which raises damping and lowers the gain.
    Engine shaky;
    float guard = 0;
    for (int i = 0; i < 400; i++) {
        now += 0.0025;
        t.time = now;
        float pos = 0.05f * sinf(2 * 3.14159265f * 9.0f * (float)i * 0.0025f);
        guard = shaky.Update(t, true, pos, now, 0.0025f, 0.0).guard;
    }
    assert(guard > 0.9f);
}

// --- device ----------------------------------------------------------------------------------
static DeviceWrap* Open(Driver& driver) {
    auto* d = new DeviceWrap((IDirectInputDevice8A*)&driver);
    strcpy(d->name, "Test wheel");
    d->exclusive = d->acquireWanted = true;
    d->axisOfs = 0;
    d->dataSize = sizeof(LONG);
    Ffb_RegisterDevice(d);
    return d;
}

static void TestDevice() {
    void* vt[32] = {};
    vt[2] = (void*)&ReleaseDevice; vt[3] = (void*)&Caps; vt[5] = (void*)&FakeGetProp; vt[6] = (void*)&FakeSetProp;
    vt[7] = (void*)&AcquireDevice; vt[8] = (void*)&UnacquireDevice; vt[9] = (void*)&DeviceState;
    vt[18] = (void*)&Create; vt[21] = (void*)&ForceState; vt[22] = (void*)&Command; vt[25] = (void*)&Poll;
    Driver driver = {};
    driver.vtable = vt;
    g_cfg = Config();
    g_cfg.device = -1;
    DeviceWrap* d = Open(driver);
    d->Acquire();

    // One constant force, nothing else.
    assert(Device_Service(1.0, true, false) == d);
    assert(driver.creates == 1 && driver.kinds[0] == GUID_ConstantForce);
    FakeEffect* fx = driver.effects[0];
    // Every change restarts the effect, or this driver drops it.
    Device_Send(0.5f, 1.01);
    assert((fx->lastFlags & DIEP_START) && fx->applied == 5000);
    // The wheel's direction: ForceSign -1 turns + into a negative magnitude.
    g_cfg.forceSign = -1;
    Device_Send(0.4f, 1.2);
    assert(fx->applied == -4000);
    // Reverse flips it back.
    g_cfg.reverse = 1.0f;
    Device_Send(0.4f, 1.4);
    assert(fx->applied == 4000);
    g_cfg.reverse = 0.0f;
    // ForceDirection=1: the side as the direction, the magnitude positive.
    g_cfg.forceDirection = 1;
    Device_Send(0.4f, 1.6);
    assert(fx->magnitude == 4000 && fx->direction == -1 && fx->applied == -4000);
    g_cfg.forceDirection = 0;
    Device_Send(0.4f, 1.8);
    assert(fx->direction == 1 && fx->applied == -4000);
    g_cfg.forceSign = 1;

    // The wheel's position.
    s_wheelRaw = 5000;
    float pos = 0;
    assert(Device_ReadWheel(d, 2.0, &pos) && fabsf(pos - 0.5f) < 1e-3f);

    // A stopped effect is restarted by the health check.
    fx->playing = false;
    Device_Service(2.6, true, false);
    assert(Device_Service(3.2, true, false) == d && fx->playing);
    // Focus loss stops the force; focus return re-acquires and restarts it.
    assert(!Device_Service(3.4, false, false) && !fx->playing);
    d->Unacquire();
    Device_Service(3.6, true, false);
    assert(Device_Service(4.4, true, false) == d && driver.effects.size() == 1 && driver.effects[0]->playing);
    // A hardware safety switch is respected: no restart while it is off.
    driver.state |= DIGFFS_SAFETYSWITCHOFF;
    driver.effects[0]->playing = false;
    int starts = driver.starts;
    assert(!Device_Service(5.0, true, false) && !Device_Service(5.6, true, false) && driver.starts == starts);
    driver.state &= ~DIGFFS_SAFETYSWITCHOFF;
    Device_Service(6.2, true, false);
    assert(Device_Service(6.8, true, false) == d);
    // A manual restart rebuilds the effect.
    int creates = driver.creates;
    Device_Service(7.0, true, true);
    Device_Service(7.6, true, false);
    assert(Device_Service(8.2, true, false) == d && driver.creates == creates + 1);
    d->Release();
    assert(driver.effects.empty());
}

int main() {
    InitializeCriticalSection(&g_diLock);
    TestFilters();
    TestModel();
    TestEngine();
    TestDevice();
    puts("FFB filter, model, engine and device tests passed");
}
