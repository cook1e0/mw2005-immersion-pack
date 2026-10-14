// Background input thread: clutch pedal and shifter position from the configured bindings.
#pragma once
#include "common.h"

struct InputSnapshot {
    bool  clutchBound = false;  // some clutch binding is usable right now
    float clutchPedal = 0.0f;   // pedal travel, 0 = up, 1 = floored
    bool  shifterBound = false; // some gear binding is usable right now
    int   stickGear = -1;       // -1 neutral, 0 reverse, 1..7 forward gears
    uint32_t toggles = 0;       // incremented on every press of the toggle binding
    uint32_t shiftUp = 0;       // ... of [Shifter] ShiftUp (sequential mode)
    uint32_t shiftDown = 0;     // ... of [Shifter] ShiftDown
    uint32_t blows = 0;         // ... of [Engine] DebugBlow
    uint32_t repairs = 0;       // ... of [Engine] DebugRepair
    uint32_t hits = 0;          // ... of [Damage] DebugDamage
    uint32_t pops = 0;          // ... of [Tires] DebugPopTires
    uint32_t menu = 0;          // ... of [UI] Menu
    uint32_t recenters = 0;     // ... of [Controls] RecenterVr
    uint32_t menuNav[4] = {};   // ... of MenuUp / MenuDown / MenuLeft / MenuRight, with key repeat

    // [Controls]: driving controls fed to the game as its own actions (controls.cpp)
    bool  steerBound = false;
    float steer = 0.0f;         // -1 full left .. +1 full right, after the steering lock
    bool  pedalBound[2] = {};   // accelerate, brake
    float pedal[2] = {};        // 0 = up .. 1 = floored
    uint32_t buttonBound = 0;   // bit per Button: some binding is usable
    uint32_t buttonDown = 0;    // bit per Button: held
};

// Driving buttons in [Controls], in InputSnapshot bit order.
enum Button { BTN_HANDBRAKE, BTN_NITROUS, BTN_SPEEDBREAKER, BTN_RESET, BTN_CAMERA, BTN_LOOKBACK, BTN_COUNT };

// In-game binding (tuning menu): after BeginCapture the input thread opens every attached
// controller and reports the first key, button, hat direction or axis that is pressed or moved
// (controls already held when capture starts must be released first). Game thread.
void Input_BeginCapture();
void Input_CancelCapture();
// True once something was pressed: binding text ("key:F4", "joy0:b3", "joy1:-rz", ...) and, for an
// axis, its raw value at rest.
bool Input_Captured(std::string* binding, int* axisRest);
// Replaces a binding string in g_cfg and makes the input thread re-read all bindings. Game thread.
void Input_SetBinding(std::string& field, const std::string& value);

void Input_Start();             // safe to call repeatedly; starts the thread once
InputSnapshot Input_Get();
