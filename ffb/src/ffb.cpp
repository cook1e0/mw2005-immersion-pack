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
#include <stdio.h>
#include <algorithm>

static const float kPi = 3.14159265f;

// ---------------------------------------------------------------------------
// Devices (guarded by g_diLock)
// ---------------------------------------------------------------------------
static DeviceWrap* s_devs[8];
static int s_numDevs;
static IDirectInput8A* s_di;
static double s_diTime;
static double s_fallbackNext;
static volatile LONG s_started, s_resetRequested;
static volatile LONG s_menuOpen, s_resumeRequested;

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
    double healthTime = 0, retryTime = 0, recoveryTime = 0;
    bool ready = false, restartNeeded = false, rebuildNeeded = false, inhibited = false;
    LONG effectAxis = -1;
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

bool Ffb_RequestReset() {
    if (!g_cfg.enabled || g_cfg.nativeFfb || !InterlockedCompareExchange(&s_started, 0, 0)) return false;
    InterlockedExchange(&s_resetRequested, 1);
    return true;
}

void Ffb_SetMenuOpen(bool open) {
    if (!g_cfg.enabled || g_cfg.nativeFfb) return;
    InterlockedExchange(&s_menuOpen, open ? 1 : 0);
    InterlockedExchange(&s_resumeRequested, 1);
}

static bool Lost(HRESULT hr) {
    return hr == (HRESULT)DIERR_NOTEXCLUSIVEACQUIRED || hr == DIERR_INPUTLOST ||
           hr == DIERR_NOTACQUIRED || hr == (HRESULT)DIERR_UNPLUGGED || hr == DIERR_OTHERAPPHASPRIO;
}

static bool EffectResult(HRESULT hr) {
    if (SUCCEEDED(hr) && hr != DI_DOWNLOADSKIPPED) return true;
    if (s_fx.ready) {
        for (auto fx : {s_fx.constant, s_fx.spring, s_fx.damper, s_fx.friction, s_fx.sine})
            if (fx) fx->Stop();
    }
    s_fx.ready = false;
    s_fx.restartNeeded = true;
    if (Lost(hr) || hr == DI_DOWNLOADSKIPPED) {
        if (s_fx.dev) s_fx.dev->acquired = false;
    } else s_fx.rebuildNeeded = true;
    static HRESULT last = DI_OK;
    static double logged = -1e9;
    double now = Now();
    if (hr != last || now - logged >= 5.0) {
        Log("ffb: effect operation failed 0x%08lX; recovery scheduled", hr);
        last = hr; logged = now;
    }
    return false;
}

static LONG s_constDir;
static void ResetCachedParameters() {
    s_constDir = 0;   // unknown: the next update sends the direction
    s_fx.constTime = 0;
    s_fx.springCoef = s_fx.damperCoef = s_fx.frictionCoef = -1;
    s_fx.sineMag = 0xFFFFFFFF;
}

static bool ConfigureWheel(DeviceWrap* d) {
    DIPROPDWORD p = {};
    p.diph.dwSize = sizeof(p); p.diph.dwHeaderSize = sizeof(p.diph); p.diph.dwHow = DIPH_DEVICE;
    p.dwData = DIPROPAUTOCENTER_OFF;
    d->real->SetProperty(DIPROP_AUTOCENTER, &p.diph);
    p.dwData = DI_FFNOMINALMAX;
    d->real->SetProperty(DIPROP_FFGAIN, &p.diph);
    HRESULT hr = d->real->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
    return hr == DIERR_UNSUPPORTED || EffectResult(hr);
}

static bool SetParams(IDirectInputEffect* fx, void* params, DWORD size);

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
        if (Lost(hr)) EffectResult(hr);
        return nullptr;
    }
    return fx;
}

static bool StartAll() {
    s_fx.restartNeeded = s_fx.rebuildNeeded = false;
    bool ok = true, any = false;
    IDirectInputEffect* all[] = {s_fx.constant, s_fx.spring, s_fx.damper, s_fx.friction, s_fx.sine};
    DICONSTANTFORCE zero = {0};
    DICONDITION condition = {};
    condition.dwPositiveSaturation = condition.dwNegativeSaturation = DI_FFNOMINALMAX;
    DIPERIODIC sine = {}; sine.dwPeriod = 25000;
    void* parameters[] = {&zero, &condition, &condition, &condition, &sine};
    DWORD sizes[] = {sizeof(zero), sizeof(condition), sizeof(condition), sizeof(condition), sizeof(sine)};
    for (int i = 0; i < 5; ++i) {
        if (!all[i]) continue;
        any = true;
        // Clear the last driving forces before restarting downloaded effects.
        if (!SetParams(all[i], parameters[i], sizes[i]) || !EffectResult(all[i]->Start(1, 0))) ok = false;
    }
    ResetCachedParameters();
    s_fx.ready = any && ok;
    if (s_fx.ready) {
        s_fx.gen = s_fx.dev->acquireGen;
        s_fx.recoveryTime = Now();
    }
    return s_fx.ready;
}

static bool CreateEffects(DeviceWrap* d) {
    double created = s_fx.createTime;
    ReleaseEffects();
    s_fx.createTime = created;
    s_fx.dev = d;
    s_fx.effectAxis = d->axisOfs;
    if (!ConfigureWheel(d)) return false;

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
    bool started = StartAll();
    // Some wheels show up as two force feedback devices of the same name: log which one this is.
    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(caps);
    d->real->GetCapabilities(&caps);
    Log("ffb: effects on '%s' (%lu axes, %lu buttons): constant=%d spring=%d damper=%d friction=%d sine=%d", d->name,
        caps.dwAxes, caps.dwButtons, !!s_fx.constant, !!s_fx.spring, !!s_fx.damper, !!s_fx.friction, !!s_fx.sine);
    return started;
}

static bool SetParams(IDirectInputEffect* fx, void* params, DWORD size) {
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.cbTypeSpecificParams = size;
    e.lpvTypeSpecificParams = params;
    HRESULT hr = fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
    return EffectResult(hr);
}

static void SetCondition(IDirectInputEffect* fx, LONG& last, float coef) {
    if (!fx) return;
    LONG c = (LONG)(Clamp(coef, 0.0f, 1.0f) * DI_FFNOMINALMAX);
    if (abs(c - last) < 50) return;
    DICONDITION cond = {};
    cond.lPositiveCoefficient = cond.lNegativeCoefficient = c;
    cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
    if (SetParams(fx, &cond, sizeof(cond))) last = c;
}

// ForceDirection=1 sends the side as the effect's direction (+1 / -1) with a positive magnitude,
// for drivers that ignore negative magnitudes; the direction is only resent when it flips.
static void SetConstant(float v, double now) {
    if (!s_fx.constant) return;
    LONG m = (LONG)lrintf(Clamp(v, -1.0f, 1.0f) * DI_FFNOMINALMAX);
    if (abs(m - s_fx.constMag) < 15 && now - s_fx.constTime < 0.05) return;
    DICONSTANTFORCE cf = {m};
    bool ok;
    if (g_cfg.forceDirection) {
        LONG dir = m < 0 ? -1 : 1;
        cf.lMagnitude = labs(m);
        DWORD axes[1] = {AxisOfs(s_fx.dev)};
        LONG dirs[1] = {dir};
        DIEFFECT e = {};
        e.dwSize = sizeof(e);
        e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        e.cAxes = 1;
        e.rgdwAxes = axes;
        e.rglDirection = dirs;
        e.cbTypeSpecificParams = sizeof(cf);
        e.lpvTypeSpecificParams = &cf;
        ok = EffectResult(s_fx.constant->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | (dir != s_constDir ? DIEP_DIRECTION : 0)));
        if (ok) s_constDir = dir;
    } else {
        ok = SetParams(s_fx.constant, &cf, sizeof(cf));
    }
    if (ok) {
        s_fx.constMag = m; s_fx.constTime = now;
    }
}

static void SetSine(float mag, float hz) {
    if (!s_fx.sine) return;
    DWORD m = (DWORD)(Clamp(mag, 0.0f, 1.0f) * DI_FFNOMINALMAX);
    DWORD period = (DWORD)(1e6f / Clamp(hz, 5.0f, 120.0f));
    bool periodChanged = !s_fx.sinePeriod || fabsf((float)period / (float)s_fx.sinePeriod - 1.0f) > 0.08f;
    if (abs((LONG)m - (LONG)s_fx.sineMag) < 30 && !periodChanged) return;
    if (!periodChanged) period = s_fx.sinePeriod;
    DIPERIODIC p = {};
    p.dwMagnitude = m;
    p.dwPeriod = period;
    if (SetParams(s_fx.sine, &p, sizeof(p))) {
        s_fx.sineMag = m; s_fx.sinePeriod = period;
    }
}

// Normalised steering axis position (-1 left .. +1 right). Returns false if unavailable.
static bool ReadAxis(DeviceWrap* d, double now, float* out) {
    if (!d->acquired || d->axisOfs < 0 || d->dataSize < sizeof(LONG) || d->dataSize > 1024 ||
        (DWORD)d->axisOfs > d->dataSize - sizeof(LONG)) return false;
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
    HRESULT hr = d->real->Poll();
    if (Lost(hr)) { EffectResult(hr); return false; }
    hr = d->real->GetDeviceState(d->dataSize, buf);
    if (FAILED(hr)) { if (Lost(hr)) EffectResult(hr); return false; }
    LONG raw = *(LONG*)(buf + d->axisOfs);
    float mid = 0.5f * ((float)s_fx.rangeMin + (float)s_fx.rangeMax);
    float half = 0.5f * ((float)s_fx.rangeMax - (float)s_fx.rangeMin);
    *out = Clamp(((float)raw - mid) / half, -1.0f, 1.0f);
    return true;
}

// Some wheels (Fanatec) show up as two force feedback devices of the same name, and only one
// drives the motor. [FFB] Device picks by Windows' order (the order joy.cpl lists them): the
// game may open them in any order. -1 = whichever the game opened first.
static GUID s_wantGuid;
static int s_wantState;     // 0 = not looked up yet, 1 = known, -1 = not found (any device)

struct FfbEnum { int index, want; GUID guid; bool found; };
static BOOL CALLBACK NthFfbDeviceCb(LPCDIDEVICEINSTANCEA inst, LPVOID p) {
    FfbEnum* e = (FfbEnum*)p;
    if (e->index++ != e->want) return DIENUM_CONTINUE;
    e->guid = inst->guidInstance;
    e->found = true;
    Log("ffb: using FFB device %d in Windows' order: '%s'", e->want, inst->tszProductName);
    return DIENUM_STOP;
}

static bool IsWanted(DeviceWrap* d) {
    if (s_wantState != 1) return false;
    DIDEVICEINSTANCEA info = {};
    info.dwSize = sizeof(info);
    return SUCCEEDED(d->real->GetDeviceInfo(&info)) && info.guidInstance == s_wantGuid;
}

// For the F4 menu: name and buttons of the nth FFB device in Windows' order. Returns the count.
struct FfbNames { int index, want; char* out; int len; };
static BOOL CALLBACK NameFfbDeviceCb(LPCDIDEVICEINSTANCEA inst, LPVOID p) {
    FfbNames* e = (FfbNames*)p;
    if (e->index++ == e->want && e->out) {
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        IDirectInputDevice8A* d = nullptr;
        if (s_di && SUCCEEDED(s_di->CreateDevice(inst->guidInstance, &d, nullptr)) && d) {
            d->GetCapabilities(&caps);
            d->Release();
        }
        snprintf(e->out, e->len, "%d: %s (%lu axes, %lu buttons)", e->want, inst->tszProductName, caps.dwAxes, caps.dwButtons);
    }
    return DIENUM_CONTINUE;
}

int Ffb_DeviceName(int index, char* out, int len) {
    EnterCriticalSection(&g_diLock);
    FfbNames e = {0, index, out, len};
    if (out && len > 0) out[0] = 0;
    if (s_di) s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, NameFfbDeviceCb, &e, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    LeaveCriticalSection(&g_diLock);
    return e.index;
}

static int s_wantDevice = -2;   // g_cfg.device the lookup was made for

static DeviceWrap* PickDevice() {
    int device = (int)lrintf(g_cfg.device);
    if (device != s_wantDevice) s_wantState = 0;   // changed in F4: look it up again
    if (!s_wantState && s_di) {
        s_wantState = -1;
        s_wantDevice = device;
        if (device >= 0) {
            FfbEnum e = {0, device, {}, false};
            s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, NthFfbDeviceCb, &e, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
            if (e.found) { s_wantGuid = e.guid; s_wantState = 1; }
            else Log("ffb: no FFB device %d, using the first one the game opens", device);
        }
    }
    for (int i = 0; i < s_numDevs; ++i)
        if (s_devs[i]->acquired && s_devs[i]->exclusive && IsWanted(s_devs[i])) return s_devs[i];
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
    if (FAILED(w->SetDataFormat(&df)) || FAILED(w->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_FOREGROUND))) {
        real->Release(); delete w;
        Log("ffb: fallback device setup failed; will retry");
        return;
    }
    Ffb_RegisterDevice(w);
    w->Acquire();
    s_ownDev = w;
    Log("ffb: game did not open a force feedback device; opened '%s' ourselves", found.name);
}

static bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool s_focusKnown, s_focused;
static void UpdateFocus(bool focused) {
    if (s_focusKnown && focused == s_focused) return;
    s_focusKnown = true; s_focused = focused;
    if (!focused) {
        for (auto fx : {s_fx.constant, s_fx.spring, s_fx.damper, s_fx.friction, s_fx.sine})
            if (fx) fx->Stop();
        s_fx.ready = false;
        s_fx.restartNeeded = true;
        return;
    }
    // The game's own focus handling can leave the wheel unacquired. Re-arm handles
    // that previously worked, including ones explicitly released during Alt-Tab.
    for (int i = 0; i < s_numDevs; ++i) {
        DeviceWrap* d = s_devs[i];
        if (!d->exclusive || d->acquireGen == 0) continue;
        d->acquireWanted = true; d->acquired = false; d->acquireRetry = 0;
    }
    s_fx.restartNeeded = true;
    s_fx.healthTime = s_fx.retryTime = 0;
}

static void CheckEffectHealth(double now) {
    if (!s_fx.dev || now < s_fx.healthTime) return;
    s_fx.healthTime = now + 0.5;
    DWORD state = 0;
    HRESULT hr = s_fx.dev->real->GetForceFeedbackState(&state);
    if (Lost(hr)) { EffectResult(hr); return; }
    if (SUCCEEDED(hr)) {
        // Respect hardware power, safety and user switches. Missing flags mean unknown.
        s_fx.inhibited = (state & (DIGFFS_POWEROFF | DIGFFS_SAFETYSWITCHOFF | DIGFFS_USERFFSWITCHOFF)) != 0;
        if (s_fx.inhibited) {
            s_fx.ready = false; s_fx.restartNeeded = true;
            return;
        }
        if (state & DIGFFS_DEVICELOST) {
            s_fx.dev->real->Unacquire();
            s_fx.dev->acquired = false;
            s_fx.ready = false; s_fx.rebuildNeeded = true;
            return;
        }
        if (state & DIGFFS_EMPTY) { s_fx.ready = false; s_fx.restartNeeded = true; }
        if (state & DIGFFS_PAUSED) {
            HRESULT continued = s_fx.dev->real->SendForceFeedbackCommand(DISFFC_CONTINUE);
            if (Lost(continued)) { EffectResult(continued); return; }
            s_fx.ready = false; s_fx.restartNeeded = true;
        }
        if (state & DIGFFS_ACTUATORSOFF) {
            if (!ConfigureWheel(s_fx.dev)) return;
            s_fx.ready = false; s_fx.restartNeeded = true;
        }
    }
    // Some drivers don't support device status (or misreport STOPPED). Check the
    // individual effects too; unsupported status queries are not treated as loss.
    for (auto fx : {s_fx.constant, s_fx.spring, s_fx.damper, s_fx.friction, s_fx.sine}) {
        if (!fx) continue;
        DWORD status = 0;
        hr = fx->GetEffectStatus(&status);
        if (hr == DIERR_UNSUPPORTED) continue;
        if (hr == (HRESULT)DIERR_NOTDOWNLOADED || (SUCCEEDED(hr) && !(status & DIEGES_PLAYING))) {
            s_fx.ready = false; s_fx.restartNeeded = true;
        } else if (FAILED(hr)) EffectResult(hr);
    }
}

// All wheel calls run under g_diLock. Retry only while focused, with bounded
// frequency; healthy effects are neither reset nor restarted on every tick.
static DeviceWrap* ServiceEffects(double now, bool focused, bool manual) {
    UpdateFocus(focused);
    if (!focused) return nullptr;
    bool resume = InterlockedExchange(&s_resumeRequested, 0) != 0;
    bool menu = InterlockedCompareExchange(&s_menuOpen, 0, 0) != 0;
    if (resume || menu) {
        for (int i = 0; i < s_numDevs; ++i) {
            DeviceWrap* d = s_devs[i];
            if (d->exclusive && d->acquireGen > 0) d->acquireWanted = true;
        }
        if (resume) s_fx.healthTime = s_fx.retryTime = 0;
    }
    if (manual) {
        DeviceWrap* previous = s_fx.dev;
        ReleaseEffects();
        for (int i = 0; i < s_numDevs; ++i) {
            DeviceWrap* d = s_devs[i];
            if (!d->exclusive || (d != previous && d->acquireGen == 0 && !d->acquireWanted)) continue;
            d->real->Unacquire();
            d->acquired = false; d->acquireWanted = true; d->acquireRetry = 0;
        }
        s_fallbackNext = 0;
        Log("ffb: manual restart requested (calibration kept)");
    }
    for (int i = 0; i < s_numDevs; ++i) {
        DeviceWrap* d = s_devs[i];
        if (!d->exclusive || !d->acquireWanted || d->acquired || now < d->acquireRetry) continue;
        d->acquireRetry = now + 0.5;
        d->Acquire();
    }
    DeviceWrap* d = PickDevice();
    if (!d) return nullptr;
    if (d != s_fx.dev) {
        CreateEffects(d);
        s_fx.createTime = now; s_fx.healthTime = now + 0.5; s_fx.retryTime = now + 0.5;
    } else {
        CheckEffectHealth(now);
        if (!d->acquired || s_fx.inhibited) return nullptr;
        bool none = !s_fx.constant && !s_fx.spring && !s_fx.damper && !s_fx.friction && !s_fx.sine;
        bool changed = s_fx.gen != d->acquireGen || s_fx.effectAxis != d->axisOfs;
        if (now >= s_fx.retryTime && (none || changed || s_fx.restartNeeded || s_fx.rebuildNeeded)) {
            bool rebuild = none || s_fx.rebuildNeeded || s_fx.effectAxis != d->axisOfs;
            bool ok = rebuild ? CreateEffects(d) : ConfigureWheel(d) && StartAll();
            s_fx.healthTime = now + 0.5;
            s_fx.retryTime = now + (ok ? 0.5 : 2.0);
            if (ok) Log("ffb: wheel effects recovered on '%s'", d->name);
        }
    }
    return d->acquired && s_fx.ready ? d : nullptr;
}

// ---------------------------------------------------------------------------
// Force model
//
// Constant force (+ = turn the wheel right), built from:
//   tyre      self-aligning torque from the front slip angle and load (goes light past the peak)
//   centring  -SpeedCentering * v * sin(steering angle): none parked, stronger with speed and lock
//   understeer both of the above lose UndersteerLightening when the front tyres are past their grip
//             (slip beyond the peak, or the car yawing less than the steering asks for)
//   road      front suspension movement, left vs. right
//   surface   square-wave rumble at the game's own per-surface frequency and intensity, and a pull
//             toward the rougher side
//   kerbs     jerk of the body's vertical motion in its own frame above CurbJerk: a short push
//             away from the side that hit (both sides: a short shake)
//   impacts   head-on: a decaying shake both ways; side and angled: a one-way kick
//   scrapes   acceleration spikes beside a wall: pull toward the contact + rumble
//   air       limp in the air (AirborneForce of the steering forces), a jolt on landing
// Spring, damper and friction effects add the speed-dependent centring spring, weight and scrub.
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
    uint32_t lastHitSeq = 0, lastScrapeSeq = 0, lastLandSeq = 0;
    bool  haveSeqs = false;
    float loadRef = 0;
    float sat = 0, road = 0, centre = 0;   // smoothed components
    float roadTarget = 0, roadVib = 0;
    float understeer = 0;         // 0..1, smoothed
    float air = 1;                // 1 on the ground .. AirborneForce in the air
    float surfacePhase = 0;
    double joltTime = -1;
    float joltAmp = 0;
    int   joltFlip = 1;
    bool  joltShake = false;      // head-on: both ways
    double curbTime = -1;
    float curbAmp = 0;
    int   curbSide = 0;
    double scrapeUntil = -1;
    float scrapeAmp = 0;
    int   scrapeSide = 0;
    double landTime = -1;
    float landAmp = 0;
    Calib slip, right;
    int   forceSign = 0;
    float axis = 0, axisPrev = 0, axisVel = 0;
    bool  haveAxis = false;
    LONG  lastDir = 1;
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
    const float peak = Clamp(c.satPeakAngle, 1.0f, 30.0f) * kPi / 180.0f;

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
        float x = slipSign * alphaRaw / peak;
        float shape = x * expf(0.5f * (1.0f - x * x)) + c.driftAlign * tanhf(0.5f * x);
        satTarget = -c.satGain * shape * loadF * vs;  // tyre pulls the wheel back toward the direction of travel
    } else {
        precalSpring = c.satGain * 0.6f * vs * (onF * 0.5f);
    }
    S.sat += (satTarget - S.sat) * Clamp(dt / 0.015f, 0.0f, 1.0f);

    // --- speed-sensitive centring: -K * v * sin(steering angle) ----------------------------
    // The angle is the wheel's, as a share of the steering lock (where the front wheels are at full
    // lock, 45 degrees, with the gameplay mod's direct steering).
    float centreTarget = 0.0f;
    if (S.haveAxis && driving) {
        float lockShare = c.softLockAt < 0.98f ? c.softLockAt : 1.0f;
        float theta = Clamp(S.axis / lockShare, -1.5f, 1.5f) * (45.0f * kPi / 180.0f);
        float v = Clamp(speed * 3.6f / c.speedCenteringRef, 0.0f, 1.5f);
        centreTarget = -c.speedCentering * v * sinf(theta) * (onF * 0.5f);
    }
    S.centre += (centreTarget - S.centre) * Clamp(dt / 0.02f, 0.0f, 1.0f);

    // --- understeer: front tyres past their grip ---------------------------------------------
    float under = 0.0f;
    if (driving && onF > 0 && speed > 5.0f) {
        float alphaF = fabsf(alphaRaw), alphaR = fabsf(t.rearSlipAngle);
        under = Smoothstep(1.0f, 1.6f, alphaF / peak);
        // Yawing less than the steered front wheels ask for (single-track model), while the front
        // slides more than the rear: the car is pushing wide.
        float steer = fabsf(t.steer);
        if (steer > 0.03f && t.wheelbase > 1.0f && alphaF > alphaR * 1.2f) {
            float expected = speed * tanf(std::min(steer, 0.8f)) / t.wheelbase;
            if (expected > 0.15f) under = std::max(under, Smoothstep(0.85f, 0.5f, fabsf(t.yawRate) / expected));
        }
    }
    S.understeer += (under - S.understeer) * Clamp(dt / 0.08f, 0.0f, 1.0f);
    float lighten = 1.0f - Clamp(c.understeerLightening, 0.0f, 1.0f) * S.understeer;

    // --- airborne: the wheel goes limp ---------------------------------------------------------
    float airTarget = driving && t.airborne ? c.airborneForce : 1.0f;
    S.air += (airTarget - S.air) * Clamp(dt / (airTarget < S.air ? 0.05f : 0.15f), 0.0f, 1.0f);

    // --- road: front suspension movement ----------------------------------------------------
    if (newSample) {
        float diff = rightSign * (t.compVel[1] - t.compVel[0]);  // right minus left
        S.roadTarget = c.roadGain * 0.35f * tanhf(diff * 0.6f) * Smoothstep(1.0f, 5.0f, speed);
        S.roadVib = c.roadGain * 0.12f * Clamp((fabsf(t.compVel[0]) + fabsf(t.compVel[1])) * 0.25f, 0.0f, 1.0f) *
                    Smoothstep(2.0f, 8.0f, speed);
    }
    S.roadTarget *= expf(-dt / 0.05f);
    S.road += (S.roadTarget - S.road) * Clamp(dt / 0.01f, 0.0f, 1.0f);

    // --- surface rumble -----------------------------------------------------------------------
    float surface = 0.0f;
    if (driving && onF > 0) {
        float rough[2] = {t.onGround[0] ? t.surfaceRumble[0] : 0.0f, t.onGround[1] ? t.surfaceRumble[1] : 0.0f};
        float amount = c.surfaceRumble * Clamp(std::max(rough[0], rough[1]) * 1.5f, 0.0f, 1.0f) * Smoothstep(1.0f, 8.0f, speed);
        float hzBase = std::max(t.surfaceHz[0], t.surfaceHz[1]);
        float hz = Clamp((hzBase > 0 ? hzBase : 18.0f) * Clamp(speed / 15.0f, 0.4f, 1.5f), 6.0f, 40.0f);
        S.surfacePhase += dt * hz;
        S.surfacePhase -= floorf(S.surfacePhase);
        surface = (S.surfacePhase < 0.5f ? 0.5f : -0.5f) * amount;
        // One side on the grass pulls the wheel that way.
        surface += 0.3f * c.surfaceRumble * rightSign * (rough[1] - rough[0]) * Smoothstep(2.0f, 10.0f, speed);
    }

    // --- events: impacts, kerbs, scrapes, landings -------------------------------------------
    if (!S.haveSeqs) {
        S.lastHitSeq = t.hitSeq; S.lastScrapeSeq = t.scrapeSeq; S.lastLandSeq = t.landSeq;
        S.haveSeqs = true;
    }
    if (t.hitSeq != S.lastHitSeq) {
        S.lastHitSeq = t.hitSeq;
        if (driving) {
            S.joltTime = now;
            S.joltShake = fabsf(t.hitLon) > 0.75f;       // head-on (or from behind)
            S.joltAmp = c.collisionGain * Clamp(t.hitMag / 10.0f, 0.25f, 1.0f);
            float lat = t.hitLat * rightSign;
            S.joltFlip = lat > 0 ? 1 : -1;               // kicked the way the car was shoved
            if (c.logTelemetry) Log("hit: dv=%.2f lat=%.2f lon=%.2f amp=%.2f %s", t.hitMag, t.hitLat, t.hitLon, S.joltAmp,
                                    S.joltShake ? "head-on" : "kick");
        }
    }
    if (t.scrapeSeq != S.lastScrapeSeq) {
        S.lastScrapeSeq = t.scrapeSeq;
        if (driving && speed > 2.0f) {
            S.scrapeUntil = now + 0.25;
            S.scrapeAmp = c.scrapes * Clamp(t.scrapeMag / 40.0f, 0.3f, 1.0f);
            S.scrapeSide = t.scrapeLat * rightSign > 0 ? 1 : -1;   // the car was pushed this way
        }
    }
    if (t.landSeq != S.lastLandSeq) {
        S.lastLandSeq = t.landSeq;
        if (driving && t.landSpeed > 1.0f) {
            S.landTime = now;
            S.landAmp = c.landing * Clamp(t.landSpeed / 6.0f, 0.2f, 1.0f);
            if (c.logTelemetry) Log("landing: %.1f m/s, amp %.2f", t.landSpeed, S.landAmp);
        }
    }
    if (newSample && driving && onF > 0 && speed > 2.0f && fabsf(t.vertJerk) > c.curbJerk && now - S.curbTime > 0.1) {
        float side = rightSign * (fabsf(t.compAccel[1]) - fabsf(t.compAccel[0]));   // > 0: the right wheel hit
        float both = fabsf(t.compAccel[0]) + fabsf(t.compAccel[1]);
        S.curbTime = now;
        S.curbAmp = c.curbs * Clamp(fabsf(t.vertJerk) / (c.curbJerk * 5.0f), 0.25f, 1.0f);
        S.curbSide = both > 0 && fabsf(side) > 0.3f * both ? (side > 0 ? 1 : -1) : 0;
        if (c.logTelemetry) Log("kerb: jerk %.0f side %d amp %.2f", t.vertJerk, S.curbSide, S.curbAmp);
    }

    float jolt = 0.0f, joltVib = 0.0f;
    if (S.joltTime >= 0) {
        float jt = (float)(now - S.joltTime);
        if (jt > 0.5f) {
            S.joltTime = -1;
        } else if (S.joltShake) {
            float env = S.joltAmp * expf(-jt / 0.07f);
            jolt = S.joltFlip * env * cosf(2.0f * kPi * 14.0f * jt);
            joltVib = env * 0.5f;
        } else {
            float env = S.joltAmp * 0.8f * expf(-jt / 0.1f);
            jolt = S.joltFlip * env;
            joltVib = env * 0.3f;
        }
    }
    float curb = 0.0f;
    if (S.curbTime >= 0) {
        float ct = (float)(now - S.curbTime);
        if (ct > 0.08f) S.curbTime = -1;
        else if (S.curbSide) curb = -S.curbSide * S.curbAmp * sinf(kPi * ct / 0.08f);   // away from the kerb
        else curb = S.curbAmp * 0.7f * sinf(2.0f * kPi * 25.0f * ct);
    }
    float scrape = 0.0f, scrapeVib = 0.0f;
    if (now < S.scrapeUntil) {
        float env = Clamp((float)(S.scrapeUntil - now) / 0.1f, 0.0f, 1.0f);
        scrape = -S.scrapeSide * S.scrapeAmp * 0.35f * env;   // toward the wall: against the push
        scrapeVib = S.scrapeAmp * 0.5f * env;
    }
    float land = 0.0f;
    if (S.landTime >= 0) {
        float lt = (float)(now - S.landTime);
        if (lt > 0.3f) S.landTime = -1;
        else land = S.landAmp * expf(-lt / 0.06f) * cosf(2.0f * kPi * 9.0f * lt);
    }

    // --- vibration --------------------------------------------------------------------------
    float engine = driving ? c.engineVibration * (0.4f + 0.6f * t.rpmNorm) * (1.0f - 0.6f * Smoothstep(0.0f, 20.0f, speed)) : 0.0f;
    float engineHz = Clamp(t.rpm / 60.0f, 10.0f, 100.0f);   // one ripple per crank turn
    float lock = (driving && (t.brakeLocked[0] || t.brakeLocked[1]) && speed > 3.0f) ? c.brakeLockVibration : 0.0f;
    float road = driving ? S.roadVib * S.air : 0.0f;
    float vib[4] = {engine, lock, road, scrapeVib};
    float hzs[4] = {engineHz, 22.0f, 30.0f, 45.0f};
    int strongest = 0;
    for (int i = 1; i < 4; i++) if (vib[i] > vib[strongest]) strongest = i;
    o.sineMag = (engine + lock + road + scrapeVib + joltVib * 0.3f) * S.fade;
    o.sineHz = hzs[strongest];

    // --- centring spring and damper ----------------------------------------------------------
    // Computed here and sent with the rest on the constant force, as sims do: wheels scale the
    // DirectInput spring and damper effects by their own settings (Fanatec SPR / DPR, often low or
    // off), which left the wheel where it was at speed. Without the wheel's position (no axis yet)
    // the spring and damper effects stand in.
    float springSpeed = Clamp(speed * 3.6f / Clamp(c.centerSpringSpeed, 10.0f, 400.0f), 0.0f, 1.0f);
    float centre = c.centerSpring * (c.centerSpringParked + (1.0f - c.centerSpringParked) * springSpeed);
    float springK = c.menuSpring * (1.0f - S.fade) + (precalSpring + centre) * S.fade * S.air * lighten;
    float damperDrive = c.damperLow + (c.damperHigh - c.damperLow) * Smoothstep(0.0f, 25.0f, speed);
    float damperK = damperDrive * S.fade * std::max(S.air, 0.3f) + c.damperHigh * (1.0f - S.fade);
    float spring = 0.0f, damping = 0.0f;
    if (S.haveAxis) {
        // Full spring force at half the steering lock; the damper per wheel speed (lock share / s).
        float lockShare = c.softLockAt < 0.98f ? c.softLockAt : 1.0f;
        spring = -springK * Clamp(S.axis / lockShare * 2.0f, -1.0f, 1.0f);
        damping = -damperK * 0.25f * Clamp(S.axisVel / lockShare, -4.0f, 4.0f);
        o.spring = o.damper = 0.0f;
    } else {
        o.spring = springK;
        o.damper = damperK;
    }
    o.friction = (c.friction + c.standstillFriction * (1.0f - Smoothstep(0.5f, 5.0f, speed))) * S.fade * S.air + c.friction * (1.0f - S.fade);

    // --- constant ---------------------------------------------------------------------------
    float steering = ((S.sat + S.centre) * lighten + S.road + surface) * S.air;
    float events = jolt + curb + scrape + land;
    o.constant = (steering + events) * S.fade + spring + damping;

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
            "yaw=%.2f rpm=%.0f axis=%.3f air=%d(%.2f) jerk=%.0f surf=%.2f/%.2f@%.0f roll=%.2f/%.2f | sat=%.3f centre=%.3f "
            "under=%.2f road=%.3f surf=%.3f const=%.3f spr=%.2f dmp=%.2f fr=%.2f vib=%.3f@%.0f",
            driving, speed, t.fwdSpeed, t.slipAngle[0], t.slipAngle[1], t.rearSlipAngle, t.load[0], t.load[1], S.loadRef,
            t.compVel[0], t.compVel[1], t.steer, t.yawRate, t.rpm, S.axis, t.airborne, t.airTime, t.vertJerk,
            t.surfaceRumble[0], t.surfaceRumble[1], std::max(t.surfaceHz[0], t.surfaceHz[1]), t.rollingResist[0],
            t.rollingResist[1], S.sat, S.centre, S.understeer, S.road, surface, o.constant, o.spring, o.damper,
            o.friction, o.sineMag, o.sineHz);
    }
}

// F4 > Force feedback > Test: a firm push to the right for half a second, so the player can see
// that the right device moves and in which direction (Reverse force direction fixes the latter).
static volatile LONG s_testRequested;
static double s_testUntil = -1;

bool Ffb_RequestTest() {
    if (!g_cfg.enabled || g_cfg.nativeFfb || !InterlockedCompareExchange(&s_started, 0, 0)) return false;
    InterlockedExchange(&s_testRequested, 1);
    return true;
}

static bool ForceTest(double now, Out& o) {
    if (InterlockedExchange(&s_testRequested, 0)) {
        s_testUntil = now + 0.5;
        Log("ffb: test push to the right");
    }
    if (now >= s_testUntil) return false;
    o = Out();
    o.constant = 0.35f;
    return true;
}

static void Apply(const Out& o, double now) {
    // Bring forces back gradually after acquisition/restart, rather than applying
    // the previous driving torque immediately when focus returns.
    float fade = Clamp((float)(now - s_fx.recoveryTime) / 0.25f, 0.0f, 1.0f);
    float sign = (float)S.forceSign * (g_cfg.invertForce ? -1.0f : 1.0f);
    float v = SoftClip(o.constant);
    if (fabsf(v) > 0.74f) S.clipCount++;
    if (g_cfg.minForce > 0 && fabsf(v) > 0.002f) v = Sign(v) * (g_cfg.minForce + (1.0f - g_cfg.minForce) * fabsf(v));
    v *= sign;
    SetConstant(v * fade, now);
    if (!s_fx.ready) return;
    SetCondition(s_fx.spring, s_fx.springCoef, o.spring * fade);
    if (!s_fx.ready) return;
    SetCondition(s_fx.damper, s_fx.damperCoef, o.damper * fade);
    if (!s_fx.ready) return;
    SetCondition(s_fx.friction, s_fx.frictionCoef, o.friction * fade);
    if (!s_fx.ready) return;
    SetSine(o.sineMag * fade, o.sineHz);
}

static DWORD WINAPI FfbThread(LPVOID) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const double period = 1.0 / g_cfg.updateHz;
    double next = Now(), last = Now();
    bool keyDown = false;
    DeviceWrap* axisDevice = nullptr;
    LONG axisGen = -1;
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

        bool focused = GameFocused();
        bool down = g_cfg.keyReset && (GetAsyncKeyState(g_cfg.keyReset) & 0x8000);
        if (focused && down && !keyDown) Ffb_RequestReset();
        keyDown = down;
        bool manual = focused && InterlockedExchange(&s_resetRequested, 0) != 0;

        EnterCriticalSection(&g_diLock);
        DeviceWrap* dev = ServiceEffects(now, focused, manual);
        if (focused && s_numDevs == 0 && s_di && now >= s_fallbackNext && (manual || now - s_diTime > 10.0)) {
            s_fallbackNext = now + 3.0;
            TryFallbackDevice();
            dev = ServiceEffects(now, focused, false);
        }
        if (!dev || dev != axisDevice || dev->acquireGen != axisGen) {
            S.haveAxis = false; S.axisVel = 0;
            axisDevice = dev; axisGen = dev ? dev->acquireGen : -1;
        }
        if (dev) {
            float a;
            static int axisLogged;   // 1 = read fine, -1 = failing (logged once each)
            bool read = ReadAxis(dev, now, &a);
            if (read != (axisLogged == 1)) {
                axisLogged = read ? 1 : -1;
                if (read) Log("ffb: reading the wheel position (axis offset %ld, %.3f)", dev->axisOfs, a);
                else Log("ffb: cannot read the wheel position (axis offset %ld): centring falls back to the wheel's spring effect", dev->axisOfs);
            }
            if (read) {
                if (S.haveAxis && dt > 0) S.axisVel += ((a - S.axis) / dt - S.axisVel) * Clamp(dt / 0.02f, 0.0f, 1.0f);
                S.axis = a;
                S.haveAxis = true;
            }
        }
        LeaveCriticalSection(&g_diLock);

        Out o;
        Compute(t, now, dt, o);

        EnterCriticalSection(&g_diLock);
        if (dev && dev == s_fx.dev && s_fx.ready && dev->acquired && focused) {
            ForceTest(now, o);
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
    // DirectInput's convention: a positive force turns the wheel toward the axis' positive end
    // (right). [Calibration] ForceSign=-1 or F4's Reverse force direction for wheels that differ.
    S.forceSign = g_cfg.forceSign ? g_cfg.forceSign : 1;
    S.slip.sign = g_cfg.slipSign;
    S.right.sign = g_cfg.rightSign;
    HANDLE thread = CreateThread(nullptr, 0, FfbThread, nullptr, 0, nullptr);
    if (!thread) { Log("ffb: cannot start effect thread (%lu)", GetLastError()); return; }
    InterlockedExchange(&s_started, 1);
    CloseHandle(thread);
    Log("ffb: engine started at %d Hz", g_cfg.updateHz);
}
