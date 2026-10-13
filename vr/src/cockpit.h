// Cockpit: driver's seat and the driver model's head.
#pragma once
#include "common.h"
#include "vrmath.h"

void Cockpit_Install();
// Called with the game camera's world->view matrix before the eyes are placed; moves it to the
// driver's seat in the bumper and hood cameras. Returns true if it did.
bool Cockpit_ApplySeat(M4& view);
// After Present: the driver head seen this frame becomes the seat for the next one.
void Cockpit_EndFrame();
// Seat adjustment keys, once per frame.
void Cockpit_Keys();
