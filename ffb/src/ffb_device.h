// The wheel as an output: one DirectInput constant-force effect carrying the whole torque.
//
// Direct drive practice: the force model computes centring, damping and friction itself, and the
// wheel only plays a single torque. Its own spring / damper / friction effects are not used:
// drivers implement them differently (a Fanatec base was measured pushing away from centre with
// its spring). Measured on a Fanatec base as well: changes to a playing effect are ignored unless
// they restart it, so every update is sent with DIEP_START ([FFB] RestartEffects).
//
// Owns device choice (by Windows' order, as joy.cpl lists them), acquisition, the effect's
// lifecycle and recovery: focus loss and return, a lost or reset device, stopped or unloaded
// effects, failed operations (retried with a delay), hardware power / safety switches (respected),
// and a manual restart. All calls on the FFB thread with g_diLock held.
#pragma once
#include "dinput_wrap.h"

namespace ffb {

// Picks, acquires and keeps the effect running. Returns the device when forces can be sent.
DeviceWrap* Device_Service(double now, bool focused, bool manualRestart);
// Sends a torque, + = turns the wheel right (the device's direction is applied here). The
// effect is updated only when the value changes noticeably or every 50 ms.
void Device_Send(float force, double now);
// The steering axis' position, -1 .. +1. False if unavailable.
bool Device_ReadWheel(DeviceWrap* d, double now, float* pos);
// The gameplay overlay opened or closed: it may release the wheel without the game losing focus,
// so re-arm the devices that worked and check the effect now.
void Device_Recheck();
// When the effect last (re)started: forces fade back in from there.
double Device_StartTime();
// Opens the first FFB device ourselves when the game never did (after a grace period).
void Device_Fallback(double now, bool manual);

}  // namespace ffb
