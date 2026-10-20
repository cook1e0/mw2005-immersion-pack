// Physics sliders for the player's car: driving aids, grip, torque split, gravity.
#pragma once
#include <string>

// Hooks the player's suspension and rigid body simulation. Called from Game_Install.
void Physics_Install();
// Once per frame (game thread), reads tuning tables the game builds at startup.
void Physics_Frame();

// Testing: lifts the player's car [Tires] DropHeight metres straight up at its next physics step
// (vertical and spin speed zeroed, forward speed kept) and lets it fall. Any thread.
void Physics_DropCar();

// Per-car settings (game thread). The player's car changed: its [Car.<name>] section, if it has
// one, replaces the [Physics] slider values in g_cfg until another car is driven.
void Physics_SetCar(const char* vehicleName);
const char* Physics_Car();                  // "" until a car is known
bool Physics_OwnSettings();                 // the current car has its own section
void Physics_SetOwnSettings(bool on);       // off deletes the car's section
std::string Physics_Section();              // where the per-car sliders are saved
bool Physics_IsPerCar(const void* value);   // a g_cfg field that is kept per car
