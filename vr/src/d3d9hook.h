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

// The texture a surface belongs to (null if none), looked up once per surface.
void* D3D_TextureOfSurface(void* surface);

// A side mirror's glass: the draw entry (index buffer, first index, triangles) and how the glass
// lies in model space: centre xyz, u axis xyz (to the right as the driver sees it), v axis xyz (up),
// half width, half height, and the rear view's u range for it. Its vertices get texture coordinates
// across the glass (once), and each draw of it samples the rear view instead of its texture.
// `vertices`: the vertex indices the glass's triangles use (as in the index buffer); only these get
// the new coordinates (the rest of the model shares the range).
void D3D_RegisterMirrorGlass(void* indexBuffer, int firstIndex, int triangles, const float basis[14], const uint16_t* vertices,
                             int vertexCount);

// Draws sampling any of these textures are skipped (null: none). Used for the HUD's rear view mirror.
void D3D_HideTextures(void* a, void* b, void* c);

// Mirrors drawn with the game's models: for the eye about to render, the view and projection it
// uses and the mirrors' glass (triangles: x, y, z, u, v per vertex, world space), each drawn right
// after the game's first draw from that index buffer, so the scene's depth hides it where the car
// does. Set as the cars are submitted, before the queue is drawn.
void D3D_BeginMirrorEye(const float* view, const float* proj, float brightness);
// `frame` (optional): drawn plain black first, under the image (the housing's rim).
void D3D_AddMirror(void* indexBuffer, const float* verts, int vertexCount, const float* frame, int frameCount);
// After the eye: mirrors whose model never came through are drawn now (over the scene), and the
// rear view surface is copied for the next eye's mirrors.
void D3D_EndMirrorEye(void* rearViewSurface);

// Called after the last eye's scene render: captures eye 1 and redirects whatever
// the game draws next (HUD/frontend) into a transparent HUD target.
void D3D_BeginHudPhase();
