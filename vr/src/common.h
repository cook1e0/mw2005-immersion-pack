// Shared declarations for the NFSMW VR mod.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <math.h>      // NAN (the dashboard elements' default: automatic layout)

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
    float depthSplit = 1.0f;       // seated: the player car gets NearZ, the world WorldNearZ, in separate parts of the depth buffer (0/1, live)
    float worldNearZ = 0.3f;       // with DepthSplit, the near plane of everything but the player car (m)
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
    int   keyTrace = VK_DELETE;      // debug: record a 30 s per-frame trace to nfsmw_frametrace_NNN.csv (0 = disabled)
    float renderPriority = 1;        // the render thread as an MMCSS "Games" task and a 1 ms system timer (0 = off)
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
    float dash = 1.0f;               // 1 = minimap, pursuit meter and speedometer on the car's dashboard, not on the HUD (live from F4)
    float dashRight = 0.0f;          // dashboard panel moved right (m) from the gauge cluster ([Dash], always per car model)
    float dashUp = 0.0f;             // ... and up (m)
    float dashZoom = 1.0f;           // its size
    float dashDepth = 0.0f;          // ... and toward the driver (m) from 3 cm off the gauge cluster
    float dashYaw = 0.0f;            // ... turned to the right (+) / left (-) about its own up axis, degrees
    float dashPitch = 0.0f;          // ... tilted to face up (+) / down (-) about its own right axis, degrees
    float dashCover = 1.3f;          // (old rectangle cover, no longer drawn: the gauges' own triangles are black)
    // Each dashboard element (DASH_EL_*): x right, y up (m, at zoom 1, from the panel's anchor) and
    // size (1 = the HUD's own scale, 0 = hidden). x/y NaN = automatic layout. F4 > VR > Adjust dashboard;
    // shared by all cars in [DashLayout] Default. Legacy [DashLayout] SUPRA is used if Default is absent.
    float dashEl[6][3] = {{NAN, NAN, 1.0f}, {NAN, NAN, 1.0f}, {NAN, NAN, 1.0f},
                          {NAN, NAN, 1.0f}, {NAN, NAN, 1.0f}, {NAN, NAN, 1.0f}};
    // Each element's crop of the HUD: left, top, right, bottom (shares of the HUD picture, the game's
    // own 4:3 layout: the widescreen fix's FixHUD off). Keys Dash<Name>L / T / R / B; the same for every
    // car (it's the HUD's layout).
    float dashCrop[6][4] = {{0.165f, 0.555f, 0.415f, 0.980f},   // minimap
                            {0.370f, 0.165f, 0.630f, 0.295f},   // cop radar
                            {0.585f, 0.5375f, 0.8575f, 0.990f}, // speedometer
                            {0.350f, 0.7025f, 0.6625f, 0.900f}, // pursuit info
                            {0.635f, 0.000f, 0.950f, 0.430f},   // leaderboard / bounty
                            {0.355f, 0.2925f, 0.6325f, 0.5325f}}; // messages
    float dashBox = 0.55f;           // the display: a white screen with an orange outline the elements sit on, its opacity (0 = none)
    unsigned dashColor = 0x3C3F44u;       // the display's colour (DashColor, hex RRGGBB): the game's HUD gray
    float dashGlass = 0.05f;              // the glass sheen over the display and its elements: its opacity (0 = none)
    float dashGlassDepth = 0.01f;         // ... how far in front of the display (m)
    float dashReflect = 0.05f;             // ... the world (the rear view picture) reflected in it: strength (0 = none)
    float wheelPivot[2] = {0, 0};         // the steering wheel's turning centre moved right, up in its plane (m; per car, [WheelPivot])
    float dashScreenW = 0.30f, dashScreenH = 0.13f;   // ... its size (m, at zoom 1; per car in [Dash]), centred on the panel's anchor
    float dashElBox[6] = {1, 1, 1, 1, 1, 0};   // (no longer used: one display for all)
    // Live from F4, not saved: the Adjust dashboard page is open (the panel shows with the menu open,
    // `dashHighlight` outlined); the Triangle editor page is open (picking by gaze), and its command
    // (1 hide, 2 black, 3 add to the steering wheel, 4 take off it, 5 restore, 6 undo; reset once done).
    float dashAdjust = 0, dashHighlight = -1;
    float editorOn = 0, editCommand = 0;
    // Live from F4, not saved: picking (the menu hidden: the selection follows the gaze), which of
    // the triangles along the gaze (0 = nearest), the selection's size (0 one triangle, 1..3 within
    // 2 / 5 / 10 cm, 4 the connected piece, 5 the whole draw entry); out: triangles selected.
    // Commands besides those above: 7 add to the dashboard blackout, 8 take off it, 9 drop the selection.
    float editPicking = 0, editLayer = 0, editMode = 0, editPicked = 0;
    float editBrush = 0.0f;          // triangle editor brush radius (m, 0 = one triangle)
    float headSize = 0.25f;          // how far down from the top of the driver model the head reaches (m)
    bool  sharedViewsOnce = true;    // rear view and environment map rendered once a frame, not per eye
    int   envMapFacesPerFrame = 2;   // environment map cube faces refreshed per VR frame (1..6, 6 = every face every frame)
    float rearViewEvery = 1.0f;      // the rear view is rendered every Nth VR frame (1 = every frame; live)
    float sharedShadows = 1.0f;      // the sun shadow map drawn for the first eye only; the second eye reuses it (0/1, live)
    float smaa = 3.0f;               // SMAA on each eye's picture: 0 off, 1 low, 2 medium, 3 high, 4 ultra (live from F4)
    float smaaEdges = 0.0f;          // test: the eyes show the edges SMAA found instead of the picture (0/1)
    float anisotropic = 4.0f;        // anisotropic filtering: 0 off, 1 2x, 2 4x, 3 8x, 4 16x (live from F4)
    float simRateFromHeadset = 1.0f; // the widescreen fix's SimRate set to a multiple of the headset's rate (next start)
    float shadowRes = 2.0f;          // the widescreen fix's ShadowsRes over the game's 1024 (F4; next start)
    float shadowArea = 0.75f;        // the sun shadow map's coverage around you (1 = the game's ~350 m square)
    bool  asyncSubmit = true;        // hand the finished frame to the XR bridge without waiting for it
    bool  logGlassShader = false;    // debug: disassemble the game's glass vertex shader into the log (one-off cost)
    bool  logPasses = false;         // debug: list how often the game binds each render target per frame (per-eye vs shared passes)
    bool  eyeCameraShared = false;   // debug: render both eyes from the seat camera (no eye offset), so a difference
                                     // between the two images is per-eye state (a pool, a counter, a matrix) instead of the eye's viewpoint
    bool  screenRain = false;        // raindrops on the screen in the headset (always shown on a flat screen)
    bool  directionArrow = false;    // the direction arrow above the car in the headset (always shown on a flat screen)
    bool  sunGlare = false;          // the screen-space sun glare in the headset (always shown on a flat screen)
    bool  stableReflections = true; // reflections use the seat's view, not the head's (they sit still as the head moves)
    bool  mirrors = true;            // the game's rear view on the interior rear view mirror (VR), no HUD mirror
    bool  gameMirrors = false;       // the game's side mirror glass shows the rear view (VR); off = vanilla side mirrors
    char  gameMirrorsSkip[512] = {}; // cars (model names, comma separated) whose side mirrors stay vanilla
    bool  mirrorFlip = false;        // flip the mirror image left-right
    float mirrorScale = 0.82f;       // the image's size on the glass; the rest is a dark rim (1 = no rim)
    float mirrorBrightness = 1.4f;   // mirror picture brightness (all mirrors; the car's lighting darkens the side mirrors)
    float mirrorContrast = 1.25f;    // mirror picture contrast
    float mirrorOffset = 0.0f;       // our rear view mirror picture moved toward the driver (+) or back into the glass (m), per car ([MirrorOffset])
    float sideMirrorWidth = 0.6f;    // share of the rear view's width each side mirror shows
    bool  steeringWheel = true;      // the player car's steering wheel turns with the bound wheel (gameplay mod)
    int   keySeatForward = VK_NUMPAD8, keySeatBack = VK_NUMPAD2;
    int   keySeatLeft = VK_NUMPAD4, keySeatRight = VK_NUMPAD6;
    int   keySeatUp = VK_NUMPAD9, keySeatDown = VK_NUMPAD3, keySeatReset = VK_NUMPAD5;
    int   keyToggleShared = 0;       // debug: toggle reusing the rear-view/environment-map views
    int   keyToggleSameEye = 0;      // debug: toggle rendering both eyes from the same (seat) camera
};
extern Config g_cfg;
// Dashboard elements, in Config::dashEl order: minimap, cop radar, speedometer, pursuit info, leaderboard.
enum { DASH_EL_MAP, DASH_EL_RADAR, DASH_EL_SPEEDO, DASH_EL_PURSUIT, DASH_EL_BOARD, DASH_EL_MSG, DASH_EL_COUNT };
extern const char* const kDashElKey[6];     // ini key part: Map, Radar, Speedo, Pursuit, Board, Msg (messages)
void LoadConfig();

// Directory containing speed.exe (with trailing backslash).
const char* GameDir();
// Folder of this DLL and the mods' .ini files (see common.cpp).
const char* ModDir();
void ConfigFile(const char* name, char* out, bool migrate);
