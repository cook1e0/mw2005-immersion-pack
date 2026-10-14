// DirectInput8 wrappers: give the FFB engine the game's own wheel device.
#pragma once
#include "common.h"
#include "menu_input.h"

// Serialises every call into a wrapped device (game thread vs FFB thread).
extern CRITICAL_SECTION g_diLock;

struct DeviceWrap : public IDirectInputDevice8A {
    IDirectInputDevice8A* real;
    LONG refs = 1;
    char name[MAX_PATH] = {};
    // State shared with the FFB engine (guarded by g_diLock)
    bool  acquired = false;
    bool  acquireWanted = false; // explicit Unacquire suspends retries until Acquire/focus return
    double acquireRetry = 0;
    bool  exclusive = false;
    LONG  acquireGen = 0;       // bumps on each successful fresh Acquire
    DWORD dataSize = 0;         // current data format size
    LONG  axisOfs = -1;         // offset of the steering axis inside the data format
    LONG  forceAxisOfs = -1;    // offset of the force (motor) axis when it differs from axisOfs
    HWND  hwnd = nullptr;
    bool  ffbDevice = true;     // false: wrapped only so its input can be hidden from the game
    std::vector<MenuObject> menuObjects;
    MenuResetQueue menuReset;
    bool relativeAxes = false;
    void RefreshMenuRanges();

    explicit DeviceWrap(IDirectInputDevice8A* r) : real(r) {}
    bool OwnsFfb() const;

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    // IDirectInputDevice8A
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS a) override;
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA a, LPVOID b, DWORD c) override;
    STDMETHOD(GetProperty)(REFGUID a, LPDIPROPHEADER b) override;
    STDMETHOD(SetProperty)(REFGUID a, LPCDIPROPHEADER b) override;
    STDMETHOD(Acquire)() override;
    STDMETHOD(Unacquire)() override;
    STDMETHOD(GetDeviceState)(DWORD a, LPVOID b) override;
    STDMETHOD(GetDeviceData)(DWORD a, LPDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override;
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT a) override;
    STDMETHOD(SetEventNotification)(HANDLE a) override;
    STDMETHOD(SetCooperativeLevel)(HWND a, DWORD b) override;
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA a, DWORD b, DWORD c) override;
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA a) override;
    STDMETHOD(RunControlPanel)(HWND a, DWORD b) override;
    STDMETHOD(Initialize)(HINSTANCE a, DWORD b, REFGUID c) override;
    STDMETHOD(CreateEffect)(REFGUID a, LPCDIEFFECT b, LPDIRECTINPUTEFFECT* c, LPUNKNOWN d) override;
    STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA a, LPVOID b, DWORD c) override;
    STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA a, REFGUID b) override;
    STDMETHOD(GetForceFeedbackState)(LPDWORD a) override;
    STDMETHOD(SendForceFeedbackCommand)(DWORD a) override;
    STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b, DWORD c) override;
    STDMETHOD(Escape)(LPDIEFFESCAPE a) override;
    STDMETHOD(Poll)() override;
    STDMETHOD(SendDeviceData)(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override;
    STDMETHOD(EnumEffectsInFile)(LPCSTR a, LPDIENUMEFFECTSINFILECALLBACK b, LPVOID c, DWORD d) override;
    STDMETHOD(WriteEffectToFile)(LPCSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) override;
    STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override;
    STDMETHOD(SetActionMap)(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override;
    STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA a) override;
};

// Wraps the interface returned by the real DirectInput8Create when it is IDirectInput8A.
HRESULT WrapDirectInput(REFIID riid, void** ppv);

// The game's controller input is hidden (the gameplay mod's [Controls] GameController=0).
bool GameInputHidden();

// GUID of the axis selected by Config::steerAxis.
const GUID& SteerAxisGuid();
