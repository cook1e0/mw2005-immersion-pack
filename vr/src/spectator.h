// Desktop "spectator" view: what the game window (and so OBS) shows while in VR.
#pragma once
#include "common.h"
#include "vrmath.h"
#include <d3d9.h>

enum class SpectatorMode { Eye, Smooth, SideBySide };

struct SpectatorFrame {
    IDirect3DTexture9* eye[2];  // eye images (eye[0] holds the flat frame when !stereo)
    IDirect3DTexture9* hud;     // transparent HUD/frontend layer, or null
    IDirect3DTexture9* overlay; // pixel-based mod overlay, kept separate from the anamorphic game HUD
    float overlayAspect = 0;    // mod layer uses desktop client aspect for matching mouse coordinates
    bool stereo;                // eye[0]/eye[1] are 3D eye renders
    float eyeTanX, eyeTanY;     // symmetric half-FOV tangents the eyes were rendered with
    Quat eyeRot[2];             // eye orientations relative to the recentred origin (OpenXR axes)
    V3 headPos;                 // head position relative to the recentred origin
    float hmdTanX, hmdTanY;     // headset per-eye half-FOV tangents (largest of both eyes)
    int bbWidth, bbHeight;      // back buffer size
    unsigned poseEpoch;         // recenter/tracking discontinuity since the rendered image
    bool menuOpen;              // keep controls visible with the separate HUD off
    float panelAspect;          // frontend/HUD content aspect; zero keeps the legacy 4:3 canvas
};

SpectatorMode Spectator_Mode();
void Spectator_CycleMode();
float* Spectator_Setting(const char* name);
bool Spectator_Edit(const char* name, float value);
// Client-area aspect ratio the game window should have in the current mode.
float Spectator_WindowAspect(int bbWidth, int bbHeight);
// True once after the desired window aspect changed (e.g. the headset FOV became known).
bool Spectator_ConsumeResize();
// Replaces the back buffer contents with the spectator view.
void Spectator_Draw(IDirect3DDevice9* dev, const SpectatorFrame& f);
// Alpha-blends `tex` over the whole back buffer (used by the raw Eye mode for the HUD).
void Spectator_BlendFullscreen(IDirect3DDevice9* dev, IDirect3DTexture9* tex, int bbWidth, int bbHeight,
                               IDirect3DSurface9* target = nullptr, bool premultiplied = false, float contentAspect = 0, float targetAspect = 0);
