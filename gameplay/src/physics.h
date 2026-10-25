// Physics sliders for the player's car: driving aids, grip, torque split, gravity.
#pragma once
#include <string>
#include <vector>

struct IniWrite;
struct CarCharacter;

// Hooks the player's suspension and rigid body simulation. Called from Game_Install.
void Physics_Install();
// Once per frame (game thread), reads tuning tables the game builds at startup.
void Physics_Frame();

// Testing: lifts the player's car [Tires] DropHeight metres straight up at its next physics step
// (vertical and spin speed zeroed, forward speed kept) and lets it fall. Any thread.
void Physics_DropCar();
// The player's car's speed (m/s) at its last physics step.
float Physics_PlayerSpeed();
// The throttle the game's chassis took at the player's last physics step (0..1).
float Physics_GasInput();
// Traction control: the share of the throttle to send now (1 = all of it; 1 with TC off).
float Physics_TcThrottle();
// ABS / traction control working right now (for the HUD icons). Game thread.
void Physics_Aids(bool* absActive, bool* tcActive);

// Per-car settings (game thread). The player's car changed: its [Car.<name>] section, if it has
// one, replaces the [Physics] slider values in g_cfg until another car is driven.
void Physics_SetCar(const char* vehicleName);
const char* Physics_Car();                  // "" until a car is known
// The current car's Character and real weight factors and stock data (cars.h), as of its last
// physics step; `known` false before one, or for a car the game's data doesn't have.
const CarCharacter& Physics_Character();
// Where the current car's per-car sliders come from, and are saved: its own [Car.<name>]
// section, a group's [Group.<name>] (shared by the cars in it), or the shared [Physics].
bool Physics_OwnSettings();                 // the current car has its own section
const char* Physics_CarGroup();             // the group it is in, "" = none (or its own)
std::string Physics_Section();              // where the per-car sliders are saved
void Physics_UseShared();                   // the current car on [Physics] (its own section deleted, out of its group)
void Physics_UseOwn();                      // its own section, from the values in use
void Physics_UseGroup(const std::string& name);    // into a group (its values from now on)
// Groups (game thread): names in the ini's order; their cars (CARS folder names).
std::vector<std::string> Physics_Groups();
std::vector<std::string> Physics_GroupCars(const std::string& name);
// A new group, "Group <n>", from the values in use (the car being driven), with no cars yet. Its name.
std::string Physics_NewGroup();
// A group's value of a per-car setting (`value` = its g_cfg field), and setting it (written at
// once; the car being driven gets it too if it is in the group).
float Physics_GroupValue(const std::string& name, const float* value);
void Physics_SetGroupValue(const std::string& name, const float* value, float v);
// Renames a group; nullptr if done, else why not (empty, [ ] = ; , or taken).
const char* Physics_RenameGroup(const std::string& from, const std::string& to);
bool Physics_CarHasOwn(const std::string& car);    // the car has its own [Car.<name>] values
void Physics_DeleteGroup(const std::string& name);
// Profiles: a group (its values and cars) or a car's own settings in a file of its own, in
// scripts\profiles\, to share. Export writes "Group - <name>.ini" / "Car - <car>.ini" (every
// value filled in) and returns its path ("" if there is nothing to export). Import makes the group
// (or replaces the one with its name; its cars move into it) or the car's own settings, and says
// what it did.
struct PhysicsProfile {
    std::string file;           // in the profiles folder
    bool car = false;           // a car's own settings, else a group
    std::string name;           // the group's name, or the car
    std::vector<std::string> cars;   // a group's cars
};
std::string Physics_ProfileDir();
std::string Physics_ExportGroup(const std::string& name);
std::string Physics_ExportCar(const std::string& car);
std::vector<PhysicsProfile> Physics_Profiles();
std::string Physics_ImportProfile(const std::string& file);   // its cars back on [Physics]
// Adds a car to the group (out of any other) or takes it out; the car being driven switches now.
void Physics_SetInGroup(const std::string& group, const std::string& car, bool in);
bool Physics_IsPerCar(const void* value);   // a g_cfg field that is kept per car
// Removals of the per-car keys that are gone (FrontGrip, RearGrip: now GripBalance) from where the
// sliders are saved, for the menu's save.
void Physics_EraseOldKeys(std::vector<IniWrite>* writes);
// How far behind where the game puts it the current car's engine smoke comes out (m): the
// rear- and mid-engined cars have their engine behind the driver. 0 for the others.
float Physics_EngineSmokeBack();
