#include "ffb_device.h"
#include "ffb.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ffb {
namespace {

// Devices the game opened (or we did), in the order they were created.
DeviceWrap* s_devs[8];
int s_numDevs;
IDirectInput8A* s_di;
double s_diTime;

struct Output {
    DeviceWrap* dev = nullptr;
    IDirectInputEffect* fx = nullptr;
    LONG gen = -1;                  // the device's acquireGen the effect was started for
    LONG axis = -1;                 // the axis offset it was created on
    bool ready = false;             // playing and accepting updates
    bool rebuild = false;           // recreate before the next start
    bool inhibited = false;         // hardware power / safety switch off
    bool stateIgnored = false;      // the device's state reports are nonsense: not used
    double startTime = 0, healthTime = 0, retryTime = 0;
    LONG sent = 0;                  // last magnitude sent
    LONG sentDir = 0;               // last direction sent (0 = unknown)
    double sentTime = 0;
    LONG rangeMin = 0, rangeMax = 0;
    double rangeTime = -10;
} s_out;

bool s_focusKnown, s_focused;

// What the device layer is doing, for F4 and the log (logged when it changes).
CRITICAL_SECTION s_statusLock;
bool s_statusInit;
char s_status[200];
void SetStatus(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!s_statusInit) { InitializeCriticalSection(&s_statusLock); s_statusInit = true; }
    EnterCriticalSection(&s_statusLock);
    bool changed = strcmp(buf, s_status) != 0;
    if (changed) strcpy(s_status, buf);
    LeaveCriticalSection(&s_statusLock);
    if (changed) Log("ffb: status: %s", buf);
}
DeviceWrap* s_ownDev;               // the fallback device, if we opened one
double s_fallbackNext;

// Device choice ([FFB] Device, live from F4): the nth FFB device in Windows' order.
GUID s_wantGuid;
int s_wantState;                    // 0 = look up, 1 = known, -1 = any
int s_wantDevice = -2;

bool Lost(HRESULT hr) {
    return hr == (HRESULT)DIERR_NOTEXCLUSIVEACQUIRED || hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED ||
           hr == (HRESULT)DIERR_UNPLUGGED || hr == DIERR_OTHERAPPHASPRIO;
}

// Every failed effect operation stops output until the service loop recovers it.
bool Check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr) && hr != DI_DOWNLOADSKIPPED) return true;
    s_out.ready = false;
    if (Lost(hr) || hr == DI_DOWNLOADSKIPPED) {
        if (s_out.dev) s_out.dev->acquired = false;
    } else {
        s_out.rebuild = true;
    }
    static HRESULT lastHr;
    static double logged = -1e9;
    if (hr != lastHr || Now() - logged > 5.0) {
        Log("ffb: %s failed 0x%08lX; recovering", what, hr);
        lastHr = hr;
        logged = Now();
    }
    return false;
}

void ReleaseEffect() {
    if (s_out.fx) {
        s_out.fx->Stop();
        s_out.fx->Release();
    }
    DeviceWrap* dev = s_out.dev;
    s_out = Output();
    s_out.dev = dev;
}

DWORD Axis(DeviceWrap* d) { return d->axisOfs >= 0 ? (DWORD)d->axisOfs : DIJOFS_X; }

// Constant force with the given magnitude and direction (+1 / -1).
DIEFFECT ConstantParams(DWORD* axes, LONG* dirs, DICONSTANTFORCE* cf) {
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.cAxes = 1;
    e.rgdwAxes = axes;
    e.rglDirection = dirs;
    e.cbTypeSpecificParams = sizeof(*cf);
    e.lpvTypeSpecificParams = cf;
    return e;
}

// Autocentre off, full gain, actuators on.
bool PrepareWheel(DeviceWrap* d) {
    DIPROPDWORD p = {};
    p.diph.dwSize = sizeof(p);
    p.diph.dwHeaderSize = sizeof(p.diph);
    p.diph.dwHow = DIPH_DEVICE;
    p.dwData = DIPROPAUTOCENTER_OFF;
    d->real->SetProperty(DIPROP_AUTOCENTER, &p.diph);
    p.dwData = DI_FFNOMINALMAX;
    d->real->SetProperty(DIPROP_FFGAIN, &p.diph);
    HRESULT hr = d->real->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
    return hr == DIERR_UNSUPPORTED || Check(hr, "enabling the actuators");
}

// Creates (if needed) and starts the effect at zero force.
bool StartEffect(DeviceWrap* d, double now) {
    if (!PrepareWheel(d)) return false;
    if (s_out.rebuild || !s_out.fx || s_out.axis != d->axisOfs) {
        ReleaseEffect();
        DWORD axes[1] = {Axis(d)};
        LONG dirs[1] = {1};
        DICONSTANTFORCE cf = {0};
        DIEFFECT e = ConstantParams(axes, dirs, &cf);
        HRESULT hr = d->real->CreateEffect(GUID_ConstantForce, &e, &s_out.fx, nullptr);
        if (FAILED(hr)) {
            s_out.fx = nullptr;
            Check(hr, "creating the constant force");
            return false;
        }
        s_out.axis = d->axisOfs;
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        d->real->GetCapabilities(&caps);
        Log("ffb: constant force on '%s' (%lu axes, %lu buttons), axis offset %ld, %s updates", d->name, caps.dwAxes,
            caps.dwButtons, d->axisOfs, g_cfg.restartEffects ? "restarting" : "plain");
    }
    if (!Check(s_out.fx->Start(1, 0), "starting the constant force")) return false;
    s_out.ready = true;
    s_out.rebuild = false;
    s_out.gen = d->acquireGen;
    s_out.sent = 0;
    s_out.sentDir = 0;
    s_out.sentTime = 0;
    s_out.startTime = now;
    return true;
}

void UpdateFocus(bool focused) {
    if (s_focusKnown && focused == s_focused) return;
    s_focusKnown = true;
    s_focused = focused;
    if (!focused) {
        // Let go of the wheel while the game is in the background.
        if (s_out.fx) s_out.fx->Stop();
        s_out.ready = false;
        return;
    }
    // Back in front: the game's own focus handling can leave the wheel unacquired. Re-arm every
    // handle that worked before, including ones released during Alt-Tab.
    for (int i = 0; i < s_numDevs; ++i) {
        DeviceWrap* d = s_devs[i];
        if (!d->exclusive || d->acquireGen == 0) continue;
        d->acquireWanted = true;
        d->acquired = false;
        d->acquireRetry = 0;
    }
    s_out.healthTime = s_out.retryTime = 0;
}

// Twice a second: respect hardware switches, recover lost or stopped effects.
void CheckHealth(double now) {
    if (!s_out.dev || now < s_out.healthTime) return;
    s_out.healthTime = now + 0.5;
    DWORD state = 0;
    HRESULT hr = s_out.dev->real->GetForceFeedbackState(&state);
    if (Lost(hr)) { Check(hr, "reading the wheel state"); return; }
    // Some drivers report nonsense here: a Fanatec base answered with different random values each
    // time (0x005FF6AD, 0x011FF66D, ...), "power off" and "safety switch off" among them, which
    // stopped every force. Only a report made of known flags, without contradictions, counts.
    const DWORD known = DIGFFS_EMPTY | DIGFFS_STOPPED | DIGFFS_PAUSED | DIGFFS_ACTUATORSON | DIGFFS_ACTUATORSOFF |
                        DIGFFS_POWERON | DIGFFS_POWEROFF | DIGFFS_SAFETYSWITCHON | DIGFFS_SAFETYSWITCHOFF |
                        DIGFFS_USERFFSWITCHON | DIGFFS_USERFFSWITCHOFF | DIGFFS_DEVICELOST;
    auto both = [&](DWORD a, DWORD b) { return (state & a) && (state & b); };
    bool plausible = !(state & ~known) && !both(DIGFFS_ACTUATORSON, DIGFFS_ACTUATORSOFF) &&
                     !both(DIGFFS_POWERON, DIGFFS_POWEROFF) && !both(DIGFFS_SAFETYSWITCHON, DIGFFS_SAFETYSWITCHOFF) &&
                     !both(DIGFFS_USERFFSWITCHON, DIGFFS_USERFFSWITCHOFF);
    if (SUCCEEDED(hr) && !plausible && !s_out.stateIgnored) {
        s_out.stateIgnored = true;
        Log("ffb: '%s' reports an impossible force feedback state 0x%08lX; ignoring its state reports", s_out.dev->name, state);
    }
    if (SUCCEEDED(hr) && plausible) {
        s_out.inhibited = (state & (DIGFFS_POWEROFF | DIGFFS_SAFETYSWITCHOFF | DIGFFS_USERFFSWITCHOFF)) != 0;
        if (s_out.inhibited) { s_out.ready = false; return; }
        if (state & DIGFFS_DEVICELOST) {
            s_out.dev->real->Unacquire();
            s_out.dev->acquired = false;
            s_out.ready = false;
            s_out.rebuild = true;
            return;
        }
        if (state & DIGFFS_PAUSED) {
            if (!Check(s_out.dev->real->SendForceFeedbackCommand(DISFFC_CONTINUE), "resuming the wheel")) return;
            s_out.ready = false;
        }
        if (state & (DIGFFS_ACTUATORSOFF | DIGFFS_EMPTY)) s_out.ready = false;
    }
    if (!s_out.fx) return;
    DWORD status = 0;
    hr = s_out.fx->GetEffectStatus(&status);
    if (hr == DIERR_UNSUPPORTED) return;
    if (SUCCEEDED(hr) && (status & ~(DWORD)(DIEGES_PLAYING | DIEGES_EMULATED))) return;   // nonsense, as above
    if (hr == (HRESULT)DIERR_NOTDOWNLOADED || (SUCCEEDED(hr) && !(status & DIEGES_PLAYING))) s_out.ready = false;
    else if (FAILED(hr)) Check(hr, "reading the effect status");
}

struct FfbEnum { int index, want; GUID guid; bool found; };
BOOL CALLBACK NthFfbDeviceCb(LPCDIDEVICEINSTANCEA inst, LPVOID p) {
    FfbEnum* e = (FfbEnum*)p;
    if (e->index++ != e->want) return DIENUM_CONTINUE;
    e->guid = inst->guidInstance;
    e->found = true;
    Log("ffb: using FFB device %d in Windows' order: '%s'", e->want, inst->tszProductName);
    return DIENUM_STOP;
}

bool IsWanted(DeviceWrap* d) {
    if (s_wantState != 1) return false;
    DIDEVICEINSTANCEA info = {};
    info.dwSize = sizeof(info);
    return SUCCEEDED(d->real->GetDeviceInfo(&info)) && info.guidInstance == s_wantGuid;
}

DeviceWrap* PickDevice() {
    int device = (int)lrintf(g_cfg.device);
    if (device != s_wantDevice) s_wantState = 0;
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
    // The chosen device isn't one the game opened (or its id doesn't match): use what the game has.
    static bool logged;
    if (s_wantState == 1 && !logged && s_numDevs) {
        logged = true;
        Log("ffb: FFB device %d is not among the devices the game opened; using the game's", device);
    }
    if (s_out.dev && s_out.dev->acquired && s_out.dev->exclusive) return s_out.dev;
    for (int i = 0; i < s_numDevs; ++i)
        if (s_devs[i]->acquired && s_devs[i]->exclusive) return s_devs[i];
    return nullptr;
}

// --- fallback: the game never opened a force feedback device ---------------------------------
struct WindowSearch { DWORD pid; HWND found; };
BOOL CALLBACK FindWindowCb(HWND w, LPARAM p) {
    WindowSearch* c = (WindowSearch*)p;
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid == c->pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { c->found = w; return FALSE; }
    return TRUE;
}
struct FoundDevice { GUID guid; char name[MAX_PATH]; bool ok; };
BOOL CALLBACK FirstFfbDeviceCb(LPCDIDEVICEINSTANCEA inst, LPVOID p) {
    FoundDevice* f = (FoundDevice*)p;
    f->guid = inst->guidInstance;
    strncpy(f->name, inst->tszProductName, MAX_PATH - 1);
    f->ok = true;
    return DIENUM_STOP;
}

// For the F4 menu: "n: name (axes, buttons)" of the nth FFB device in Windows' order.
struct FfbNames { int index, want; char* out; int len; };
BOOL CALLBACK NameFfbDeviceCb(LPCDIDEVICEINSTANCEA inst, LPVOID p) {
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

}  // namespace

DeviceWrap* Device_Service(double now, bool focused, bool manualRestart) {
    UpdateFocus(focused);
    if (!focused) {
        SetStatus("Paused: the game window is not in front (click it, or Alt-Tab to it)");
        return nullptr;
    }
    if (manualRestart) {
        DeviceWrap* previous = s_out.dev;
        ReleaseEffect();
        s_out.dev = nullptr;
        for (int i = 0; i < s_numDevs; ++i) {
            DeviceWrap* d = s_devs[i];
            if (!d->exclusive || (d != previous && d->acquireGen == 0 && !d->acquireWanted)) continue;
            d->real->Unacquire();
            d->acquired = false;
            d->acquireWanted = true;
            d->acquireRetry = 0;
        }
        s_fallbackNext = 0;
        Log("ffb: manual restart");
    }
    for (int i = 0; i < s_numDevs; ++i) {
        DeviceWrap* d = s_devs[i];
        if (!d->exclusive || !d->acquireWanted || d->acquired || now < d->acquireRetry) continue;
        d->acquireRetry = now + 0.5;
        d->Acquire();
    }
    DeviceWrap* d = PickDevice();
    if (!d) {
        if (!s_numDevs) SetStatus("Waiting: the game has not opened a force feedback device");
        else SetStatus("Waiting: '%s' is not acquired yet", s_devs[0]->name);
        return nullptr;
    }
    if (d != s_out.dev) {
        ReleaseEffect();
        s_out.dev = d;
        s_out.rebuild = true;
        s_out.retryTime = 0;
    }
    CheckHealth(now);
    if (!d->acquired) { SetStatus("Waiting: '%s' is not acquired", d->name); return nullptr; }
    if (s_out.inhibited) { SetStatus("Off: '%s' reports its force feedback switched off", d->name); return nullptr; }
    bool needStart = !s_out.ready || s_out.gen != d->acquireGen || s_out.axis != d->axisOfs || !s_out.fx;
    if (needStart && now >= s_out.retryTime) {
        bool ok = StartEffect(d, now);
        s_out.healthTime = now + 0.5;
        s_out.retryTime = now + (ok ? 0.5 : 2.0);
        if (ok) Log("ffb: wheel force running on '%s'", d->name);
        else SetStatus("Retrying: '%s' would not start the force (see nfsmw_ffb.log)", d->name);
    }
    if (d->acquired && s_out.ready) {
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        static DeviceWrap* capsDev;
        static DWORD axes, buttons;
        if (capsDev != d && SUCCEEDED(d->real->GetCapabilities(&caps))) { capsDev = d; axes = caps.dwAxes; buttons = caps.dwButtons; }
        SetStatus("Running on '%s' (%lu axes, %lu buttons)", d->name, axes, buttons);
        return d;
    }
    return nullptr;
}

void Device_Send(float force, double now) {
    if (!s_out.fx || !s_out.ready) return;
    float sign = (float)(g_cfg.forceSign < 0 ? -1 : 1) * (g_cfg.reverse > 0.5f ? -1.0f : 1.0f);
    LONG m = (LONG)lrintf(fmaxf(-1.0f, fminf(1.0f, force * sign)) * DI_FFNOMINALMAX);
    if (labs(m - s_out.sent) < 10 && now - s_out.sentTime < 0.05) return;
    // The side as a signed magnitude ([FFB] ForceDirection=0) or as the effect's direction (=1).
    LONG dir = g_cfg.forceDirection && m < 0 ? -1 : 1;
    DWORD axes[1] = {Axis(s_out.dev)};
    LONG dirs[1] = {dir};
    DICONSTANTFORCE cf = {g_cfg.forceDirection ? labs(m) : m};
    DIEFFECT e = ConstantParams(axes, dirs, &cf);
    DWORD flags = DIEP_TYPESPECIFICPARAMS;
    if (dir != s_out.sentDir) flags |= DIEP_DIRECTION;
    if (g_cfg.restartEffects) flags |= DIEP_START;
    if (Check(s_out.fx->SetParameters(&e, flags), "updating the force")) {
        s_out.sent = m;
        s_out.sentDir = dir;
        s_out.sentTime = now;
    }
}

bool Device_ReadWheel(DeviceWrap* d, double now, float* pos) {
    if (!d || !d->acquired || d->axisOfs < 0 || d->dataSize < sizeof(LONG) || d->dataSize > 1024 ||
        (DWORD)d->axisOfs > d->dataSize - sizeof(LONG))
        return false;
    if (now - s_out.rangeTime > 1.0) {
        DIPROPRANGE r = {};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(r.diph);
        r.diph.dwHow = DIPH_BYOFFSET;
        r.diph.dwObj = (DWORD)d->axisOfs;
        if (SUCCEEDED(d->real->GetProperty(DIPROP_RANGE, &r.diph))) {
            s_out.rangeMin = r.lMin;
            s_out.rangeMax = r.lMax;
        }
        s_out.rangeTime = now;
    }
    if (s_out.rangeMax <= s_out.rangeMin) return false;
    alignas(8) uint8_t buf[1024];
    HRESULT hr = d->real->Poll();
    if (Lost(hr)) { Check(hr, "polling the wheel"); return false; }
    hr = d->real->GetDeviceState(d->dataSize, buf);
    if (FAILED(hr)) { if (Lost(hr)) Check(hr, "reading the wheel"); return false; }
    float mid = 0.5f * ((float)s_out.rangeMin + (float)s_out.rangeMax);
    float half = 0.5f * ((float)s_out.rangeMax - (float)s_out.rangeMin);
    *pos = fmaxf(-1.0f, fminf(1.0f, ((float)*(LONG*)(buf + d->axisOfs) - mid) / half));
    return true;
}

double Device_StartTime() { return s_out.startTime; }

void Device_Recheck() {
    for (int i = 0; i < s_numDevs; ++i) {
        DeviceWrap* d = s_devs[i];
        if (d->exclusive && d->acquireGen > 0) d->acquireWanted = true;
    }
    s_out.healthTime = s_out.retryTime = 0;
}

void Device_Fallback(double now, bool manual) {
    if (s_numDevs || !s_di || now < s_fallbackNext || (!manual && now - s_diTime < 10.0)) return;
    s_fallbackNext = now + 3.0;
    WindowSearch ctx = {GetCurrentProcessId(), nullptr};
    EnumWindows(FindWindowCb, (LPARAM)&ctx);
    if (!ctx.found) return;
    FoundDevice found = {};
    s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, FirstFfbDeviceCb, &found, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
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
    if (FAILED(w->SetDataFormat(&df)) || FAILED(w->SetCooperativeLevel(ctx.found, DISCL_EXCLUSIVE | DISCL_FOREGROUND))) {
        real->Release();
        delete w;
        Log("ffb: fallback device setup failed; will retry");
        return;
    }
    Ffb_RegisterDevice(w);
    w->Acquire();
    s_ownDev = w;
    Log("ffb: the game did not open a force feedback device; opened '%s' ourselves", found.name);
}

}  // namespace ffb

// --- registry, called by the DirectInput wrapper with g_diLock held ----------------------------
void Ffb_SetDirectInput(IDirectInput8A* real) {
    EnterCriticalSection(&g_diLock);
    if (ffb::s_di) ffb::s_di->Release();
    ffb::s_di = real;
    ffb::s_diTime = Now();
    LeaveCriticalSection(&g_diLock);
}

void Ffb_RegisterDevice(DeviceWrap* dev) {
    if (ffb::s_numDevs < 8) ffb::s_devs[ffb::s_numDevs++] = dev;
    Log("ffb: registered FFB device '%s'", dev->name);
}

void Ffb_UnregisterDevice(DeviceWrap* dev) {
    for (int i = 0; i < ffb::s_numDevs; ++i) {
        if (ffb::s_devs[i] == dev) {
            ffb::s_devs[i] = ffb::s_devs[--ffb::s_numDevs];
            break;
        }
    }
    if (ffb::s_out.dev == dev) {
        ffb::ReleaseEffect();
        ffb::s_out.dev = nullptr;
        Log("ffb: force released with device '%s'", dev->name);
    }
}

int Ffb_Status(char* out, int len) {
    if (!out || len <= 0) return 0;
    if (!ffb::s_statusInit) { snprintf(out, len, "Starting..."); return 0; }
    EnterCriticalSection(&ffb::s_statusLock);
    snprintf(out, len, "%s", ffb::s_status[0] ? ffb::s_status : "Starting...");
    LeaveCriticalSection(&ffb::s_statusLock);
    return (int)strlen(out);
}

int Ffb_DeviceName(int index, char* out, int len) {
    EnterCriticalSection(&g_diLock);
    ffb::FfbNames e = {0, index, out, len};
    if (out && len > 0) out[0] = 0;
    if (ffb::s_di) ffb::s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, ffb::NameFfbDeviceCb, &e, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    LeaveCriticalSection(&g_diLock);
    return e.index;
}
