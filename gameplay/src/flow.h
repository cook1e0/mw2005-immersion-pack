// Game flow fixes: the first safehouse visit (Mia's intro) when the career script misses it, the car
// handed back at a race start when the race script doesn't, and leftover screen clutter (the
// loading screen's developer text, HUD popups, the EA Trax song banner).
#pragma once

// Installs all of them. Called from Game_Install.
void Flow_Install();

// Every step of the player's engine: hands the car back if the AI still drives it after GO.
// Game thread only.
void Flow_PlayerStep();
