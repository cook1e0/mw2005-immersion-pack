// Production wrapper with synthetic game predicates and a fake DirectInput device.
// No controller is opened. The fixed-base test reserves the game's addresses.
#include "../src/dinput_wrap.cpp"
#include <assert.h>
#include <stdio.h>
Config g_cfg;
volatile unsigned char gameAddressPadding[0x600000];
void Log(const char*, ...) {}
void Ffb_UnregisterDevice(DeviceWrap*) {}
void Ffb_RegisterDevice(DeviceWrap*) {}
void Ffb_SetDirectInput(IDirectInput8A*) {}

static bool inWorld = true;
static bool __cdecl InWorld() { return inWorld; }
static int __cdecl Running() { return 3; }
struct State { LONG steering, pedal; BYTE button; BYTE pad[3]; DWORD hat; };
static State physical;
static bool eventAvailable;
static DIDEVICEOBJECTDATA realEvent;
static bool missingPedal;
static HRESULT WINAPI GetState(IDirectInputDevice8A*, DWORD n, void* out) {
    assert(n == sizeof(physical)); memcpy(out, &physical, n); return DI_OK;
}
static HRESULT WINAPI GetProperty(IDirectInputDevice8A*, REFGUID, DIPROPHEADER* p) {
    auto* range = (DIPROPRANGE*)p;
    if (missingPedal && p->dwObj == 4) return DIERR_OBJECTNOTFOUND;
    range->lMin = -10000; range->lMax = 10000; return DI_OK;
}
static HRESULT WINAPI SetFormat(IDirectInputDevice8A*, const DIDATAFORMAT*) { return DI_OK; }
static HRESULT WINAPI GetData(IDirectInputDevice8A*, DWORD stride, DIDEVICEOBJECTDATA* out, DWORD* count, DWORD flags) {
    if (eventAvailable && *count && out) {
        memcpy(out, &realEvent, stride);
        *count = 1;
        if (!(flags & DIGDD_PEEK)) eventAvailable = false;
    } else *count = 0;
    return DI_OK;
}
static void Stub(uintptr_t address, void* fn) {
    DWORD old;
    assert(VirtualProtect((void*)address, 5, PAGE_EXECUTE_READWRITE, &old));
    *(uint8_t*)address = 0xE9;
    *(int32_t*)(address + 1) = (int32_t)((uintptr_t)fn - address - 5);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, 5);
}
int main() {
    static_assert(sizeof(void*) == 4, "test the game layout");
    assert((uintptr_t)gameAddressPadding < 0x468380 &&
           (uintptr_t)gameAddressPadding + sizeof(gameAddressPadding) > 0x91CAF0);
    InitializeCriticalSection(&g_diLock);
    Stub(0x468380, (void*)&InWorld); Stub(0x6E8D20, (void*)&Running);
    *(void**)0x91CAE0 = nullptr;
    void* vt[32] = {};
    vt[5] = (void*)&GetProperty; vt[9] = (void*)&GetState;
    vt[10] = (void*)&GetData; vt[11] = (void*)&SetFormat;
    struct Fake { void** vt; } real = {vt};
    DeviceWrap wrapper((IDirectInputDevice8A*)&real);
    wrapper.ffbDevice = false;
    DIOBJECTDATAFORMAT objects[] = {
        {&GUID_XAxis, 0, DIDFT_ABSAXIS, 0}, {&GUID_YAxis, 4, DIDFT_ABSAXIS, 0},
        {nullptr, 8, DIDFT_PSHBUTTON, 0}, {&GUID_POV, 12, DIDFT_POV, 0}
    };
    DIDATAFORMAT format = {sizeof(format), sizeof(objects[0]), DIDF_ABSAXIS, sizeof(State), 4, objects};
    assert(SUCCEEDED(wrapper.SetDataFormat(&format)));
    physical = {10000, -10000, 0x80, {}, 9000};
    State out = {};
    g_cfg.gameController = 0;
    assert(SUCCEEDED(wrapper.GetDeviceState(sizeof(out), &out)));
    assert(out.steering == 0 && out.pedal == 0 && out.button == 0 && out.hat == 0xFFFFFFFF);
    physical.steering = 0;
    wrapper.GetDeviceState(sizeof(out), &out);
    assert(out.steering == 0);
    g_cfg.gameController = 1;
    wrapper.GetDeviceState(sizeof(out), &out);
    assert(out.pedal == -10000 && out.button == 0x80 && out.hat == 9000);
    inWorld = false;
    wrapper.GetDeviceState(sizeof(out), &out);
    assert(out.pedal == 0 && out.button == 0x80 && out.hat == 9000);
    inWorld = true;
    DIDEVICEOBJECTDATA events[8] = {};
    DWORD count = 8;
    wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 0);
    g_cfg.gameController = 0;
    count = 8; wrapper.GetDeviceData(sizeof(events[0]), events, &count, DIGDD_PEEK);
    assert(count == 4 && events[0].dwData == 0 && events[2].dwData == 0 && events[3].dwData == 0xFFFFFFFF);
    count = 8; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 4);
    count = 8; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 0);
    g_cfg.gameController = 1;
    count = 8; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 4 && (LONG)events[1].dwData == -10000 && events[2].dwData == 0x80);

    // A small caller buffer defers the pedal snapshot. Releasing it before the
    // next read must not be followed by a synthetic full-pedal event.
    inWorld = false;
    count = 8; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 2);
    inWorld = true; physical.pedal = 10000;
    count = 1; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 1 && wrapper.menuReset.pending.size() == 1);
    physical.pedal = 0;
    realEvent.dwOfs = 4; realEvent.dwData = 0; eventAvailable = true;
    count = 1; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 1 && events[0].dwData == 0 && wrapper.menuReset.pending.empty());
    count = 8; wrapper.GetDeviceData(sizeof(events[0]), events, &count, 0);
    assert(count == 0);
    missingPedal = true;
    wrapper.RefreshMenuRanges();
    g_cfg.gameController = 0;
    wrapper.GetDeviceState(sizeof(out), &out);
    assert(out.pedal == 0); // An unavailable optional axis cannot become phantom input.
    puts("device input tests passed");
}
