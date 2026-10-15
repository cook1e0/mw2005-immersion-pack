// Game-side hooks for speed.exe v1.3 (6,029,312 bytes).
#pragma once
#include "common.h"

// Check the host and all required hook sites before changing the import or game code.
bool Game_ValidateHooks();
bool Game_InstallHooks();

// True when the current frame rendered separate left/right eye images.
bool Game_StereoThisFrame();
// Symmetric half-FOV tangents the game actually rendered with this frame.
void Game_RenderedFov(float* tanX, float* tanY);
// Called by the Present hook after the frame has been submitted.
void Game_EndOfFrame();

// Toggles the level-horizon camera at runtime.
void Game_ToggleLevel();

// Debug: toggles stereo rendering (VR stays on, game renders mono screen).
void Game_ToggleStereo();

// In the world and driving (not paused or in a menu).
bool Game_HudInWorld();
