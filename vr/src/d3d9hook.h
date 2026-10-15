// D3D9Ex device-side helpers shared between the proxy and the game hooks.
#pragma once
#include "common.h"

// Copies the current back buffer into eye texture 0 or 1.
void D3D_CaptureEye(int eye);

// Initialises OpenXR if it hasn't been yet. Returns whether VR is available.
bool D3D_EnsureXr();

// Records the render target / depth buffer bound when the scene render starts.
void D3D_BeginEyes();
// Restores the state the game's own post-Present Clear leaves the frame in, so the
// next eye starts from the same blank targets as the first one.
void D3D_ResetForNextEye();

// Draws textured quads (four vertices each: x, y, z, u, v) over the eye just rendered, into the
// back buffer with the frame's depth buffer, using the game's view and projection matrices
// (row vectors, as the game sets them). The image is the render target surface `source` (copied
// into a texture first). Used for the side mirrors. All device state is restored.
void D3D_DrawMirrors(const float* verts, int quads, const float* view, const float* proj, void* source, float brightness);

// Draws sampling either texture are skipped (null: none). Used for the HUD's rear view mirror.
void D3D_HideTextures(void* a, void* b);

// Called after the last eye's scene render: captures eye 1 and redirects whatever
// the game draws next (HUD/frontend) into a transparent HUD target.
void D3D_BeginHudPhase();
