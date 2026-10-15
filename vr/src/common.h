// Shared declarations for the NFSMW VR mod.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Logging (nfsmw_vr.log next to speed.exe)
// ---------------------------------------------------------------------------
void LogInit(const char* fileName = "nfsmw_vr.log");
void Log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Configuration (nfsmw_vr.ini next to speed.exe)
// ---------------------------------------------------------------------------
struct Config {
    bool  enabled = true;          // master switch; false = plain passthrough d3d9
    bool  crashDumps = true;        // save local diagnostics on an unhandled game exception
    bool  forceWindowed = true;    // keep the desktop usable while in the headset
    bool  disableVSync = true;     // the headset paces frames, not the monitor
    float worldScale = 1.0f;       // multiplies head translation (game units are metres)
    float levelRoll = 1.0f;        // 0..1: how much of the car's roll to remove from the camera
    float levelPitch = 0.0f;       // 0..1: how much of the car's pitch to remove (1 = always look at the horizon)
    char  resolution[32] = "auto"; // "auto" = headset per-eye size, "game" = in-game setting, or "WIDTHxHEIGHT"
    float resolutionScale = 1.0f;  // multiplies the "auto" resolution
    float fovScale = 1.0f;         // >1 renders a slightly wider FOV than the HMD (more reprojection margin)
    float nearZ = 0.05f;           // player view near clip plane (m; the game's is 0.5), so the cockpit isn't cut off; 0 = the game's own
    float screenDistance = 2.5f;   // menu/movie virtual screen distance (m)
    float screenWidth = 3.0f;      // menu/movie virtual screen width (m)
    float hudDistance = 1.2f;      // in-game HUD/frontend panel distance (m)
    float hudWidth = 1.4f;         // in-game HUD/frontend panel width (m)
    float hudHeight = -0.05f;      // vertical offset of the HUD panel (m)
    float hudRight = 0.0f;         // sideways offset of the HUD panel (m, + = right)
    int   bridge = -1;             // 64-bit OpenXR host (nfsmw_vr_host.exe): -1 auto, 0 never, 1 always
    bool  fakeHmd = false;         // debug: run the stereo path with a fake head when no runtime is present
    float fakeYaw = 0, fakePitch = 0; // debug: fake head orientation in degrees (+yaw = left, +pitch = up)
    float fakeIpd = 0.064f;        // debug: fake eye separation (m)
    int   dumpFrames = 1;          // debug: consecutive frames written per dump key press
    int   keyRecenter = VK_F8;
    int   keyDump = 0;               // debug: dump eye/HUD images to BMP (0 = disabled)
    int   keyToggle = 0;             // debug: toggle stereo rendering (0 = disabled)
    int   keyLevel = VK_F7;
    int   keySpectator = VK_F6;
    char  spectatorMode[16] = "smooth"; // desktop window: smooth (stabilised headset view), sbs (both eyes), eye (raw)
    int   spectatorEye = 0;          // eye used by "smooth" (0 = left, 1 = right)
    float spectatorFov = 0.0f;       // horizontal FOV of the smooth/sbs views (degrees); 0 = match the headset
    float spectatorAspect = 0.0f;    // width/height of one view; 0 = match the headset (e.g. 1.7778 for 16:9)
    float spectatorSmoothing = 0.2f; // head-motion smoothing time constant (s); 0 = raw head
    bool  spectatorFollowHead = true; // false = locked straight ahead (classic hood-cam footage)
    int   spectatorWidth = 1600;     // window client width in smooth/sbs modes (px)
    bool  seat = true;               // bumper / hood camera: eyes at the driver model's head
    bool  hideHead = true;           // leave out the driver model's head when it is at the eyes
    float hideArms = 1.0f;           // 1 = leave out the seated driver's arms and hands (a 0/1 float: live from F4)
    float hudPerCar = 0.0f;          // 1 = each car remembers its HUD size, distance and height ([Hud] per car; live from F4)
    float wheelOverHud = 0.0f;       // 1 = the car's steering wheel in front of the HUD panel while driving (live from F4)
    float headSize = 0.25f;          // how far down from the top of the driver model the head reaches (m)
    bool  sharedViewsOnce = true;    // rear view and environment map rendered once a frame, not per eye
    bool  screenRain = false;        // raindrops on the screen in the headset (always shown on a flat screen)
    bool  stableReflections = true; // reflections use the seat's view, not the head's (they sit still as the head moves)
    bool  mirrors = true;            // the game's rear view on the interior rear view mirror (VR), no HUD mirror
    bool  gameMirrors = false;       // the game's side mirror glass shows the rear view (VR); off = vanilla side mirrors
    char  gameMirrorsSkip[512] = {}; // cars (model names, comma separated) whose side mirrors stay vanilla
    bool  mirrorFlip = false;        // flip the mirror image left-right
    float mirrorScale = 0.82f;       // the image's size on the glass; the rest is a dark rim (1 = no rim)
    float mirrorBrightness = 1.4f;   // mirror picture brightness (all mirrors; the car's lighting darkens the side mirrors)
    float mirrorContrast = 1.25f;    // mirror picture contrast
    bool  steeringWheel = true;      // the player car's steering wheel turns with the bound wheel (gameplay mod)
    int   keySeatForward = VK_NUMPAD8, keySeatBack = VK_NUMPAD2;
    int   keySeatLeft = VK_NUMPAD4, keySeatRight = VK_NUMPAD6;
    int   keySeatUp = VK_NUMPAD9, keySeatDown = VK_NUMPAD3, keySeatReset = VK_NUMPAD5;
};
extern Config g_cfg;
void LoadConfig();

// Directory containing speed.exe (with trailing backslash).
const char* GameDir();
// Folder of this DLL and the mods' .ini files (see common.cpp).
const char* ModDir();
void ConfigFile(const char* name, char* out, bool migrate);
