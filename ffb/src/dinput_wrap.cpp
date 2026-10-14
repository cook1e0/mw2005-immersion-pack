#include "dinput_wrap.h"
#include "ffb.h"
#include <string.h>

CRITICAL_SECTION g_diLock;

struct Lock {
    Lock() { EnterCriticalSection(&g_diLock); }
    ~Lock() { LeaveCriticalSection(&g_diLock); }
};

const GUID& SteerAxisGuid() {
    static const GUID* axes[] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis,
                                 &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider};
    return *axes[g_cfg.steerAxis];
}

// While our engine owns the wheel, the game's own effects go to this no-op object.
struct StubEffect : public IDirectInputEffect {
    LONG refs = 1;
    GUID guid;
    explicit StubEffect(REFGUID g) : guid(g) {}
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputEffect)) {
            AddRef();
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return r;
    }
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(GetEffectGuid)(LPGUID g) override { *g = guid; return DI_OK; }
    STDMETHOD(GetParameters)(LPDIEFFECT, DWORD) override { return DI_OK; }
    STDMETHOD(SetParameters)(LPCDIEFFECT, DWORD) override { return DI_OK; }
    STDMETHOD(Start)(DWORD, DWORD) override { return DI_OK; }
    STDMETHOD(Stop)() override { return DI_OK; }
    STDMETHOD(GetEffectStatus)(LPDWORD s) override { if (s) *s = 0; return DI_OK; }
    STDMETHOD(Download)() override { return DI_OK; }
    STDMETHOD(Unload)() override { return DI_OK; }
    STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DI_OK; }
};

static bool OwnFfb() { return g_cfg.enabled && !g_cfg.nativeFfb; }
bool DeviceWrap::OwnsFfb() const { return ffbDevice && OwnFfb(); }
bool GameInputHidden() { return g_cfg.gameController < 0.5f; }

// Sim::GetState (v1.3, verified in IsSupportedExe): 3 = running, 4 = paused.
// No simulation covers startup/frontend; other states cover loading and transitions.
static bool GameInMenus() {
    // GameFlow 6 = in world. Check FE's pause request too, before Sim has switched to PAUSED.
    if (!((bool(__cdecl*)())0x468380)() || ((int(__cdecl*)())0x6E8D20)() != 3) return true;
    return *(void**)0x91CAE0 && ((bool(__cdecl*)(bool))0x525B20)(true);
}

void DeviceWrap::RefreshMenuRanges() {
    for (auto& o : menuObjects) {
        if (!o.Axis()) continue;
        if (MenuAxisRelative(o.type, relativeAxes)) {
            o.neutral = 0;
            continue;
        }
        DIPROPRANGE r = {};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(r.diph);
        r.diph.dwHow = DIPH_BYOFFSET;
        r.diph.dwObj = o.offset;
        o.neutral = SUCCEEDED(real->GetProperty(DIPROP_RANGE, &r.diph))
            ? MenuAxisCenter(r.lMin, r.lMax) : 0; // Optional absent axes: the game's zero input.
    }
}

static void SetAutoCenterOff(IDirectInputDevice8A* d) {
    DIPROPDWORD p = {};
    p.diph.dwSize = sizeof(p);
    p.diph.dwHeaderSize = sizeof(p.diph);
    p.diph.dwHow = DIPH_DEVICE;
    p.dwData = DIPROPAUTOCENTER_OFF;
    d->SetProperty(DIPROP_AUTOCENTER, &p.diph);
}

// ---------------------------------------------------------------------------
// DeviceWrap
// ---------------------------------------------------------------------------
HRESULT DeviceWrap::QueryInterface(REFIID riid, void** ppv) {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputDevice8A)) {
        AddRef();
        *ppv = this;
        return S_OK;
    }
    return real->QueryInterface(riid, ppv);
}
ULONG DeviceWrap::AddRef() { return InterlockedIncrement(&refs); }
ULONG DeviceWrap::Release() {
    LONG r = InterlockedDecrement(&refs);
    if (!r) {
        Lock l;
        if (ffbDevice) Ffb_UnregisterDevice(this);
        real->Release();
        Log("dinput: released '%s'", name);
        delete this;
    }
    return r;
}
HRESULT DeviceWrap::GetCapabilities(LPDIDEVCAPS a) { Lock l; return real->GetCapabilities(a); }
HRESULT DeviceWrap::EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA a, LPVOID b, DWORD c) { return real->EnumObjects(a, b, c); }
HRESULT DeviceWrap::GetProperty(REFGUID a, LPDIPROPHEADER b) { Lock l; return real->GetProperty(a, b); }
HRESULT DeviceWrap::SetProperty(REFGUID a, LPCDIPROPHEADER b) {
    Lock l;
    if (OwnsFfb() && &a == &DIPROP_AUTOCENTER) {
        // The game must not re-enable the wheel's built-in centring spring.
        DIPROPDWORD p = *(const DIPROPDWORD*)b;
        p.dwData = DIPROPAUTOCENTER_OFF;
        return real->SetProperty(a, &p.diph);
    }
    if (OwnsFfb() && &a == &DIPROP_FFGAIN) return DI_OK;
    HRESULT hr = real->SetProperty(a, b);
    if (SUCCEEDED(hr) && (&a == &DIPROP_RANGE || &a == &DIPROP_AXISMODE)) {
        if (&a == &DIPROP_AXISMODE && b)
            relativeAxes = ((const DIPROPDWORD*)b)->dwData == DIPROPAXISMODE_REL;
        RefreshMenuRanges();
        menuReset.Reset();
    }
    return hr;
}
HRESULT DeviceWrap::Acquire() {
    Lock l;
    acquireWanted = true;
    HRESULT hr = real->Acquire();
    if (SUCCEEDED(hr)) {
        bool fresh = !acquired || hr == DI_OK;
        if (!acquired) Log("dinput: '%s' acquired (exclusive=%d)", name, exclusive);
        acquired = true;
        if (fresh) acquireGen++;
    } else {
        acquired = false;
    }
    return hr;
}
HRESULT DeviceWrap::Unacquire() {
    Lock l;
    if (acquired) Log("dinput: '%s' unacquired", name);
    acquired = false;
    acquireWanted = false;
    return real->Unacquire();
}
HRESULT DeviceWrap::GetDeviceState(DWORD a, LPVOID b) {
    Lock l;
    HRESULT hr = real->GetDeviceState(a, b);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) acquired = false;
    if (FAILED(hr) || !b) return hr;
    if (GameInputHidden()) {
        // The game sets joystick axes to -10000..10000 (v1.3, 0x6D8270);
        // their centre is zero input, regardless of what was held on the first read.
        memset(b, 0, a);
        NeutralizeMenuState(b, a, menuObjects, true);
    } else if (GameInMenus()) {
        bool all = g_cfg.menuController < 0.5f;
        if (all || g_cfg.menuAxes < 0.5f) NeutralizeMenuState(b, a, menuObjects, all);
    }
    return hr;
}
HRESULT DeviceWrap::GetDeviceData(DWORD a, LPDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) {
    Lock l;
    DWORD capacity = c ? *c : 0;
    HRESULT hr = real->GetDeviceData(a, b, c, d);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) acquired = false;
    if (SUCCEEDED(hr) && c) {
        int mode = 0;
        if (GameInputHidden()) {
            mode = 2;
            NeutralizeMenuEvents(a, b, *c, menuObjects, true);
        } else if (GameInMenus()) {
            bool all = g_cfg.menuController < 0.5f;
            mode = all ? 2 : g_cfg.menuAxes < 0.5f ? 1 : 0;
            if (mode) NeutralizeMenuEvents(a, b, *c, menuObjects, all);
        }
        if ((mode != menuReset.mode || !menuReset.pending.empty()) && dataSize) {
            std::vector<uint8_t> state(dataSize);
            if (SUCCEEDED(real->GetDeviceState(dataSize, state.data()))) {
                if (mode != menuReset.mode) menuReset.Change(mode, state.data(), dataSize, menuObjects);
                else menuReset.Refresh(state.data(), dataSize, menuObjects);
                menuReset.DiscardSuperseded(a, b, *c);
            } else if (mode == 0) menuReset.Reset(); // Never replay an unverified driving snapshot.
        }
        *c = menuReset.Append(a, b, *c, capacity, (d & DIGDD_PEEK) != 0);
    }
    return hr;
}
struct ForceAxis { DWORD ofs; char name[MAX_PATH]; bool ok; };
static BOOL CALLBACK FirstForceAxisCb(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID p) {
    ForceAxis* fd = (ForceAxis*)p;
    fd->ofs = o->dwOfs;
    strncpy(fd->name, o->tszName, MAX_PATH - 1);
    fd->ok = true;
    return DIENUM_STOP;
}

HRESULT DeviceWrap::SetDataFormat(LPCDIDATAFORMAT f) {
    Lock l;
    HRESULT hr = real->SetDataFormat(f);
    if (SUCCEEDED(hr) && f) {
        dataSize = f->dwDataSize;
        axisOfs = -1;
        menuObjects.clear();
        menuReset.Reset();
        relativeAxes = (f->dwFlags & DIDF_RELAXIS) != 0;
        const GUID& want = SteerAxisGuid();
        for (DWORD i = 0; i < f->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = f->rgodf[i];
            if (o.dwType & (DIDFT_AXIS | DIDFT_BUTTON | DIDFT_POV))
                menuObjects.push_back({o.dwOfs, o.dwType, (o.dwType & DIDFT_POV) ? 0xFFFFFFFFu : 0u});
            if (axisOfs < 0 && o.pguid && IsEqualGUID(*o.pguid, want) && (o.dwType & DIDFT_AXIS)) {
                axisOfs = (LONG)o.dwOfs;
            }
        }
        // The axis with the motor is the wheel, whatever [FFB] SteerAxis says: Fanatec wheels report
        // it as Ry on one of their two devices. Its offset in this data format, if the format has it.
        if (ffbDevice) {
            ForceAxis found = {};
            real->EnumObjects(FirstForceAxisCb, &found, DIDFT_AXIS | DIDFT_FFACTUATOR);
            if (found.ok && found.ofs + sizeof(LONG) <= f->dwDataSize) {
                bool inFormat = false;
                for (DWORD i = 0; i < f->dwNumObjs && !inFormat; ++i) inFormat = f->rgodf[i].dwOfs == found.ofs;
                if (inFormat && (LONG)found.ofs != axisOfs) {
                    Log("dinput: '%s' force axis '%s' at offset %lu (not the configured SteerAxis, offset %ld): using it",
                        name, found.name, found.ofs, axisOfs);
                    axisOfs = (LONG)found.ofs;
                }
            }
        }
        RefreshMenuRanges();
        Log("dinput: '%s' data format size=%lu objs=%lu steerAxisOfs=%ld", name, f->dwDataSize, f->dwNumObjs, axisOfs);
        if (OwnsFfb()) SetAutoCenterOff(real);
    }
    return hr;
}
HRESULT DeviceWrap::SetEventNotification(HANDLE a) { Lock l; return real->SetEventNotification(a); }
HRESULT DeviceWrap::SetCooperativeLevel(HWND w, DWORD flags) {
    Lock l;
    hwnd = w;
    if (!OwnsFfb()) {
        HRESULT hr = real->SetCooperativeLevel(w, flags);
        exclusive = SUCCEEDED(hr) && (flags & DISCL_EXCLUSIVE);
        return hr;
    }
    // Force feedback requires exclusive access.
    DWORD want = (flags & ~DISCL_NONEXCLUSIVE) | DISCL_EXCLUSIVE;
    HRESULT hr = real->SetCooperativeLevel(w, want);
    if (FAILED(hr)) {
        Log("dinput: '%s' exclusive cooperative level refused (0x%08lX), using game flags 0x%lX", name, hr, flags);
        hr = real->SetCooperativeLevel(w, flags);
        exclusive = SUCCEEDED(hr) && (flags & DISCL_EXCLUSIVE);
    } else {
        exclusive = true;
    }
    Log("dinput: '%s' SetCooperativeLevel(hwnd=%p, game=0x%lX) -> exclusive=%d", name, w, flags, exclusive);
    SetAutoCenterOff(real);
    return hr;
}
HRESULT DeviceWrap::GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA a, DWORD b, DWORD c) { return real->GetObjectInfo(a, b, c); }
HRESULT DeviceWrap::GetDeviceInfo(LPDIDEVICEINSTANCEA a) { return real->GetDeviceInfo(a); }
HRESULT DeviceWrap::RunControlPanel(HWND a, DWORD b) { return real->RunControlPanel(a, b); }
HRESULT DeviceWrap::Initialize(HINSTANCE a, DWORD b, REFGUID c) { return real->Initialize(a, b, c); }
HRESULT DeviceWrap::CreateEffect(REFGUID a, LPCDIEFFECT b, LPDIRECTINPUTEFFECT* c, LPUNKNOWN d) {
    if (OwnsFfb()) {
        *c = new StubEffect(a);
        return DI_OK;
    }
    Lock l;
    return real->CreateEffect(a, b, c, d);
}
HRESULT DeviceWrap::EnumEffects(LPDIENUMEFFECTSCALLBACKA a, LPVOID b, DWORD c) { return real->EnumEffects(a, b, c); }
HRESULT DeviceWrap::GetEffectInfo(LPDIEFFECTINFOA a, REFGUID b) { return real->GetEffectInfo(a, b); }
HRESULT DeviceWrap::GetForceFeedbackState(LPDWORD a) { Lock l; return real->GetForceFeedbackState(a); }
HRESULT DeviceWrap::SendForceFeedbackCommand(DWORD a) {
    if (OwnsFfb()) return DI_OK;  // a game RESET/STOPALL would kill our effects
    Lock l;
    return real->SendForceFeedbackCommand(a);
}
HRESULT DeviceWrap::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b, DWORD c) {
    if (OwnsFfb()) return DI_OK;
    return real->EnumCreatedEffectObjects(a, b, c);
}
HRESULT DeviceWrap::Escape(LPDIEFFESCAPE a) { Lock l; return real->Escape(a); }
HRESULT DeviceWrap::Poll() {
    Lock l;
    HRESULT hr = real->Poll();
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) acquired = false;
    return hr;
}
HRESULT DeviceWrap::SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) { Lock l; return real->SendDeviceData(a, b, c, d); }
HRESULT DeviceWrap::EnumEffectsInFile(LPCSTR a, LPDIENUMEFFECTSINFILECALLBACK b, LPVOID c, DWORD d) { return real->EnumEffectsInFile(a, b, c, d); }
HRESULT DeviceWrap::WriteEffectToFile(LPCSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) { return real->WriteEffectToFile(a, b, c, d); }
HRESULT DeviceWrap::BuildActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) { return real->BuildActionMap(a, b, c); }
HRESULT DeviceWrap::SetActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) { Lock l; return real->SetActionMap(a, b, c); }
HRESULT DeviceWrap::GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA a) { return real->GetImageInfo(a); }

// ---------------------------------------------------------------------------
// IDirectInput8A wrapper
// ---------------------------------------------------------------------------
struct DInputWrap : public IDirectInput8A {
    IDirectInput8A* real;
    LONG refs = 1;
    explicit DInputWrap(IDirectInput8A* r) : real(r) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInput8A)) {
            AddRef();
            *ppv = this;
            return S_OK;
        }
        return real->QueryInterface(riid, ppv);
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&refs);
        if (!r) {
            real->Release();
            delete this;
        }
        return r;
    }
    STDMETHOD(CreateDevice)(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override {
        HRESULT hr = real->CreateDevice(guid, out, outer);
        if (FAILED(hr) || !*out) return hr;
        IDirectInputDevice8A* dev = *out;
        DIDEVICEINSTANCEA info = {};
        info.dwSize = sizeof(info);
        dev->GetDeviceInfo(&info);
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        dev->GetCapabilities(&caps);
        bool ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
        Log("dinput: CreateDevice '%s' type=0x%lX axes=%lu ffb=%d", info.tszProductName, info.dwDevType, caps.dwAxes, ffb);
        // Controllers other than force feedback wheels are wrapped too (when enabled), so their
        // input can be hidden from the game; keyboard and mouse never are.
        BYTE type = GET_DIDEVICE_TYPE(info.dwDevType);
        bool controller = type != DI8DEVTYPE_KEYBOARD && type != DI8DEVTYPE_MOUSE;
        if (!g_cfg.enabled || !controller) return hr;
        DeviceWrap* w = new DeviceWrap(dev);
        strncpy(w->name, info.tszProductName, sizeof(w->name) - 1);
        w->ffbDevice = ffb;
        if (ffb) {
            Lock l;
            Ffb_RegisterDevice(w);
        }
        *out = w;
        return hr;
    }
    STDMETHOD(EnumDevices)(DWORD a, LPDIENUMDEVICESCALLBACKA b, LPVOID c, DWORD d) override { return real->EnumDevices(a, b, c, d); }
    STDMETHOD(GetDeviceStatus)(REFGUID a) override { return real->GetDeviceStatus(a); }
    STDMETHOD(RunControlPanel)(HWND a, DWORD b) override { return real->RunControlPanel(a, b); }
    STDMETHOD(Initialize)(HINSTANCE a, DWORD b) override { return real->Initialize(a, b); }
    STDMETHOD(FindDevice)(REFGUID a, LPCSTR b, LPGUID c) override { return real->FindDevice(a, b, c); }
    STDMETHOD(EnumDevicesBySemantics)(LPCSTR a, LPDIACTIONFORMATA b, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID d, DWORD e) override {
        return real->EnumDevicesBySemantics(a, b, c, d, e);
    }
    STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK a, LPDICONFIGUREDEVICESPARAMSA b, DWORD c, LPVOID d) override {
        return real->ConfigureDevices(a, b, c, d);
    }
};

HRESULT WrapDirectInput(REFIID riid, void** ppv) {
    if (!IsEqualIID(riid, IID_IDirectInput8A)) {
        Log("dinput: DirectInput8Create for a non-ANSI interface, not wrapped");
        return S_OK;
    }
    IDirectInput8A* real = (IDirectInput8A*)*ppv;
    real->AddRef();
    Ffb_SetDirectInput(real);
    *ppv = new DInputWrap(real);
    Log("dinput: IDirectInput8A wrapped");
    return S_OK;
}
