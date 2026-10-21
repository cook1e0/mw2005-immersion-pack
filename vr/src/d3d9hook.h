// D3D9Ex device-side helpers shared between the proxy and the game hooks.
#pragma once
#include "common.h"

// Frame timing, logged every 5 s: where a VR frame's time goes (seconds, D3D_ProfNow's clock).
enum ProfSlot { PROF_WAITFRAME, PROF_EYES, PROF_GPUWAIT, PROF_SUBMIT, PROF_SPECTATOR, PROF_PRESENT, PROF_UPDATE, PROF_COUNT };
// The game's update: from the end of Present to the next scene render (simulation, AI, streaming,
// the other mods' per-frame work). Called as the scene render starts.
void D3D_ProfUpdateDone();
double D3D_ProfNow();
void D3D_ProfAdd(int slot, double seconds);

// Copies the current back buffer into eye texture 0 or 1.
void D3D_CaptureEye(int eye);

// Initialises OpenXR if it hasn't been yet. Returns whether VR is available.
bool D3D_EnsureXr();

// Builds what the mirrors and the in-car dashboard would otherwise build on their first draw
// (compiler, shaders, dashboard textures, state block), while the game is still loading.
void D3D_WarmUp();

// Records the render target / depth buffer bound when the scene render starts.
void D3D_BeginEyes();
// Which eye's scene render is running, for the per-frame pass log ([VR] LogPasses): 0 or 1 inside
// an eye, -1 outside (frame setup, HUD).
void D3D_EyePassBegin(int eye);
void D3D_EyeRenderProfile(int eye, double seconds);
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

// The steering wheel in front of the HUD: the cockpit notes the wheel's draw entries (index buffer,
// first index) as the player car is submitted; their draws into the eye mark the stencil, and at the
// end of the eye the HUD panel (last frame's HUD, where the headset would show it: seat-space
// centre (0, hudHeight, hudDistance), hudWidth wide) is drawn into the eye everywhere but the marked
// pixels, instead of as the headset's HUD layer. `on`: driving, not paused (game.cpp); the F4 menu
// being open turns it off too. `seat`: origin, right, up, forward (12 floats) in the world.
void D3D_RegisterWheelDraw(void* indexBuffer, int firstIndex);
void D3D_BeginWheelEye(bool on, const float seat[12], const float* view, const float* proj);
void D3D_EndWheelEye();

// One more glass in an entry already registered (both side mirrors' glass in one entry, as on
// the BMW M3 GTR's body): its own basis and vertices.
void D3D_AddMirrorGlassPart(void* indexBuffer, int firstIndex, const float basis[14], const uint16_t* vertices,
                            int vertexCount);

// Where the side mirror glass (index buffer, first index, as registered) looks for this eye: the
// rear view's texture coordinates seen at its four corners (top left, top right, bottom left,
// bottom right: u, v each), from the eye's view reflected off the glass.
void D3D_SetGlassCorners(void* indexBuffer, int firstIndex, const float uv[8]);

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

// The in-car dashboard: the HUD's parts (Config::dashEl: minimap, cop radar, speedometer, pursuit
// info, leaderboard) each on its own quad on a plane at the car's gauge cluster. For each eye: `on`
// (driving, not paused, Dash on; the F4 menu closed unless on its Adjust dashboard page), the view
// and projection. The cockpit adds the plane (world: origin, right, up, toward the eye, unit) as
// the interior model is submitted; it is drawn right after the game's first draw from that index
// buffer, so the wheel and arms hide it. When it was drawn this frame, the HUD phase copies the
// parts into the dashboard texture and clears the HUD from the headset's HUD layer (but messages).
// Starting an eye also clears its coloured triangles (D3D_AddColorTris).
void D3D_BeginDashEye(bool on, const float* view, const float* proj);
void D3D_AddDash(void* indexBuffer, const float origin[3], const float right[3], const float up[3], const float toEye[3]);
// After the eye's scene (post-processing done): the panel drawn into it, against the scene's depth.
void D3D_EndDashEye();
// Triangles (xyz per vertex, world, already lifted toward the eye) in one colour, drawn right after
// the game's first draw from that index buffer in the player view this eye: opaque ones write
// depth (black triangles of the triangle editor, the car's own gauges), translucent ones don't
// (the editor's highlight). One batch per index buffer and colour per eye.
void D3D_AddColorTris(void* indexBuffer, const float* xyz, int triangles, uint32_t argb);
// Triangles (world) drawn over the finished eye right away, without depth test, in one colour.
void D3D_DrawOverlay(const float* xyz, int triangles, uint32_t argb);

// Called after the last eye's scene render: captures eye 1 and redirects whatever
// the game draws next (HUD/frontend) into a transparent HUD target.
void D3D_BeginHudPhase();
// While set, draws and clears are dropped (the second eye's pass of a shared shadow map: the game
// still runs it, so its state is as usual, but the map keeps the first eye's image).
void D3D_DropDraws(bool on);
