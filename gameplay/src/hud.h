// The game's speedbreaker meter (right of the tachometer) turned into a car damage meter with
// the engine heat overlaid in red.
#pragma once

// Patches SpeedBreakerMeter's vtable. Called from Game_Install after the version checks.
void Hud_Install();

// Each frame before the HUD is drawn: hides the meter and its icons while the game is paused.
void Hud_Frame();

// Shows a message in the HUD's generic message slot (as "cop taken out" does), with the meter's
// car icon. Game thread. Returns false if there is no HUD.
bool Hud_Message(const char* text);
