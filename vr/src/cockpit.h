// Cockpit: driver's seat and the driver model's head.
#pragma once
#include "common.h"
#include "vrmath.h"

void Cockpit_Install();
// Called with the game camera's world->view matrix before the eyes are placed; moves it to the
// driver's seat in the bumper and hood cameras. Returns true if it did.
bool Cockpit_ApplySeat(M4& view);
// After Present (including flat-screen frames): refresh the observed car and clear frame state.
void Cockpit_EndFrame();
// Seat adjustment keys, once per frame.
void Cockpit_Keys();
// The physical wheel's angle in degrees (+ = right), set by the gameplay mod through
// NfsmwVr_Setting("SteeringWheelAngle"); the player car's steering wheel is drawn turned by it.
extern float g_steeringWheelAngle;
