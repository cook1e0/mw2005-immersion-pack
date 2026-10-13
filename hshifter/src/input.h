// Background input thread: clutch pedal and shifter position from the configured bindings.
#pragma once
#include "common.h"

struct InputSnapshot {
    bool  clutchBound = false;  // some clutch binding is usable right now
    float clutchPedal = 0.0f;   // pedal travel, 0 = up, 1 = floored
    bool  shifterBound = false; // some gear binding is usable right now
    int   stickGear = -1;       // -1 neutral, 0 reverse, 1..7 forward gears
    uint32_t toggles = 0;       // incremented on every press of the toggle binding
    uint32_t blows = 0;         // ... of [Engine] DebugBlow
    uint32_t repairs = 0;       // ... of [Engine] DebugRepair
    uint32_t menu = 0;          // ... of [UI] Menu
    uint32_t menuNav[4] = {};   // ... of MenuUp / MenuDown / MenuLeft / MenuRight, with key repeat
};

void Input_Start();             // safe to call repeatedly; starts the thread once
InputSnapshot Input_Get();
