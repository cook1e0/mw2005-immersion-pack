// Diagnostic tool: lists DirectInput game controllers and their force feedback support.
//   ffbtool            list devices
//   ffbtool test [n]   on the first (or nth) FFB wheel: short pushes each way, printing how far the
//                      wheel moved. Keep your hands off the wheel. Both ways of giving a direction
//                      are tried (negative magnitude and negative direction), since drivers differ.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IDirectInput8A* g_di;
static GUID g_ffb[16];
static int g_numFfb;

static BOOL CALLBACK EnumCb(LPCDIDEVICEINSTANCEA inst, LPVOID) {
    IDirectInputDevice8A* d = nullptr;
    DIDEVCAPS caps = {sizeof(caps)};
    if (SUCCEEDED(g_di->CreateDevice(inst->guidInstance, &d, nullptr))) {
        d->GetCapabilities(&caps);
        d->Release();
    }
    bool ffb = caps.dwFlags & DIDC_FORCEFEEDBACK;
    printf("  %-40s type=0x%02lX axes=%lu buttons=%lu ffb=%s", inst->tszProductName,
           inst->dwDevType & 0xFF, caps.dwAxes, caps.dwButtons, ffb ? "YES" : "no");
    if (ffb && g_numFfb < 16) {
        printf(" [ffb %d]", g_numFfb);
        g_ffb[g_numFfb++] = inst->guidInstance;
    }
    printf("\n");
    return DIENUM_CONTINUE;
}

static DWORD g_forceAxis = DIJOFS_X;    // the axis with the motor, as an offset into DIJOYSTATE2
static bool g_haveForceAxis;
static DWORD g_forceAxisId;             // the force axis' object id (dwType), for DIEFF_OBJECTIDS
static BOOL CALLBACK ActuatorCb(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID) {
    printf("  force axis: %s (offset %lu, id 0x%08lX)\n", o->tszName, o->dwOfs, o->dwType);
    if (!g_haveForceAxis) { g_forceAxis = o->dwOfs; g_forceAxisId = o->dwType; g_haveForceAxis = true; }
    return DIENUM_CONTINUE;
}

// Position of the force axis.
static LONG ReadX(IDirectInputDevice8A* d) {
    DIJOYSTATE2 s = {};
    d->Poll();
    d->GetDeviceState(sizeof(s), &s);
    return *(LONG*)((char*)&s + g_forceAxis);
}

// Wheel movement over `ms` with the given force (magnitude, direction sign).
static LONG Push(IDirectInputDevice8A* d, IDirectInputEffect* fx, LONG magnitude, LONG direction, int ms) {
    DWORD axes[1] = {g_forceAxis};
    LONG dir[1] = {direction};
    DICONSTANTFORCE cf = {magnitude};
    DIEFFECT e = {sizeof(e), DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, INFINITE, 0, DI_FFNOMINALMAX,
                  DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(cf), &cf, 0};
    LONG before = ReadX(d);
    HRESULT hr = fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION);
    if (FAILED(hr)) printf("    SetParameters failed 0x%08lX\n", hr);
    Sleep(ms);
    LONG after = ReadX(d);
    cf.lMagnitude = 0;
    dir[0] = 1;
    fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION);
    return after - before;
}

int main(int argc, char** argv) {
    // Windows' own DirectInput: next to speed.exe, dinput8.dll is the ASI loader (or the FFB mod),
    // which would load the game's mods into this tool.
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    strcat(sys, "\\dinput8.dll");
    HMODULE dll = LoadLibraryA(sys);
    typedef HRESULT(WINAPI * Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create_t create = dll ? (Create_t)GetProcAddress(dll, "DirectInput8Create") : nullptr;
    if (!create || FAILED(create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&g_di, nullptr))) {
        printf("DirectInput8Create failed\n");
        return 1;
    }
    printf("Game controllers:\n");
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, nullptr, DIEDFL_ATTACHEDONLY);
    if (!g_numFfb) { printf("No force feedback device found.\n"); return 0; }
    // ffbtool axes: every FFB device's main axes for a few seconds, no forces (turn the wheel).
    if (argc >= 2 && !strcmp(argv[1], "axes")) {
        IDirectInputDevice8A* devs[16] = {};
        for (int i = 0; i < g_numFfb; i++) {
            g_di->CreateDevice(g_ffb[i], &devs[i], nullptr);
            devs[i]->SetDataFormat(&c_dfDIJoystick2);
            devs[i]->Acquire();
        }
        for (int n = 0; n < 12; n++) {
            for (int i = 0; i < g_numFfb; i++) {
                DIJOYSTATE2 s = {};
                devs[i]->Poll();
                devs[i]->GetDeviceState(sizeof(s), &s);
                printf("[ffb %d] X %6ld Y %6ld Z %6ld Rx %6ld Ry %6ld Rz %6ld   ", i, s.lX, s.lY, s.lZ, s.lRx, s.lRy, s.lRz);
            }
            printf("\n");
            Sleep(500);
        }
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "test")) return 0;
    int pick = argc > 2 ? atoi(argv[2]) : 0;
    if (pick < 0 || pick >= g_numFfb) { printf("No FFB device %d.\n", pick); return 1; }

    IDirectInputDevice8A* d;
    g_di->CreateDevice(g_ffb[pick], &d, nullptr);
    d->SetDataFormat(&c_dfDIJoystick2);
    // Force feedback needs exclusive access, which needs a window (there is none when run from
    // some terminals): the console's, or a small one of our own.
    // Always a real top-level window of our own: a terminal's console window may be a hidden
    // pseudo-console that DirectInput rejects (0x80070578).
    HWND wnd = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "ffbtool - hands off the wheel", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               100, 100, 360, 80, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    SetForegroundWindow(wnd);
    HRESULT coop = d->SetCooperativeLevel(wnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(coop)) printf("SetCooperativeLevel failed 0x%08lX\n", coop);
    d->EnumObjects(ActuatorCb, nullptr, DIDFT_AXIS | DIDFT_FFACTUATOR);
    DIPROPDWORD ac = {{sizeof(ac), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, DIPROPAUTOCENTER_OFF};
    d->SetProperty(DIPROP_AUTOCENTER, &ac.diph);
    DIPROPRANGE r = {{sizeof(r), sizeof(DIPROPHEADER), g_forceAxis, DIPH_BYOFFSET}, -10000, 10000};
    d->SetProperty(DIPROP_RANGE, &r.diph);
    HRESULT acq = d->Acquire();
    if (FAILED(acq)) { printf("Acquire failed 0x%08lX\n", acq); return 1; }
    d->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
    DWORD state = 0;
    d->GetForceFeedbackState(&state);
    printf("force feedback state 0x%08lX%s%s%s\n", state, state & DIGFFS_ACTUATORSOFF ? " actuators-off" : "",
           state & DIGFFS_POWEROFF ? " power-off" : "", state & DIGFFS_SAFETYSWITCHOFF ? " safety-off" : "");

    // The wheel's position: X (the steering axis) and the force axis, -10000..10000.
    DIPROPRANGE rx = {{sizeof(rx), sizeof(DIPROPHEADER), DIJOFS_X, DIPH_BYOFFSET}, -10000, 10000};
    d->SetProperty(DIPROP_RANGE, &rx.diph);
    auto wheel = [&]() {
        DIJOYSTATE2 s = {};
        d->Poll();
        d->GetDeviceState(sizeof(s), &s);
        return s.lX;
    };
    auto watch = [&](const char* what, int ms) {
        LONG before = wheel();
        Sleep(ms);
        LONG moved = wheel() - before;
        printf("  %-46s wheel moved %+6ld (%s)\n", what, moved, moved > 60 ? "RIGHT" : moved < -60 ? "LEFT" : "still");
        return moved;
    };
    printf("Hands off the wheel for about 15 seconds. Wheel position %ld (-10000 = full left, +10000 = full right).\n", wheel());
    Sleep(1000);

    printf("1. No effects at all:\n");
    watch("nothing, 1 s", 1000);

    // One effect type at a time, then pushes, on the steering axis and on the force axis.
    DWORD axisList[2] = {DIJOFS_X, g_forceAxis};
    const char* axisName[2] = {"X (steering axis)", "force axis"};
    for (int ai = 0; ai < (g_forceAxis == DIJOFS_X ? 1 : 2); ai++) {
        DWORD axes[1] = {axisList[ai]};
        LONG dir[1] = {1};
        printf("%d. Effects on the %s:\n", ai + 2, axisName[ai]);
        DICONSTANTFORCE cf = {0};
        DIEFFECT e = {sizeof(e), DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, INFINITE, 0, DI_FFNOMINALMAX,
                      DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(cf), &cf, 0};
        IDirectInputEffect* fx = nullptr;
        HRESULT hr = d->CreateEffect(GUID_ConstantForce, &e, &fx, nullptr);
        if (FAILED(hr)) { printf("  constant force: CreateEffect failed 0x%08lX\n", hr); continue; }
        hr = fx->Start(1, 0);
        printf("  constant force started 0x%08lX\n", hr);
        watch("constant force at 0, 1 s", 1000);
        struct { const char* what; LONG magnitude, direction; } pushes[] = {
            {"push: magnitude +3000, direction +1", 3000, 1},
            {"push: magnitude -3000, direction +1", -3000, 1},
            {"push: magnitude +3000, direction -1", 3000, -1},
        };
        for (auto& t : pushes) {
            LONG moved = Push(d, fx, t.magnitude, t.direction, 150);
            printf("  %-46s wheel moved %+6ld (%s)\n", t.what, moved, moved > 60 ? "RIGHT" : moved < -60 ? "LEFT" : "still");
            Sleep(700);
        }
        fx->Stop();
        fx->Release();

        DICONDITION cond = {};
        cond.lPositiveCoefficient = cond.lNegativeCoefficient = 3000;
        cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
        DIEFFECT ce = {sizeof(ce), DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, INFINITE, 0, DI_FFNOMINALMAX,
                       DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(cond), &cond, 0};
        const GUID* conds[3] = {&GUID_Friction, &GUID_Damper, &GUID_Spring};
        const char* condName[3] = {"friction 30%, 1 s", "damper 30%, 1 s", "spring 30% (centre 0), 1 s"};
        for (int ci = 0; ci < 3; ci++) {
            IDirectInputEffect* c = nullptr;
            hr = d->CreateEffect(*conds[ci], &ce, &c, nullptr);
            if (FAILED(hr)) { printf("  %s: CreateEffect failed 0x%08lX\n", condName[ci], hr); continue; }
            c->Start(1, 0);
            watch(condName[ci], 1000);
            c->Stop();
            c->Release();
            Sleep(300);
        }
        DIPERIODIC per = {};
        per.dwMagnitude = 500;
        per.dwPeriod = 25000;
        DIEFFECT pe = {sizeof(pe), DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, INFINITE, 0, DI_FFNOMINALMAX,
                       DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(per), &per, 0};
        IDirectInputEffect* sine = nullptr;
        hr = d->CreateEffect(GUID_Sine, &pe, &sine, nullptr);
        if (SUCCEEDED(hr)) {
            sine->Start(1, 0);
            watch("vibration 5% at 40 Hz, 1 s", 1000);
            sine->Stop();
            sine->Release();
        } else {
            printf("  vibration: CreateEffect failed 0x%08lX\n", hr);
        }
    }
    // Other ways of giving a constant force, for drivers that ignore magnitude updates on a
    // running effect: each a fresh effect created with its force, played for 0.3 s.
    printf("4. Constant force, other ways (fresh effect each, 0.3 s):\n");
    struct Variant { const char* what; DWORD flags; DWORD axis; LONG magnitude; LONG direction; bool startFlag; };
    Variant variants[] = {
        {"created with +4000 on X (offsets)", DIEFF_OBJECTOFFSETS, DIJOFS_X, 4000, 1, false},
        {"created with -4000 on X (offsets)", DIEFF_OBJECTOFFSETS, DIJOFS_X, -4000, 1, false},
        {"created +4000 dir -1 on X (offsets)", DIEFF_OBJECTOFFSETS, DIJOFS_X, 4000, -1, false},
        {"created with +4000 on force axis (object id)", DIEFF_OBJECTIDS, g_forceAxisId, 4000, 1, false},
        {"created with -4000 on force axis (object id)", DIEFF_OBJECTIDS, g_forceAxisId, -4000, 1, false},
        {"0, then +4000 with DIEP_START on X", DIEFF_OBJECTOFFSETS, DIJOFS_X, 4000, 1, true},
        {"0, then -4000 with DIEP_START on X", DIEFF_OBJECTOFFSETS, DIJOFS_X, -4000, 1, true},
    };
    for (auto& v : variants) {
        DWORD axes[1] = {v.axis};
        LONG dir[1] = {v.direction};
        DICONSTANTFORCE cf = {v.startFlag ? 0 : v.magnitude};
        DIEFFECT e = {sizeof(e), DIEFF_CARTESIAN | v.flags, INFINITE, 0, DI_FFNOMINALMAX,
                      DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(cf), &cf, 0};
        IDirectInputEffect* fx = nullptr;
        HRESULT hr = d->CreateEffect(GUID_ConstantForce, &e, &fx, nullptr);
        if (FAILED(hr)) { printf("  %-46s CreateEffect failed 0x%08lX\n", v.what, hr); continue; }
        LONG before = wheel();
        if (v.startFlag) {
            fx->Start(1, 0);
            cf.lMagnitude = v.magnitude;
            hr = fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_START);
        } else {
            hr = fx->Start(1, 0);
        }
        Sleep(300);
        LONG moved = wheel() - before;
        fx->Stop();
        fx->Release();
        printf("  %-46s wheel moved %+6ld (%s)  [0x%08lX]\n", v.what, moved, moved > 60 ? "RIGHT" : moved < -60 ? "LEFT" : "still", hr);
        Sleep(800);
    }
    printf("Done. Wheel position %ld.\n", wheel());
    d->Unacquire();
    d->Release();
    g_di->Release();
    return 0;
}
