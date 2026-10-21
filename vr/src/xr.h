// OpenXR side of the mod: runtime loading, D3D11 session, swapchains, frame loop.
#pragma once
#include "common.h"
#include "vrmath.h"

struct EyeView {
    Quat orientation;   // eye pose in the runtime's LOCAL space
    V3   position;
    Quat relOrientation; // eye pose relative to the recentred origin (what drives the game camera)
    V3   relPosition;
    float tanLeft, tanRight, tanUp, tanDown; // runtime-recommended FOV (tangents, all positive)
};

enum class LayerMode { None, Stereo, Screen };

// Creates instance + system + D3D11 device + session. d3d9Luid lets us warn if the
// headset GPU differs from the one the game renders on.
bool Xr_Init(LUID d3d9Luid);
void Xr_Shutdown();
bool Xr_Available();                   // a runtime/session exists (or FakeHmd)
// Runtime-recommended per-eye image size (0 when unknown).
void Xr_RecommendedSize(int* width, int* height);

// Begin the XR frame (xrWaitFrame + xrBeginFrame + locate views). Idempotent per frame.
// Returns true when the application should render content this frame.
bool Xr_BeginFrame();
bool Xr_FrameBegun();
// The headset's refresh rate (Hz) from the runtime's predicted display period, 0 until a frame has
// been waited for.
float Xr_DisplayHz();
const EyeView& Xr_Eye(int eye);

// D3D9Ex shared handles of the eye textures and the HUD texture (BGRA8). Re-call when the size changes.
bool Xr_SetSharedTextures(HANDLE eye0, HANDLE eye1, HANDLE hud, int width, int height);

// Submit the frame. For Stereo, tanX/tanY are the symmetric half-FOV tangents the game rendered with;
// withHud adds the HUD texture as a transparent panel in front of the driver.
void Xr_EndFrame(LayerMode mode, float tanX, float tanY, bool withHud);

void Xr_Recenter();

// Name the runtime reports (empty before Xr_Init).
const char* Xr_RuntimeName();

// Handles runtime events (session start/stop, quit requests) outside the frame loop.
void Xr_PollEvents();
