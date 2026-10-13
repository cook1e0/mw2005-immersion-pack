// Engine heat gauge and the in-game tuning menu, drawn right after the game's HUD.
#pragma once

// Called once per frame from eDisplayFrame, after the HUD / frontend and before EndScene.
// With the VR mod that is inside its HUD phase, so everything lands on the floating HUD panel.
void Ui_Frame();
