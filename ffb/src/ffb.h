// Force feedback: the engine thread and the API the rest of the mod uses.
//
//   telemetry.cpp    the player car, sampled on the game thread (Telemetry)
//   ffb_model.cpp    what the steering should feel like (ForceParts), pure
//   ffb_filters.h    smoothing, soft limiting, slew limiting, oscillation guard, pure
//   ffb_device.cpp   the wheel: one constant force, device choice, recovery
//   ffb_engine.cpp   the loop tying them together at [FFB] UpdateHz
#pragma once
#include "dinput_wrap.h"

void Ffb_Start();
// Queues a restart of the wheel's force on the FFB thread. False when mod forces are inactive.
bool Ffb_RequestReset();
// F4 menu: a short push to the right. False when mod forces are inactive.
bool Ffb_RequestTest();
// F4 menu: "n: name (axes, buttons)" of the nth FFB device in Windows' order; returns the count.
int Ffb_DeviceName(int index, char* out, int len);
// The gameplay overlay opened or closed: re-check the wheel.
void Ffb_SetMenuOpen(bool open);
// Real (unwrapped) IDirectInput8A, used for the fallback when the game never opens the wheel.
void Ffb_SetDirectInput(IDirectInput8A* real);
// FFB-capable device opened by the game. Called with g_diLock held.
void Ffb_RegisterDevice(DeviceWrap* dev);
// Device is about to be released. Called with g_diLock held.
void Ffb_UnregisterDevice(DeviceWrap* dev);
