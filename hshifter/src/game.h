// Hooks into speed.exe v1.3: gear selection (InputPlayer), clutch and engine wear (EngineRacer),
// and a HUD draw hook for the overlay.
#pragma once

// Verifies the code at every hook site and installs the hooks. Returns false (and patches
// nothing) if the executable is not the expected version.
bool Game_Install();

// The local player's engine, for the heat gauge. Game thread only.
struct EngineStatus {
    bool driving = false;       // the player's engine simulated in the last moments (not paused)
    bool wear = false;          // engine wear applies (manual shifting, not a drag race)
    bool blown = false;
    float heat = 0.0f;          // 0..1
};
EngineStatus Game_Engine();

// For the HUD meter: the player car's damage (0..1, 1 = totaled) and engine heat. Game thread only.
struct HudStatus {
    bool valid = false;         // the player's car is known
    float damage = 0.0f;
    float heat = 0.0f;          // 0 unless engine wear applies
};
HudStatus Game_Hud();

// The local player's car as a COM object: the interface for `handle` (an _IHandle function's
// address), or the car's ISimable for nullptr. Null when not driving. Game thread only.
void* Game_PlayerInterface(void* handle);

// Blocks the car's controls while the tuning menu is open. Game thread only.
void Game_SetMenuOpen(bool open);
