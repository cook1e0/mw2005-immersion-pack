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
static BOOL CALLBACK ActuatorCb(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID) {
    printf("  force axis: %s (offset %lu)\n", o->tszName, o->dwOfs);
    if (!g_haveForceAxis) { g_forceAxis = o->dwOfs; g_haveForceAxis = true; }
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
    if (FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&g_di, nullptr))) {
        printf("DirectInput8Create failed\n");
        return 1;
    }
    printf("Game controllers:\n");
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, nullptr, DIEDFL_ATTACHEDONLY);
    if (!g_numFfb) { printf("No force feedback device found.\n"); return 0; }
    if (argc < 2 || strcmp(argv[1], "test")) return 0;
    int pick = argc > 2 ? atoi(argv[2]) : 0;
    if (pick < 0 || pick >= g_numFfb) { printf("No FFB device %d.\n", pick); return 1; }

    IDirectInputDevice8A* d;
    g_di->CreateDevice(g_ffb[pick], &d, nullptr);
    d->SetDataFormat(&c_dfDIJoystick2);
    // Force feedback needs exclusive access, which needs a window (there is none when run from
    // some terminals): the console's, or a small one of our own.
    HWND wnd = GetConsoleWindow();
    if (!wnd) wnd = CreateWindowExA(0, "STATIC", "ffbtool", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
                                    GetModuleHandle(nullptr), nullptr);
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

    DWORD axes[1] = {g_forceAxis};
    LONG dir[1] = {1};
    DICONSTANTFORCE cf = {0};
    DIEFFECT e = {sizeof(e), DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, INFINITE, 0, DI_FFNOMINALMAX,
                  DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(cf), &cf, 0};
    IDirectInputEffect* fx;
    HRESULT hr = d->CreateEffect(GUID_ConstantForce, &e, &fx, nullptr);
    if (FAILED(hr)) { printf("CreateEffect failed 0x%08lX\n", hr); return 1; }
    hr = fx->Start(1, 0);
    DWORD status = 0;
    fx->GetEffectStatus(&status);
    printf("constant force effect started 0x%08lX, playing=%d\n", hr, (status & DIEGES_PLAYING) != 0);
    printf("Hands off the wheel. Axis range -10000..10000 (positive = right).\n");
    Sleep(600);

    struct { const char* what; LONG magnitude, direction; } tests[] = {
        {"no force (drift)            ", 0, 1},
        {"magnitude +3000, direction +1", 3000, 1},
        {"magnitude -3000, direction +1", -3000, 1},
        {"magnitude +3000, direction -1", 3000, -1},
    };
    for (auto& t : tests) {
        LONG moved = Push(d, fx, t.magnitude, t.direction, 150);
        printf("%s moved the wheel %+6ld (%s)\n", t.what, moved, moved > 30 ? "right" : moved < -30 ? "left" : "none");
        Sleep(700);
    }
    fx->Stop();
    fx->Release();
    d->Unacquire();
    d->Release();
    g_di->Release();
    return 0;
}
