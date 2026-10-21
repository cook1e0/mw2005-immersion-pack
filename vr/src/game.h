// Game-side hooks for speed.exe v1.3 (6,029,312 bytes).
#pragma once
#include "common.h"

// Check the host and all required hook sites before changing the import or game code.
bool Game_ValidateHooks();
bool Game_InstallHooks();

// True when the current frame rendered separate left/right eye images.
bool Game_StereoThisFrame();
// For the per-frame trace: reflection cube faces drawn this frame (bit per face), the rear view drawn,
// and the shadow map (0 per eye, 1 shared, 2 sharing paused).
void Game_TraceInfo(int* envFaces, int* rearView, int* shadow);
// Symmetric half-FOV tangents the game actually rendered with this frame.
void Game_RenderedFov(float* tanX, float* tanY);
// Called by the Present hook after the frame has been submitted.
void Game_EndOfFrame();

// Toggles the level-horizon camera at runtime.
void Game_ToggleLevel();

// Debug: toggles stereo rendering (VR stays on, game renders mono screen).
void Game_ToggleStereo();
