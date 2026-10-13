// Physics sliders for the player's car: driving aids, grip, front/rear grip balance, gravity.
#pragma once

// Hooks the player's suspension and rigid body simulation. Called from Game_Install.
void Physics_Install();
// Once per frame (game thread), reads tuning tables the game builds at startup.
void Physics_Frame();
