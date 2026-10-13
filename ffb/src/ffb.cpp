// Force feedback engine.
//
// Runs on its own thread at Config::updateHz. Reads the latest telemetry snapshot, builds the
// steering torque and drives five DirectInput effects on the wheel:
//   constant  - self-aligning torque + road kicks + collision jolts
//   spring    - centring in menus / pause / before calibration
//   damper    - speed-dependent weight
//   friction  - static friction + tyre scrub when parked
//   sine      - engine buzz, brake lock, rough road
//
// Sign conventions are not knowable from the binary alone, so three are calibrated at runtime
// against the physical wheel position and stored in nfsmw_ffb.ini [Calibration]:
//   ForceSign - constant-force magnitude sign that turns the wheel right (short pulse in menus)
//   SlipSign  - game tyre slip-angle sign vs. steering right (while cornering)
//   RightSign - game "right guess" vector vs. physical right (yaw while cornering)
// Until a sign is known, the forces that depend on it are replaced by sign-agnostic ones.
#include "ffb.h"
#include "telemetry.h"
#include <math.h>
#include <string.h>
#include <mmsystem.h>
#include <stdlib.h>

static const float kPi = 3.14159265f;

// ---------------------------------------------------------------------------
// Devices (guarded by g_diLock)
// ---------------------------------------------------------------------------
static DeviceWrap* s_devs[8];
static int s_numDevs;
static IDirectInput8A* s_di;
static double s_diTime;
static bool s_triedFallback;

struct Effects {
    DeviceWrap* dev = nullptr;
    LONG gen = -1;
    IDirectInputEffect* constant = nullptr;
    IDirectInputEffect* spring = nullptr;
    IDirectInputEffect* damper = nullptr;
    IDirectInputEffect* friction = nullptr;
    IDirectInputEffect* sine = nullptr;
    // last values sent
    LONG constMag = 0;
    double constTime = 0;
    LONG springCoef = -1, damperCoef = -1, frictionCoef = -1;
    DWORD sineMag = 0xFFFFFFFF, sinePeriod = 0;
    // axis
    LONG rangeMin = 0, rangeMax = 0;
    double rangeTime = 0;
    double createTime = 0;
};
static Effects s_fx;

static void ReleaseEffects() {
    IDirectInputEffect** all[] = {&s_fx.constant, &s_fx.spring, &s_fx.damper, &s_fx.friction, &s_fx.sine};
    for (auto p : all) {
        if (*p) {
            (*p)->Stop();
            (*p)->Release();
            *p = nullptr;
        }
    }
    s_fx = Effects();
}

void Ffb_SetDirectInput(IDirectInput8A* real) {
    EnterCriticalSection(&g_diLock);
    if (s_di) s_di->Release();
    s_di = real;
    s_diTime = Now();
    LeaveCriticalSection(&g_diLock);
}

void Ffb_RegisterDevice(DeviceWrap* dev) {
    if (s_numDevs < 8) s_devs[s_numDevs++] = dev;
    Log("ffb: registered FFB device '%s'", dev->name);
}

void Ffb_UnregisterDevice(DeviceWrap* dev) {
    for (int i = 0; i < s_numDevs; ++i) {
        if (s_devs[i] == dev) {
            s_devs[i] = s_devs[--s_numDevs];
            break;
        }
    }
    if (s_fx.dev == dev) {
        ReleaseEffects();
        Log("ffb: effects released with device '%s'", dev->name);
    }
}

// ---------------------------------------------------------------------------
// Effect helpers (g_diLock held)
// ---------------------------------------------------------------------------
static DWORD AxisOfs(DeviceWrap* d) { return d->axisOfs >= 0 ? (DWORD)d->axisOfs : DIJOFS_X; }

static IDirectInputEffect* MakeEffect(DeviceWrap* d, REFGUID type, const char* label, void* params, DWORD size) {
    DWORD axes[1] = {AxisOfs(d)};
    LONG dir[1] = {1};
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.cAxes = 1;
    e.rgdwAxes = axes;
    e.rglDirection = dir;
    e.cbTypeSpecificParams = size;
    e.lpvTypeSpecificParams = params;
    IDirectInputEffect* fx = nullptr;
    HRESULT hr = d->real->CreateEffect(type, &e, &fx, nullptr);
    if (FAILED(hr)) {
        Log("ffb: CreateEffect(%s) failed 0x%08lX", label, hr);
        return nullptr;
    }
    return fx;
}

static void StartAll() {
    IDirectInputEffect* all[] = {s_fx.constant, s_fx.spring, s_fx.damper, s_fx.friction, s_fx.sine};
    for (auto fx : all)
        if (fx) fx->Start(1, 0);
    // force everything to be re-sent
    s_fx.constTime = 0;
    s_fx.springCoef = s_fx.damperCoef = s_fx.frictionCoef = -1;
    s_fx.sineMag = 0xFFFFFFFF;
}

static bool CreateEffects(DeviceWrap* d) {
    double created = s_fx.createTime;
    ReleaseEffects();
    s_fx.createTime = created;
    s_fx.dev = d;
    d->real->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
    DIPROPDWORD gain = {};
    gain.diph.dwSize = sizeof(gain);
    gain.diph.dwHeaderSize = sizeof(gain.diph);
    gain.diph.dwHow = DIPH_DEVICE;
    gain.dwData = DI_FFNOMINALMAX;
    d->real->SetProperty(DIPROP_FFGAIN, &gain.diph);

    DICONSTANTFORCE cf = {0};
    DICONDITION cond = {};
    cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
    DIPERIODIC per = {};
    per.dwPeriod = 25000;
    s_fx.constant = MakeEffect(d, GUID_ConstantForce, "constant", &cf, sizeof(cf));
    s_fx.spring = MakeEffect(d, GUID_Spring, "spring", &cond, sizeof(cond));
    s_fx.damper = MakeEffect(d, GUID_Damper, "damper", &cond, sizeof(cond));
    s_fx.friction = MakeEffect(d, GUID_Friction, "friction", &cond, sizeof(cond));
    s_fx.sine = MakeEffect(d, GUID_Sine, "sine", &per, sizeof(per));
    s_fx.gen = d->acquireGen;
    StartAll();
    Log("ffb: effects on '%s': constant=%d spring=%d damper=%d friction=%d sine=%d", d->name,
        !!s_fx.constant, !!s_fx.spring, !!s_fx.damper, !!s_fx.friction, !!s_fx.sine);
    return s_fx.constant || s_fx.spring || s_fx.damper;
}

static bool Lost(HRESULT hr) {
    return hr == DIERR_NOTEXCLUSIVEACQUIRED || hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED;
}

static void SetParams(IDirectInputEffect* fx, void* params, DWORD size) {
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.cbTypeSpecificParams = size;
    e.lpvTypeSpecificParams = params;
    HRESULT hr = fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
    if (Lost(hr)) s_fx.gen = -1;  // restart once the game re-acquires
}

static void SetCondition(IDirectInputEffect* fx, LONG& last, float coef) {
    if (!fx) return;
    LONG c = (LONG)(Clamp(coef, 0.0f, 1.0f) * DI_FFNOMINALMAX);
    if (abs(c - last) < 50) return;
    last = c;
    DICONDITION cond = {};
    cond.lPositiveCoefficient = cond.lNegativeCoefficient = c;
    cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
    SetParams(fx, &cond, sizeof(cond));
}

static void SetConstant(float v, double now) {
    if (!s_fx.constant) return;
    LONG m = (LONG)lrintf(Clamp(v, -1.0f, 1.0f) * DI_FFNOMINALMAX);
    if (abs(m - s_fx.constMag) < 15 && now - s_fx.constTime < 0.05) return;
    s_fx.constMag = m;
    s_fx.constTime = now;
    DICONSTANTFORCE cf = {m};
    SetParams(s_fx.constant, &cf, sizeof(cf));
}

static void SetSine(float mag, float hz) {
    if (!s_fx.sine) return;
    DWORD m = (DWORD)(Clamp(mag, 0.0f, 1.0f) * DI_FFNOMINALMAX);
    DWORD period = (DWORD)(1e6f / Clamp(hz, 5.0f, 120.0f));
    bool periodChanged = !s_fx.sinePeriod || fabsf((float)period / (float)s_fx.sinePeriod - 1.0f) > 0.08f;
    if (abs((LONG)m - (LONG)s_fx.sineMag) < 30 && !periodChanged) return;
    if (!periodChanged) period = s_fx.sinePeriod;
    s_fx.sineMag = m;
    s_fx.sinePeriod = period;
    DIPERIODIC p = {};
    p.dwMagnitude = m;
    p.dwPeriod = period;
    SetParams(s_fx.sine, &p, sizeof(p));
}

// Normalised steering axis position (-1 left .. +1 right). Returns false if unavailable.
static bool ReadAxis(DeviceWrap* d, double now, float* out) {
    if (!d->acquired || d->axisOfs < 0 || d->dataSize == 0 || d->dataSize > 1024) return false;
    if (now - s_fx.rangeTime > 1.0) {
        DIPROPRANGE r = {};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(r.diph);
        r.diph.dwHow = DIPH_BYOFFSET;
        r.diph.dwObj = (DWORD)d->axisOfs;
        if (SUCCEEDED(d->real->GetProperty(DIPROP_RANGE, &r.diph))) {
            s_fx.rangeMin = r.lMin;
            s_fx.rangeMax = r.lMax;
        }
        s_fx.rangeTime = now;
    }
    if (s_fx.rangeMax <= s_fx.rangeMin) return false;
    alignas(8) uint8_t buf[1024];
    if (FAILED(d->real->GetDeviceState(d->dataSize, buf))) return false;
    LONG raw = *(LONG*)(buf + d->axisOfs);
    float mid = 0.5f * ((float)s_fx.rangeMin + (float)s_fx.rangeMax);
    float half = 0.5f * ((float)s_fx.rangeMax - (float)s_fx.rangeMin);
    *out = Clamp(((float)raw - mid) / half, -1.0f, 1.0f);
    return true;
}

static DeviceWrap* PickDevice() {
    if (s_fx.dev && s_fx.dev->acquired && s_fx.dev->exclusive) return s_fx.dev;
    for (int i = 0; i < s_numDevs; ++i)
        if (s_devs[i]->acquired && s_devs[i]->exclusive) return s_devs[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Fallback: the game never opened a force feedback device -> open the first one ourselves.
// ---------------------------------------------------------------------------
struct WindowSearch { DWORD pid; HWND found; };
static BOOL CALLBACK FindWindowCb(HWND w, LPARAM p) {
    WindowSearch* c = (WindowSearch*)p;
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid == c->pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        c->found = w;
        return FALSE;
    }
    return TRUE;
}

static HWND FindGameWindow() {
    WindowSearch ctx = {GetCurrentProcessId(), nullptr};
    EnumWindows(FindWindowCb, (LPARAM)&ctx);
    return ctx.found;
}

struct FoundDevice { GUID guid; char name[MAX_PATH]; bool ok; };
static BOOL CALLBACK FindFfbDeviceCb(LPCDIDEVICEINSTANCEA inst, LPVOID p) {
    FoundDevice* f = (FoundDevice*)p;
    f->guid = inst->guidInstance;
    strncpy(f->name, inst->tszProductName, MAX_PATH - 1);
    f->ok = true;
    return DIENUM_STOP;
}

static DeviceWrap* s_ownDev;

static void TryFallbackDevice() {
    s_triedFallback = true;
    HWND hwnd = FindGameWindow();
    if (!s_di || !hwnd) {
        Log("ffb: fallback skipped (di=%p hwnd=%p)", s_di, hwnd);
        return;
    }
    FoundDevice found = {};
    s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, FindFfbDeviceCb, &found, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    if (!found.ok) {
        Log("ffb: no force feedback device attached");
        return;
    }
    IDirectInputDevice8A* real = nullptr;
    if (FAILED(s_di->CreateDevice(found.guid, &real, nullptr)) || !real) {
        Log("ffb: fallback CreateDevice failed for '%s'", found.name);
        return;
    }
    DeviceWrap* w = new DeviceWrap(real);
    strncpy(w->name, found.name, sizeof(w->name) - 1);
    static DIOBJECTDATAFORMAT odf = {&SteerAxisGuid(), 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0};
    static DIDATAFORMAT df = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, 4, 1, &odf};
    w->SetDataFormat(&df);
    w->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    Ffb_RegisterDevice(w);
    w->Acquire();
    s_ownDev = w;
    Log("ffb: game did not open a force feedback device; opened '%s' ourselves", found.name);
}

// ---------------------------------------------------------------------------
// Force model
// ---------------------------------------------------------------------------
struct Out {
    float constant = 0, spring = 0, damper = 0, friction = 0, sineMag = 0, sineHz = 30;
};

struct Calib {
    int sign = 0;     // decided sign (0 = unknown)
    int score = 0, n = 0;
    void Add(float a, float b) {
        if (a == 0.0f || b == 0.0f) return;
        score += (a > 0) == (b > 0) ? 1 : -1;
        n++;
    }
    // Decide after enough samples with a clear majority. Returns true when a decision changes.
    bool Decide(int minSamples) {
        if (n < minSamples) return false;
        int want = (abs(score) * 10 >= n * 6) ? (score > 0 ? 1 : -1) : 0;
        score = n = 0;
        if (want && want != sign) {
            sign = want;
            return true;
        }
        return false;
    }
};

static struct State {
    float fade = 0;               // 0 = menus .. 1 = driving
    uint32_t lastSeq = 0;
    uint32_t lastHitSeq = 0;
    bool  haveHitSeq = false;
    float loadRef = 0;
    float sat = 0, road = 0;      // smoothed components
    float roadTarget = 0, roadVib = 0;
    double joltTime = -1;
    float joltAmp = 0;
    int   joltFlip = 1;
    Calib slip, right;
    int   forceSign = 0;
    // force direction pulse test
    int   pulsePhase = 0, pulseTries = 0;
    double pulseTime = 0, pulseNext = 0;
    float pulseAxis[3];
    float axis = 0, axisPrev = 0, axisVel = 0;
    bool  haveAxis = false;
    double logTime = 0, statTime = 0;
    int   clipCount = 0, tickCount = 0;
} S;

static float SoftClip(float x) {
    float a = fabsf(x);
    if (a <= 0.75f) return x;
    return Sign(x) * (0.75f + 0.25f * tanhf((a - 0.75f) / 0.25f));
}

static void Compute(const Telemetry& t, double now, float dt, Out& o) {
    const Config& c = g_cfg;
    bool driving = t.driving && t.valid && (now - t.time) < 0.2;
    S.fade = Clamp(S.fade + (driving ? dt / 0.3f : -dt / 0.15f), 0.0f, 1.0f);
    bool newSample = t.seq != S.lastSeq;
    S.lastSeq = t.seq;

    float slipSign = (float)S.slip.sign;
    float rightSign = (float)(S.right.sign ? S.right.sign : 1);
    float speed = t.speed;

    // --- self-aligning torque -------------------------------------------------------------
    float onF = (t.onGround[0] ? 1.0f : 0.0f) + (t.onGround[1] ? 1.0f : 0.0f);
    float alphaRaw = onF > 0 ? ((t.onGround[0] ? t.slipAngle[0] : 0) + (t.onGround[1] ? t.slipAngle[1] : 0)) / onF : 0.0f;
    float frontLoad = (t.onGround[0] ? t.load[0] : 0) + (t.onGround[1] ? t.load[1] : 0);
    if (driving && onF == 2 && speed > 3.0f && frontLoad > 0) {
        if (S.loadRef <= 0) S.loadRef = frontLoad;
        S.loadRef += (frontLoad - S.loadRef) * Clamp(dt / 5.0f, 0.0f, 1.0f);
    }
    float loadF = S.loadRef > 0 ? Clamp(frontLoad / S.loadRef, 0.0f, 1.6f) : onF * 0.5f;
    float vs = Smoothstep(0.5f, 6.0f, speed);
    if (t.fwdSpeed < -0.5f) vs *= 0.4f;

    float satTarget = 0.0f, precalSpring = 0.0f;
    if (slipSign != 0.0f) {
        float peak = Clamp(c.satPeakAngle, 1.0f, 30.0f) * kPi / 180.0f;
        float x = slipSign * alphaRaw / peak;
        float shape = x * expf(0.5f * (1.0f - x * x)) + c.driftAlign * tanhf(0.5f * x);
        satTarget = -c.satGain * shape * loadF * vs;  // tyre pulls the wheel back toward the direction of travel
    } else {
        precalSpring = c.satGain * 0.6f * vs * (onF * 0.5f);
    }
    S.sat += (satTarget - S.sat) * Clamp(dt / 0.015f, 0.0f, 1.0f);

    // --- road: front suspension movement ----------------------------------------------------
    if (newSample) {
        float diff = rightSign * (t.compVel[1] - t.compVel[0]);  // right minus left
        S.roadTarget = c.roadGain * 0.35f * tanhf(diff * 0.6f) * Smoothstep(1.0f, 5.0f, speed);
        S.roadVib = c.roadGain * 0.12f * Clamp((fabsf(t.compVel[0]) + fabsf(t.compVel[1])) * 0.25f, 0.0f, 1.0f) *
                    Smoothstep(2.0f, 8.0f, speed);
    }
    S.roadTarget *= expf(-dt / 0.05f);
    S.road += (S.roadTarget - S.road) * Clamp(dt / 0.01f, 0.0f, 1.0f);

    // --- collisions -------------------------------------------------------------------------
    if (!S.haveHitSeq) {
        S.lastHitSeq = t.hitSeq;
        S.haveHitSeq = true;
    } else if (t.hitSeq != S.lastHitSeq) {
        S.lastHitSeq = t.hitSeq;
        if (driving) {
            S.joltTime = now;
            S.joltAmp = c.collisionGain * Clamp(t.hitMag / 10.0f, 0.25f, 1.0f);
            float lat = t.hitLat * rightSign;
            S.joltFlip = fabsf(lat) > 0.3f ? (lat > 0 ? 1 : -1) : -S.joltFlip;
            if (c.logTelemetry) Log("hit: dv=%.2f lat=%.2f amp=%.2f", t.hitMag, t.hitLat, S.joltAmp);
        }
    }
    float jolt = 0.0f, joltVib = 0.0f;
    if (S.joltTime >= 0) {
        float jt = (float)(now - S.joltTime);
        if (jt > 0.6f) {
            S.joltTime = -1;
        } else {
            float env = S.joltAmp * expf(-jt / 0.09f);
            jolt = S.joltFlip * env * cosf(2.0f * kPi * 8.0f * jt);
            joltVib = env * 0.5f;
        }
    }

    // --- vibration --------------------------------------------------------------------------
    float engine = driving ? c.engineVibration * (0.4f + 0.6f * t.rpmNorm) * (1.0f - 0.6f * Smoothstep(0.0f, 20.0f, speed)) : 0.0f;
    float engineHz = Clamp(t.rpm / 60.0f * 2.0f, 15.0f, 60.0f);
    float lock = (driving && (t.brakeLocked[0] || t.brakeLocked[1]) && speed > 3.0f) ? c.brakeLockVibration : 0.0f;
    float road = driving ? S.roadVib : 0.0f;
    o.sineMag = (engine + lock + road + (S.forceSign ? 0.0f : joltVib)) * S.fade;
    o.sineHz = lock > engine && lock > road ? 22.0f : (road > engine ? 30.0f : engineHz);

    // --- conditions -------------------------------------------------------------------------
    float damperDrive = c.damperLow + (c.damperHigh - c.damperLow) * Smoothstep(0.0f, 25.0f, speed);
    o.damper = damperDrive * S.fade + c.damperHigh * (1.0f - S.fade);
    o.friction = (c.friction + c.standstillFriction * (1.0f - Smoothstep(0.5f, 5.0f, speed))) * S.fade + c.friction * (1.0f - S.fade);
    // Centring spring: weak when parked, full from CenterSpringSpeed up (speed is in m/s).
    float springSpeed = Clamp(speed * 3.6f / Clamp(c.centerSpringSpeed, 10.0f, 400.0f), 0.0f, 1.0f);
    float centre = c.centerSpring * (c.centerSpringParked + (1.0f - c.centerSpringParked) * springSpeed);
    o.spring = c.menuSpring * (1.0f - S.fade) + (precalSpring + centre) * S.fade;

    // --- constant ---------------------------------------------------------------------------
    float total = (S.sat + S.road + (S.forceSign ? jolt : 0.0f)) * S.fade;
    o.constant = total;

    // master gain
    o.constant *= c.gain;
    o.spring *= c.gain;
    o.damper *= c.gain;
    o.friction *= c.gain;
    o.sineMag *= c.gain;

    // --- soft lock: the wheel stops where the game's steering is at full lock ---------------
    // (the gameplay mod's [Controls] SteeringLock). Not scaled by Gain: it marks a hard limit.
    if (S.haveAxis && c.softLockAt < 0.98f && c.softLock > 0.0f) {
        float over = fabsf(S.axis) - c.softLockAt;
        if (over > 0.0f) {
            // Full push within ~4% of travel past the lock, plus damping so it doesn't bounce.
            float push = c.softLock * Clamp(over / 0.04f, 0.0f, 1.0f);
            float damp = Clamp(S.axisVel * Sign(S.axis) * 0.15f, 0.0f, 0.5f) * c.softLock;
            o.constant -= Sign(S.axis) * (push + damp);
        }
    }

    // --- calibration from driving data ------------------------------------------------------
    if (newSample && driving && S.haveAxis && speed > 12.0f && onF == 2 && fabsf(S.axis) > 0.04f &&
        fabsf(t.rearSlipAngle) < 0.15f && t.wheelsOnGround == 4) {
        if (fabsf(alphaRaw) > 0.004f && fabsf(alphaRaw) < 0.12f) S.slip.Add(alphaRaw, S.axis);
        if (fabsf(t.yawRate) > 0.05f) S.right.Add(t.yawRate, S.axis);
        int prevSlip = S.slip.sign, prevRight = S.right.sign;
        bool changed = S.slip.Decide(S.slip.sign ? 600 : 90);
        changed |= S.right.Decide(S.right.sign ? 600 : 90);
        if (changed) {
            Log("calibration: slipSign %d -> %d, rightSign %d -> %d", prevSlip, S.slip.sign, prevRight, S.right.sign);
            SaveCalibration(S.slip.sign, S.right.sign, S.forceSign);
        }
    }

    if (c.logTelemetry && now - S.logTime > 0.1) {
        S.logTime = now;
        Log("tel: drv=%d v=%.1f fwd=%.1f slip=%.3f/%.3f rear=%.3f load=%.0f/%.0f(ref %.0f) comp=%.2f/%.2f steer=%.3f "
            "max=%.1f yaw=%.2f rpm=%.0f(%.2f) axis=%.3f | sat=%.3f road=%.3f jolt=%.3f const=%.3f spr=%.2f dmp=%.2f fr=%.2f vib=%.3f@%.0f",
            driving, speed, t.fwdSpeed, t.slipAngle[0], t.slipAngle[1], t.rearSlipAngle, t.load[0], t.load[1], S.loadRef,
            t.compVel[0], t.compVel[1], t.steer, t.maxSteer, t.yawRate, t.rpm, t.rpmNorm, S.axis, S.sat, S.road, jolt,
            o.constant, o.spring, o.damper, o.friction, o.sineMag, o.sineHz);
    }
}

// Determine which constant-force sign turns the wheel right: a short push each way while in menus.
// Returns true while the test owns the wheel.
static bool PulseTest(double now, bool driving, Out& o) {
    if (S.forceSign || !s_fx.constant || !S.haveAxis) return false;
    if (S.pulsePhase == 0) {
        if (driving || now < S.pulseNext || S.pulseTries >= 6 || fabsf(S.axisVel) > 0.05f || fabsf(S.axis) > 0.6f) return false;
        S.pulsePhase = 1;
        S.pulseTime = now;
        S.pulseAxis[0] = S.axis;
        S.pulseTries++;
    }
    float t = (float)(now - S.pulseTime);
    const float push = 0.3f, len = 0.12f;
    o = Out();
    if (S.pulsePhase == 1) {
        o.constant = push;
        if (t >= len) { S.pulseAxis[1] = S.axis; S.pulsePhase = 2; S.pulseTime = now; }
    } else if (S.pulsePhase == 2) {
        o.constant = -push;
        if (t >= len) { S.pulseAxis[2] = S.axis; S.pulsePhase = 3; S.pulseTime = now; }
    } else {
        o.constant = 0;
        S.pulsePhase = 0;
        float d1 = S.pulseAxis[1] - S.pulseAxis[0], d2 = S.pulseAxis[2] - S.pulseAxis[1];
        if (fabsf(d1) > 0.004f && fabsf(d2) > 0.004f && (d1 > 0) != (d2 > 0)) {
            S.forceSign = d1 > 0 ? 1 : -1;
            Log("calibration: force direction detected, ForceSign=%d (moves %.4f / %.4f)", S.forceSign, d1, d2);
            SaveCalibration(S.slip.sign, S.right.sign, S.forceSign);
        } else {
            Log("calibration: force direction test inconclusive (moves %.4f / %.4f), retrying later", d1, d2);
            S.pulseNext = now + 8.0;
        }
        return false;
    }
    return true;
}

static void Apply(const Out& o, double now) {
    float sign = (float)S.forceSign * (g_cfg.invertForce ? -1.0f : 1.0f);
    float v = o.constant;
    if (S.pulsePhase == 0) {
        v = SoftClip(v);
        if (fabsf(v) > 0.74f) S.clipCount++;
        if (g_cfg.minForce > 0 && fabsf(v) > 0.002f) v = Sign(v) * (g_cfg.minForce + (1.0f - g_cfg.minForce) * fabsf(v));
        v *= sign;  // 0 until the direction is known
    } else {
        v *= g_cfg.invertForce ? -1.0f : 1.0f;
    }
    SetConstant(v, now);
    SetCondition(s_fx.spring, s_fx.springCoef, o.spring);
    SetCondition(s_fx.damper, s_fx.damperCoef, o.damper);
    SetCondition(s_fx.friction, s_fx.frictionCoef, o.friction);
    SetSine(o.sineMag, o.sineHz);
}

static DWORD WINAPI FfbThread(LPVOID) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const double period = 1.0 / g_cfg.updateHz;
    double next = Now(), last = Now();
    for (;;) {
        double now = Now();
        if (now < next) {
            Sleep(next - now > 0.002 ? 1 : 0);
            continue;
        }
        next += period;
        if (next < now) next = now + period;
        float dt = (float)Clamp(now - last, 0.0, 0.1);
        last = now;

        Telemetry t;
        Telemetry_Get(&t);

        EnterCriticalSection(&g_diLock);
        if (!s_triedFallback && s_numDevs == 0 && s_di && now - s_diTime > 10.0) TryFallbackDevice();
        if (s_ownDev && !s_ownDev->acquired) s_ownDev->Acquire();

        DeviceWrap* dev = PickDevice();
        if (dev) {
            bool none = !s_fx.constant && !s_fx.spring && !s_fx.damper;
            if (dev != s_fx.dev || (none && now - s_fx.createTime > 2.0)) {
                CreateEffects(dev);
                s_fx.createTime = now;
            } else if (s_fx.gen != dev->acquireGen) {
                s_fx.gen = dev->acquireGen;
                StartAll();
            }
        }
        if (dev) {
            float a;
            if (ReadAxis(dev, now, &a)) {
                if (S.haveAxis && dt > 0) S.axisVel += ((a - S.axis) / dt - S.axisVel) * Clamp(dt / 0.1f, 0.0f, 1.0f);
                S.axis = a;
                S.haveAxis = true;
            }
        }
        LeaveCriticalSection(&g_diLock);

        Out o;
        Compute(t, now, dt, o);
        bool driving = t.driving && t.valid && (now - t.time) < 0.2;

        EnterCriticalSection(&g_diLock);
        if (dev && dev == s_fx.dev) {
            PulseTest(now, driving, o);
            Apply(o, now);
        }
        LeaveCriticalSection(&g_diLock);

        S.tickCount++;
        if (now - S.statTime > 30.0) {
            if (S.statTime > 0 && S.clipCount > S.tickCount / 20)
                Log("ffb: constant force clipped %d of %d ticks - consider lowering Gain", S.clipCount, S.tickCount);
            S.statTime = now;
            S.clipCount = S.tickCount = 0;
        }
    }
    return 0;
}

void Ffb_Start() {
    S.forceSign = g_cfg.forceSign;
    S.slip.sign = g_cfg.slipSign;
    S.right.sign = g_cfg.rightSign;
    CreateThread(nullptr, 0, FfbThread, nullptr, 0, nullptr);
    Log("ffb: engine started at %d Hz", g_cfg.updateHz);
}
