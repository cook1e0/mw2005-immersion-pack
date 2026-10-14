// Force feedback engine: turns telemetry into DirectInput effects on the wheel.
#pragma once
#include "dinput_wrap.h"

void Ffb_Start();
// Queues a device/effect rebuild on the FFB thread. False when mod effects are inactive.
bool Ffb_RequestReset();
// F4 menu: a short push to the right. False when mod effects are inactive.
bool Ffb_RequestTest();
// F4 menu: run the force direction calibration now (hands off the wheel); status 0 not run,
// 1 running, 2 done, 3 failed.
bool Ffb_RequestCalibration();
int Ffb_CalibrationStatus();
// F4 menu: "n: name (axes, buttons)" of the nth FFB device in Windows' order; returns the count.
int Ffb_DeviceName(int index, char* out, int len);
// The gameplay overlay keeps menu forces available and requests a health check on transitions.
void Ffb_SetMenuOpen(bool open);
// Real (unwrapped) IDirectInput8A, used for the fallback when the game never opens the wheel.
void Ffb_SetDirectInput(IDirectInput8A* real);
// FFB-capable device opened by the game. Called with g_diLock held.
void Ffb_RegisterDevice(DeviceWrap* dev);
// Device is about to be released. Called with g_diLock held.
void Ffb_UnregisterDevice(DeviceWrap* dev);
