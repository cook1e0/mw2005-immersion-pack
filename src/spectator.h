// Desktop "spectator" view: what the game window (and so OBS) shows while in VR.
#pragma once
#include "common.h"
#include "vrmath.h"
#include <d3d9.h>

enum class SpectatorMode { Eye, Smooth, SideBySide };

struct SpectatorFrame {
    IDirect3DTexture9* eye[2];  // eye images (eye[0] holds the flat frame when !stereo)
    IDirect3DTexture9* hud;     // transparent HUD/frontend layer, or null
    bool stereo;                // eye[0]/eye[1] are 3D eye renders
    float eyeTanX, eyeTanY;     // symmetric half-FOV tangents the eyes were rendered with
    Quat eyeRot[2];             // eye orientations relative to the recentred origin (OpenXR axes)
    V3 headPos;                 // head position relative to the recentred origin
    float hmdTanX, hmdTanY;     // headset per-eye half-FOV tangents (largest of both eyes)
    int bbWidth, bbHeight;      // back buffer size
};

SpectatorMode Spectator_Mode();
void Spectator_CycleMode();
// Client-area aspect ratio the game window should have in the current mode.
float Spectator_WindowAspect(int bbWidth, int bbHeight);
// True once after the desired window aspect changed (e.g. the headset FOV became known).
bool Spectator_ConsumeResize();
// Replaces the back buffer contents with the spectator view.
void Spectator_Draw(IDirect3DDevice9* dev, const SpectatorFrame& f);
// Alpha-blends `tex` over the whole back buffer (used by the raw Eye mode for the HUD).
void Spectator_BlendFullscreen(IDirect3DDevice9* dev, IDirect3DTexture9* tex, int bbWidth, int bbHeight);
