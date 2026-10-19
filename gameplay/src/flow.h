// Career flow: the first safehouse visit (Mia's intro) when the career script misses it, and the car
// handed back at a race start when the race script doesn't.
#pragma once

// Hooks the safehouse door and the intro, and the race start hand-over. Called from Game_Install.
void Flow_Install();

// Every step of the player's engine: hands the car back if the AI still drives it after GO.
// Game thread only.
void Flow_PlayerStep();
