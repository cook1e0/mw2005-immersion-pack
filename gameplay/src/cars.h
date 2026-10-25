// Every car model's stock physics data, read from the game's attribute database, and the
// [Physics] Character factors made from it.
#pragma once
#include <string>
#include <vector>

// The player's car's factors on its own physics values. All 1 (0 for the balances) for a car
// the database doesn't have.
struct CarCharacter {
    bool known = false;         // the car's stock data was found
    float spring = 1.0f, damper = 1.0f, swaybar = 1.0f;
    float rollBalance = 0.0f;   // added to [Physics] RollBalance
    float gripBalance = 0.0f;   // added to [Physics] GripBalance (tyre stagger for the real weight)
    float grip = 1.0f;
    float throttle = 1.0f;      // times [Physics] FrictionCircle
    float weightFront = 0.0f;   // FRONT_WEIGHT_BIAS (percent) to use, 0 = the game's
    // The stock car, for the menu.
    float mass = 0.0f;          // kg
    float gameWeightFront = 0.0f;   // the game's FRONT_WEIGHT_BIAS, percent
    float torqueSplit = 0.0f;   // share of the torque to the front axle (0 RWD, 1 FWD)
    float hp = 0.0f;            // peak power from the torque curve and boost (about 10% under the real car's)
};

// The factors for `model` (a CARS folder name, e.g. "mustanggt") at Character `k` (0..1), with
// the real weight distribution when `realWeight`. Game thread only, once the attribute database
// is loaded (a car is being driven); the database is read on the first call.
CarCharacter Cars_Character(const char* model, float k, bool realWeight);

// The player cars (CARS folder names), sorted by their names; the database is read on the first
// call (game thread, a car being driven).
std::vector<std::string> Cars_PlayerModels();
// A model's name to show ("Chevrolet Camaro SS"), the folder name for one not in the list.
std::string Cars_DisplayName(const std::string& model);
