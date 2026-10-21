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
// The player's car's speed (m/s) at its last physics step.
float Physics_PlayerSpeed();
// Traction control: the share of the throttle to send now (1 = all of it; 1 with TC off).
float Physics_TcThrottle();
// ABS / traction control working right now (for the HUD icons). Game thread.
void Physics_Aids(bool* absActive, bool* tcActive);

// Per-car settings (game thread). The player's car changed: its [Car.<name>] section, if it has
// one, replaces the [Physics] slider values in g_cfg until another car is driven.
void Physics_SetCar(const char* vehicleName);
const char* Physics_Car();                  // "" until a car is known
const char* Physics_CarPreset();              // the preset the current car takes its values from, "" = none
bool Physics_OwnSettings();                 // the current car has its own section
void Physics_SetOwnSettings(bool on);       // off deletes the car's section
std::string Physics_Section();              // where the per-car sliders are saved
bool Physics_IsPerCar(const void* value);   // a g_cfg field that is kept per car

// Presets (game thread): the per-car values saved under a name, in nfsmw_physics_presets.ini.
int Physics_PresetCount();
const char* Physics_PresetName(int i);
int Physics_SavePreset(int i);              // the current values into preset i (< 0: a new one); its index
bool Physics_LoadPreset(int i);             // preset i into the current car's own settings (made if needed)
void Physics_DeletePreset(int i);
// Renames preset i (spaces trimmed); nullptr if done, else why not (empty, [ ] = ;, taken).
const char* Physics_RenamePreset(int i, const std::string& name);
