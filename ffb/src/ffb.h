// Force feedback engine: turns telemetry into DirectInput effects on the wheel.
#pragma once
#include "dinput_wrap.h"

void Ffb_Start();
// Queues a device/effect rebuild on the FFB thread. False when mod effects are inactive.
bool Ffb_RequestReset();
// The gameplay overlay keeps menu forces available and requests a health check on transitions.
void Ffb_SetMenuOpen(bool open);
// Real (unwrapped) IDirectInput8A, used for the fallback when the game never opens the wheel.
void Ffb_SetDirectInput(IDirectInput8A* real);
// FFB-capable device opened by the game. Called with g_diLock held.
void Ffb_RegisterDevice(DeviceWrap* dev);
// Device is about to be released. Called with g_diLock held.
void Ffb_UnregisterDevice(DeviceWrap* dev);
