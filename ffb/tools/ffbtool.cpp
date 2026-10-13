// Diagnostic tool: lists DirectInput game controllers and their force feedback support.
//   ffbtool          list devices
//   ffbtool test     on the first FFB wheel: gentle push right, then left, printing axis movement
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>

static IDirectInput8A* g_di;
static GUID g_ffb;
static bool g_haveFfb;

static BOOL CALLBACK EnumCb(LPCDIDEVICEINSTANCEA inst, LPVOID) {
    IDirectInputDevice8A* d = nullptr;
    DIDEVCAPS caps = {sizeof(caps)};
    if (SUCCEEDED(g_di->CreateDevice(inst->guidInstance, &d, nullptr))) {
        d->GetCapabilities(&caps);
        d->Release();
    }
    bool ffb = caps.dwFlags & DIDC_FORCEFEEDBACK;
    printf("  %-40s type=0x%02lX axes=%lu buttons=%lu ffb=%s\n", inst->tszProductName,
           inst->dwDevType & 0xFF, caps.dwAxes, caps.dwButtons, ffb ? "YES" : "no");
    if (ffb && !g_haveFfb) { g_ffb = inst->guidInstance; g_haveFfb = true; }
    return DIENUM_CONTINUE;
}

static LONG ReadX(IDirectInputDevice8A* d) {
    DIJOYSTATE2 s = {};
    d->Poll();
    d->GetDeviceState(sizeof(s), &s);
    return s.lX;
}

int main(int argc, char** argv) {
    if (FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&g_di, nullptr))) {
        printf("DirectInput8Create failed\n");
        return 1;
    }
    printf("Game controllers:\n");
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, nullptr, DIEDFL_ATTACHEDONLY);
    if (!g_haveFfb) { printf("No force feedback device found.\n"); return 0; }
    if (argc < 2 || strcmp(argv[1], "test")) return 0;

    IDirectInputDevice8A* d;
    g_di->CreateDevice(g_ffb, &d, nullptr);
    d->SetDataFormat(&c_dfDIJoystick2);
    d->SetCooperativeLevel(GetConsoleWindow(), DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    DIPROPDWORD ac = {{sizeof(ac), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, DIPROPAUTOCENTER_OFF};
    d->SetProperty(DIPROP_AUTOCENTER, &ac.diph);
    DIPROPRANGE r = {{sizeof(r), sizeof(DIPROPHEADER), DIJOFS_X, DIPH_BYOFFSET}, -10000, 10000};
    d->SetProperty(DIPROP_RANGE, &r.diph);
    if (FAILED(d->Acquire())) { printf("Acquire failed\n"); return 1; }

    DWORD axes[1] = {DIJOFS_X};
    LONG dir[1] = {1};
    DICONSTANTFORCE cf = {0};
    DIEFFECT e = {sizeof(e), DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, INFINITE, 0, DI_FFNOMINALMAX,
                  DIEB_NOTRIGGER, 0, 1, axes, dir, nullptr, sizeof(cf), &cf, 0};
    IDirectInputEffect* fx;
    if (FAILED(d->CreateEffect(GUID_ConstantForce, &e, &fx, nullptr))) { printf("CreateEffect failed\n"); return 1; }
    fx->Start(1, 0);
    for (int pass = 0; pass < 2; ++pass) {
        LONG m = pass == 0 ? 2500 : -2500;
        LONG before = ReadX(d);
        cf.lMagnitude = m;
        fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
        Sleep(150);
        LONG after = ReadX(d);
        cf.lMagnitude = 0;
        fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
        printf("magnitude %+ld moved the wheel %+ld (%s)\n", m, after - before,
               after > before ? "right" : after < before ? "left" : "none");
        Sleep(400);
    }
    fx->Stop();
    fx->Release();
    d->Unacquire();
    d->Release();
    g_di->Release();
    return 0;
}
