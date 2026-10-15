// Stereo rendering for NFS Most Wanted (2005), speed.exe v1.3.
//
// eDisplayFrame (0x6E7220) builds every eView's matrices (sub_6CF400), opens the
// scene, calls ___eDisplayFrame (0x6DE300) to render all views (shadows,
// reflections, world, cars, particles, post FX) into the back buffer, then
// presents. We retarget two call sites:
//
//   0x6E73B8  call sub_6CF400      -> hkViewSetup  (records the player view's setup args)
//   0x6E759C  call ___eDisplayFrame -> hkRenderScene
//
// hkRenderScene renders the scene once per eye: it moves the player camera to the
// eye pose, rebuilds the player view's matrices with the headset FOV, runs the
// original renderer, and copies the result into that eye's texture.
//
// Addresses/offsets come from the dbalatoni13/nfsmw decompilation's 1.3 symbol
// map and were checked against this exe's disassembly.
#include "game.h"
#include "d3d9hook.h"
#include "xr.h"
#include "vrmath.h"
#include "cockpit.h"
#include <string.h>
#include <algorithm>
#include <stdio.h>

namespace {

// --- speed.exe v1.3 addresses --------------------------------------------
constexpr uintptr_t kCallViewSetup = 0x6E73B8;   // call sub_6CF400 inside eDisplayFrame's view loop
constexpr uintptr_t kViewSetup = 0x6CF400;       // eViewPlatInterface matrix/frustum setup (thiscall, 5 args)
constexpr uintptr_t kCallRenderScene = 0x6E759C; // call ___eDisplayFrame inside eDisplayFrame
constexpr uintptr_t kRenderScene = 0x6DE300;     // ___eDisplayFrame (cdecl, no args)
constexpr uintptr_t kGetResolution = 0x6C27D0;   // __getResolutionByIndex(int* w, int* h), stdcall-like (ret 8)
constexpr uintptr_t kResolutionIndex = 0x90181C; // selected entry of the 6 built-in resolutions
constexpr uintptr_t kPlayerView = 0x919650;      // &eViews[EVIEW_PLAYER1]
// Raindrops on the screen (Rain's OnScreenRain, drawn as a screen-space pass): stuck to your eyes
// in a headset. The pass returns at once when the player view's rain has no drops on screen.
constexpr uintptr_t kCallScreenRain = 0x6DF7EC;   // call sub_6D4620 in ___eDisplayFrame
constexpr uintptr_t kScreenRain = 0x6D4620;       // cdecl, 1 arg
// Reflections: elSetupEnvMap puts the reflection "eye" 2.78 m from the car (or model) toward the
// camera position it is given. In the cockpit the camera sits next to the car's centre, so a few
// centimetres of head movement swung that direction, and every car and window reflection with
// it. Both of these (cdecl, 6 args, the 5th/4th is the camera position) get the seat position.
constexpr uintptr_t kCloneLightContext = 0x502580;  // (ctx, local_world, world_view, cam_pos, view, old)
constexpr uintptr_t kSetupLightContext = 0x502520;  // (ctx, rig, local_world, world_view, cam_pos, view)
constexpr uintptr_t kCallsCloneLightContext[] = {0x74E474, 0x74F62E, 0x74F7D6, 0x74F968, 0x74FBC4,
                                                 0x74FFAB, 0x750158, 0x75031A, 0x7504F8};
constexpr uintptr_t kCallsSetupLightContext[] = {0x502AAF, 0x73BB67, 0x73BB89};
// Side mirrors. The game renders its rear view (eViews[3], EVIEW_PLAYER1_RVM, from a
// RearViewMirrorCameraMover the driving camera always creates) into the "RVM" texture, shown at
// the top of the HUD when the rear view mirror option is on. In the headset that HUD mirror is
// clutter, so the image goes onto the player car's mirrors instead (cockpit.cpp finds the glass).
// MaybeChangeViewMode only renders the rear view (view mode ONE_RVM) while sub_595D40 allows it:
// option on, the race HUD up and not an in-car camera, so never in the headset's bumper/hood
// views. While VR runs it always allows it, the mirror camera's option check reads "on", and the
// HUD's mirror (drawn with the "RVM" and "RVM_MASK" textures) is left out by the d3d9 wrapper.
constexpr uintptr_t kMirrorCamOption = 0x4766A7;  // RearViewMirrorCameraMover::Update: mov al, [ecx+0x39]
constexpr uint8_t kMirrorCamGame[3] = {0x8A, 0x41, 0x39}, kMirrorCamOn[3] = {0xB0, 0x01, 0x90};     // mov al, 1
constexpr uintptr_t kRvmAllowed = 0x595D40;       // bool sub_595D40(view): rear view mirror allowed
constexpr uint8_t kRvmAllowedGame[3] = {0xA1, 0x84, 0xD8}, kRvmAllowedOn[3] = {0xB0, 0x01, 0xC3};   // mov al, 1; ret
constexpr uintptr_t kRvmTexture = 0x982B48;       // TextureInfo* "RVM": +0 platform info, its +0x18 the D3D texture
constexpr uintptr_t kRvmMaskTexture = 0x982B4C;   // TextureInfo* "RVM_MASK"
// ___eDisplayFrame renders eViews[3] while it is active into this render target (type, ?, colour
// surface, depth surface), then the HUD draws it; the image is taken from its colour surface.
constexpr uintptr_t kRvmTarget = 0x93DB0C;
constexpr int kTargetSurface = 0x08;
constexpr uintptr_t kRvmViewActive = 0x919738;    // eViews[3] + 8
constexpr int kPlatView = 0x00;                   // eViewPlatInfo: view matrix (the camera's), projection at +0x40
// Reflections and the head: the per-object transform setup (sub_6C8000) gives every shader
// WorldView = local * view, with D3DXMatrixMultiply at kCallWorldView (WorldViewProj is a separate
// multiply). The car paint shader turns its reflection vector into the environment map with
// WorldView's rotation, and the glossy window shader slides its reflection with WorldView's first
// row: in the headset, with the eye's view, every reflection turned with the head. While the eyes
// render, WorldView gets the seat's view (the game camera in the seat, without the head) instead,
// so reflections sit still as the head moves, as they do with the flat screen's camera.
constexpr uintptr_t kCallWorldView = 0x6C8147;    // call D3DXMatrixMultiply(out, local, view) (stdcall thunk)
constexpr uintptr_t kMatrixMultiply = 0x7E7D5E;
// The game's own mirror glass (side mirrors) uses the global light material "MIRROR": black
// diffuse, a full white specular highlight and an environment map reflection of 0.001, i.e. no
// reflection, just a pale glow (the HUD had the rear view). While the rear view is on the glass
// (d3d9proxy swaps it in as the glass's texture), the material is plain white: the picture, lit
// like the rest of the car, and no glow or environment map over it. The shader reads the
// material's values at every draw.
constexpr uintptr_t kGetLightMaterial = 0x502820;  // eLightMaterial* elGetLightMaterial(uint32 hash), cdecl
constexpr uint32_t kMirrorMaterialHash = 0x6BFA56DA; // bStringHash("MIRROR")
constexpr int kMatNameHash = 0x0C, kMatData = 0x30;  // eLightMaterial: NameHash, eLightMaterialData
enum {
    kMatDiffMinR = 1, kMatDiffMaxR = 5, kMatSpecMinScale = 11, kMatSpecMaxScale = 15, kMatEnvPower = 19,
    kMatEnvMinScale = 20, kMatEnvMaxScale = 24,
};
// Both eyes run ___eDisplayFrame, and it starts with ePreDisplay, which steps per-frame state:
// sound, animated world models, the sun flare, the light flares' parameters, tunnel bloom and
// tinting fades. Run twice, the second eye got a later step than the first: blinking lights,
// flares and fades differed between the eyes. It runs for the first eye only.
// The views that don't depend on the eye are rendered once a frame, for the first eye: the rear
// view (3) and the environment map's six cube faces (18..23, rendered in ___eDisplayFrame's loop while
// active). For the second eye they are switched off and back; it uses the first eye's images.
constexpr int kSharedViews[] = {3, 18, 19, 20, 21, 22, 23};
constexpr uintptr_t kCallPreDisplay = 0x6DE3D1;   // call ePreDisplay in ___eDisplayFrame
constexpr uintptr_t kPreDisplay = 0x504C50;       // cdecl, no args
constexpr uintptr_t kViews = 0x9195E0;           // eViews[NUM_EVIEWS], 0x70 bytes each
constexpr int kViewStride = 0x70, kNumViews = 22;
// Passes drawn from the player's camera and laid over its image: EVIEW_PLAYER1_GLOW (bloom) and
// EVIEW_PLAYER1_SPECULAR (the wet-road / paint highlights). They share the player's Camera, but
// their matrices are built once per frame in eDisplayFrame's view loop; built from the game's
// camera while each eye looks with the head, their overlay turned with the head.
constexpr int kCompanionViews[] = {4, 6};
constexpr uintptr_t kPlayerAspectScale = 0x8AF994; // float 1.15 applied to player views' aspect
constexpr uintptr_t kCallTotaledCam = 0x631257;  // EShowRaceOverMessage: call CameraAI::MaybeDoTotaledCam(IPlayer*)
constexpr uintptr_t kTotaledCam = 0x479F20;      // cdecl, 1 arg
constexpr uintptr_t kJumpCamsOn = 0x469E30;      // cdecl, reads the "Jump Cams" option from the user profile
// Calls of kJumpCamsOn: CameraAI::MaybeDoPursuitCam (cop cam when a pursuit starts),
// Director::SelectAction (jump cam while airborne) and the crash cam at 0x479F80.
constexpr uintptr_t kCallsJumpCamsOn[] = {0x471BFD, 0x479A59, 0x479F9C};
// ECinematicMoment (pursuit breakers and other scripted moments) plays an ICE camera track:
constexpr uintptr_t kCallPlayIce = 0x6405A4;     // call ICEManager::SetGenericCameraToPlay(name, track)
constexpr uintptr_t kPlayIce = 0x46E2D0;         // thiscall, 2 args
constexpr uintptr_t kCallIceAction = 0x6405B0;   // call CameraAI::SetAction(EVIEW_PLAYER1, "CDActionIce")
constexpr uintptr_t kSetAction = 0x479EB0;       // cdecl, 2 args
// CameraAI::Director::Update calls SelectAction, which cuts to CDActionTrackCop (watching the
// cops) while the director's track-cop timer runs. Pursuit breakers start it directly, without the
// "Jump Cams" check the pursuit cam has.
constexpr uintptr_t kCallSelectAction = 0x479E4C; // call Director::SelectAction in Director::Update
constexpr uintptr_t kSelectAction = 0x479900;     // thiscall, no args
constexpr int kDirectorView = 0x08;               // EVIEW_ID
constexpr int kDirectorTrackCopTimer = 0x2B4;     // seconds of track-cop camera left
// SelectAction has more ways off the driving camera: the ICE camera when a cinematic moment's
// track is set or an NIS asks for it, and the moment camera (CDActionTrackCar). Whatever it picks
// for the player's view is checked afterwards: anything but the driving camera is turned back
// while VR runs, except during an NIS (race intros, busted: the game's own cutscenes).
constexpr int kDirectorActionName = 0x1C;         // Attrib::StringKey of the current action: its string
constexpr uintptr_t kDirectorSetAction = 0x4797F0; // thiscall, Attrib::StringKey (16 bytes) by value
constexpr uintptr_t kStringKeyCtor = 0x453760;    // Attrib::StringKey::StringKey(const char*), thiscall
constexpr uintptr_t kNisInstance = 0x9885C8;      // UTL::Singleton<INIS>::mInstance: non-null during an NIS
constexpr uintptr_t kActionDriveName = 0x894C20;  // "CDActionDrive" in speed.exe
// Not only SelectAction changes the camera: CameraAI::SetAction (0x479EB0) sets an action
// directly, from the cinematic moment (pursuit breakers: "CDActionIce") and from 0x47A12F
// ("CDActionTrackCar", the slow-motion moment camera), between the director's selections. Both of
// the director's own SetAction calls are checked, so no path cuts away from the cockpit.
constexpr uintptr_t kCallsDirectorSetAction[] = {0x479DD5, 0x479EFD};
// NIS screen overlays (table at 0x8EDEEC: 1 FLASHERS, 2 BLACK_FADE_OUT, 3 BUSTED_OVERLAY,
// 4 BLACK_BACKGROUND), pushed by id for an NIS event. BUSTED_OVERLAY.fng is the square
// camera-viewfinder frame over the busted cutscene.
constexpr uintptr_t kCallNisOverlay = 0x482EAC;   // call ShowNISOverlay(id) for an NIS event
constexpr uintptr_t kNisOverlay = 0x46EA50;       // cdecl, 1 arg (the id, a byte)
constexpr int kNisOverlayBusted = 3;
// Pursuit breakers also start an NIS (the game's scripted cutscene, let through for race intros
// and busted): ETriggerMomentNIS (handled at 0x63BE40, from its event slot 0 at 0x6424B3) stamps
// the sim time in 0x90D9D4, and the moment code (0x641080) starts the NIS while that stamp is under
// 2 s old, freezing the world and placing the car again after it. In VR the stamp is put back, so
// the moment never starts: no frozen world, no respawn.
constexpr uintptr_t kCallTriggerMomentNis = 0x6424B3;
constexpr uintptr_t kTriggerMomentNis = 0x63BE40;     // thiscall, no args
constexpr uintptr_t kMomentNisTime = 0x90D9D4;        // float: when the last moment NIS was asked for
// Frame memory: eFrameMalloc's two 400 KB buffers (eInitEngine allocates them, the frame swaps
// them in eSwapFrameMallocBuffers after Present). Car parts, lights and matrices come from it.
constexpr uintptr_t kFrameMemSizes[] = {0x5009D2, 0x5009DC, 0x500A01, 0x500A12}; // imm32 0x64000 in eInitEngine
constexpr uint32_t kFrameMemGame = 0x64000;
constexpr uint32_t kFrameMemVr = 0x200000;
constexpr uintptr_t kFrameMemBuffer = 0x915128;  // FrameMemoryBuffer (null until eInitEngine)
constexpr uintptr_t kFrameMemLargest = 0x915FA0; // Eframelargest: most used in one frame (bytes)
constexpr uintptr_t kFrameMemMaxFail = 0x915F9C; // FrameMallocMaxFailAmount: most refused in one frame (bytes)

// eView (PC layout)
constexpr int kViewPlatInfo = 0x00;
constexpr int kViewActive = 0x08;
constexpr int kViewCamera = 0x40;
constexpr int kViewRenderTarget = 0x60;
// eRenderTarget
constexpr int kRtWidth = 0x14;
constexpr int kRtHeight = 0x18;
// eViewPlatInfo
constexpr int kPlatProjection = 0x40;
// Camera / CameraParams
constexpr int kCamMatrix = 0x00;
constexpr int kCamPosition = 0x40;
constexpr int kCamDirection = 0x50;
constexpr int kCamTarget = 0x60;
constexpr int kCamTargetDistance = 0xB0;
constexpr int kCamNearZ = 0xBC;  // used by sub_6CF400 unless its 2nd argument (a float) is non-zero
constexpr int kCamFov = 0xC4;
constexpr int kCameraParamsSize = 0xD4;

constexpr float kTwoPi = 6.28318530718f;
constexpr V3 kWorldUp = {0, 0, 1}; // NFSMW world space is Z-up (verified from live camera matrices)

typedef void(__fastcall* ViewSetupFn)(void* platInfo, void* edx, void* view, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
typedef void(__cdecl* RenderSceneFn)();
typedef void(__cdecl* TotaledCamFn)(void* player);
typedef bool(__cdecl* JumpCamsOnFn)();
typedef void(__fastcall* PlayIceFn)(void* ice, void* edx, const char* name, const char* track);
typedef void(__cdecl* SetActionFn)(int view, const char* action);

ViewSetupFn oViewSetup = (ViewSetupFn)kViewSetup;
RenderSceneFn oRenderScene = (RenderSceneFn)kRenderScene;

uint32_t s_setupArgs[4];
bool s_haveSetupArgs = false;
struct { uint32_t a[4]; bool have; } s_viewArgs[kNumViews];   // every view's sub_6CF400 arguments
bool s_stereoFrame = false;
bool s_stereoEnabled = true;
bool s_levelEnabled = true;
float s_tanX = 1, s_tanY = 1;

template <typename T> T& At(void* base, int offset) { return *(T*)((char*)base + offset); }

// Writes `opcode rel32` (0xE8 call / 0xE9 jmp) at `addr`, targeting `hook`.
void PatchRel32(uintptr_t addr, uint8_t opcode, void* hook) {
    uint8_t* p = (uint8_t*)addr;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = opcode;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)hook - (addr + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

// Another mod's hook of __getResolutionByIndex found at install (the widescreen fix returns the
// desktop or its ini resolution); it supplies the size whenever we don't override it.
typedef void(__stdcall* GetResolution_t)(int*, int*);
GetResolution_t s_prevGetResolution;

// Replaces the 6-entry resolution table with the headset's per-eye size.
void __stdcall hkGetResolution(int* w, int* h) {
    static const int kTable[6][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1280, 1024}, {1600, 1200}};
    if (s_prevGetResolution) {
        s_prevGetResolution(w, h);
    } else {
        int index = *(int*)kResolutionIndex;
        if (index < 0 || index > 5) index = 2;
        *w = kTable[index][0];
        *h = kTable[index][1];
    }

    const char* mode = g_cfg.resolution;
    int cw = 0, ch = 0;
    if (_stricmp(mode, "auto") == 0) {
        if (D3D_EnsureXr()) Xr_RecommendedSize(&cw, &ch);
        cw = (int)(cw * g_cfg.resolutionScale);
        ch = (int)(ch * g_cfg.resolutionScale);
    } else if (_stricmp(mode, "game") != 0) {
        sscanf(mode, "%dx%d", &cw, &ch);
    }
    if (cw >= 320 && ch >= 240 && g_cfg.forceWindowed) {
        *w = cw & ~1;
        *h = ch & ~1;
    }
    static int lastW, lastH;
    if (*w != lastW || *h != lastH) {
        Log("game: render resolution %dx%d (Resolution=%s)", *w, *h, mode);
        lastW = *w;
        lastH = *h;
    }
}

// sub_6CF400's first setup argument overrides the camera's near plane when non-zero: [VR] NearZ
// replaces it for the player view so the cockpit right in front of the eyes isn't clipped.
uint32_t NearArg(void* cam) {
    if (!(g_cfg.nearZ > 0.0f)) return s_setupArgs[0];
    static float logged = -1;
    if (g_cfg.nearZ != logged) {
        float game = s_setupArgs[0] ? *(const float*)&s_setupArgs[0] : cam ? At<float>(cam, kCamNearZ) : 0.0f;
        Log("game: near plane %.3f m (game's %.3f m)", g_cfg.nearZ, game);
        logged = g_cfg.nearZ;
    }
    uint32_t bits;
    memcpy(&bits, &g_cfg.nearZ, sizeof(bits));
    return bits;
}

int ViewIndex(const void* view) {
    uintptr_t v = (uintptr_t)view;
    if (v < kViews || v >= kViews + kNumViews * kViewStride || (v - kViews) % kViewStride) return -1;
    return (int)((v - kViews) / kViewStride);
}

void __fastcall hkViewSetup(void* platInfo, void* edx, void* view, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    int index = ViewIndex(view);
    if (index >= 0) {
        s_viewArgs[index].a[0] = a2;
        s_viewArgs[index].a[1] = a3;
        s_viewArgs[index].a[2] = a4;
        s_viewArgs[index].a[3] = a5;
        s_viewArgs[index].have = true;
    }
    if ((uintptr_t)view == kPlayerView) {
        s_setupArgs[0] = a2;
        s_setupArgs[1] = a3;
        s_setupArgs[2] = a4;
        s_setupArgs[3] = a5;
        s_haveSetupArgs = true;
    }
    oViewSetup(platInfo, edx, view, a2, a3, a4, a5);
}

// Moves the camera to an eye: V_eye = V_game * inverse(eyePoseInCameraSpace).
// The game's camera space is +x right, +y down, +z forward; OpenXR's is +x right,
// +y up, -z forward, so S = diag(1,-1,-1) converts between them.
// While the eyes render: the head-free seat camera position, and the eye camera's position.
bool s_envPosSet = false;
float s_envPos[4];
const float* s_eyePos = nullptr;

void ApplyEyeToCamera(void* cam, const uint8_t* savedParams, const EyeView& eye) {
    M4 V;
    memcpy(&V, savedParams + kCamMatrix, sizeof(V));

    float R[3][3];
    QToMat3(eye.relOrientation, R);
    const float sgn[3] = {1, -1, -1};
    M4 E = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) E.m[i][j] = sgn[i] * sgn[j] * R[i][j];
    V3 t = V3{eye.relPosition.x, -eye.relPosition.y, -eye.relPosition.z} * g_cfg.worldScale;
    for (int j = 0; j < 3; j++) E.m[3][j] = -(t.x * E.m[0][j] + t.y * E.m[1][j] + t.z * E.m[2][j]);
    E.m[3][3] = 1;

    M4 Ve = MMul(V, E);
    memcpy((char*)cam + kCamMatrix, &Ve, sizeof(Ve));

    // Keep the derived fields consistent, as Camera::SetCameraMatrix would.
    M4 W = MInvRigid(Ve);
    float* pos = &At<float>(cam, kCamPosition);
    float* dir = &At<float>(cam, kCamDirection);
    float* tgt = &At<float>(cam, kCamTarget);
    float len = sqrtf(W.m[2][0] * W.m[2][0] + W.m[2][1] * W.m[2][1] + W.m[2][2] * W.m[2][2]);
    if (len < 1e-6f) len = 1;
    float dist = At<float>(cam, kCamTargetDistance);
    for (int i = 0; i < 3; i++) {
        pos[i] = W.m[3][i];
        dir[i] = W.m[2][i] / len;
        tgt[i] = pos[i] + dir[i] * dist;
    }
}

// Rebuilds the player view's matrices with a symmetric FOV of tanX/tanY.
// sub_6CF400 derives: horizontal half-angle = fov/2, vertical half-angle =
// fov/2 * (rtH/rtW) * 1.15. We set the camera FOV and temporarily fake the render
// target dimensions so it produces exactly the angles we want, then read back the
// resulting projection.
// Sets a view up from `cam` with that FOV (the render target's size faked as above). sub_6CF400
// applies the 1.15 aspect factor to the player and glow views (1, 2, 4, 5) only.
void SetupViewFov(void* view, int index, void* cam, float tanX, float tanY, const uint32_t* args) {
    float hx = atanf(tanX), hy = atanf(tanY);
    int fov = (int)(2 * hx / kTwoPi * 65536.0f + 0.5f);
    At<uint16_t>(cam, kCamFov) = (uint16_t)std::min(fov, 65000);

    void* rt = At<void*>(view, kViewRenderTarget);
    int savedW = 0, savedH = 0;
    if (rt) {
        savedW = At<int>(rt, kRtWidth);
        savedH = At<int>(rt, kRtHeight);
        bool scaled = index == 1 || index == 2 || index == 4 || index == 5;
        float scale = scaled ? *(float*)kPlayerAspectScale : 1.0f;
        At<int>(rt, kRtWidth) = 100000;
        At<int>(rt, kRtHeight) = (int)(100000.0f * (hy / hx) / scale + 0.5f);
    }
    oViewSetup(At<void*>(view, kViewPlatInfo), nullptr, view, NearArg(cam), args[1], args[2], args[3]);
    if (rt) {
        At<int>(rt, kRtWidth) = savedW;
        At<int>(rt, kRtHeight) = savedH;
    }
}

// The glow and specular views that use the player's camera, active this frame, with arguments.
template <class Fn> void ForEachCompanion(void* cam, Fn fn) {
    for (int index : kCompanionViews) {
        void* view = (void*)(kViews + index * kViewStride);
        if (!s_viewArgs[index].have || !At<int8_t>(view, kViewActive) || At<void*>(view, kViewCamera) != cam) continue;
        fn(view, index, s_viewArgs[index].a);
    }
}

// While the eyes render: the eye's view matrix and the seat's, for WorldView.
bool s_stableView = false;
M4 s_eyeView, s_seatView;

typedef void*(__stdcall* MatrixMultiply_t)(void* out, const void* a, const void* b);

void* __stdcall hkWorldViewMultiply(void* out, const void* local, const void* view) {
    if (s_stableView && view && !memcmp(view, &s_eyeView, sizeof(s_eyeView))) view = &s_seatView;
    return ((MatrixMultiply_t)kMatrixMultiply)(out, local, view);
}

// The MIRROR material plain white while the side mirrors show the rear view, the game's otherwise
// (looked up again every second, as global data can be reloaded).
bool s_mirrorPatched = false;

void FixGameMirrors() {
    static DWORD last;
    static uint8_t* mat;
    static float original[30];
    static bool haveOriginal;
    if (GetTickCount() - last >= 1000 || !mat) {
        last = GetTickCount();
        uint8_t* m = ((uint8_t*(__cdecl*)(uint32_t))kGetLightMaterial)(kMirrorMaterialHash);
        mat = m && At<uint32_t>(m, kMatNameHash) == kMirrorMaterialHash ? m : nullptr;
        if (mat && !haveOriginal) {
            memcpy(original, mat + kMatData, sizeof(original));
            haveOriginal = true;
        }
    }
    if (!mat || !haveOriginal) return;
    float* f = (float*)(mat + kMatData);
    float want[30];
    memcpy(want, original, sizeof(want));
    if (g_cfg.gameMirrors && s_mirrorPatched) {
        for (int i = 0; i < 3; i++) want[kMatDiffMinR + i] = want[kMatDiffMaxR + i] = 1.0f;
        want[kMatSpecMinScale] = want[kMatSpecMaxScale] = 0.0f;
        want[kMatEnvMinScale] = want[kMatEnvMaxScale] = 0.0f;
    }
    if (!memcmp(want, f, sizeof(want))) return;
    Log("game: MIRROR material %s", want[kMatDiffMinR] > 0.5f ? "white (the rear view on the glass)" : "as the game's");
    memcpy(f, want, sizeof(want));
}

bool s_secondEye = false;

void __cdecl hkPreDisplay() {
    if (s_secondEye) return;
    ((void(__cdecl*)())kPreDisplay)();
}

bool s_mirrorPatchOk = false;

void WriteCode(uintptr_t at, const uint8_t* bytes, int n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, bytes, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

// The mirror camera on and the HUD mirror off while VR runs with Mirrors=1, the game's otherwise.
void UpdateMirrorPatch() {
    bool want = s_mirrorPatchOk && g_cfg.mirrors && Xr_Available();
    if (want == s_mirrorPatched) return;
    WriteCode(kMirrorCamOption, want ? kMirrorCamOn : kMirrorCamGame, 3);
    WriteCode(kRvmAllowed, want ? kRvmAllowedOn : kRvmAllowedGame, 3);
    s_mirrorPatched = want;
    Log("game: rear view %s", want ? "on the car's mirrors (HUD mirror hidden)" : "as the game's option");
}

// The texture whose surface the rear view renders into (the HUD may draw it directly).
void* RvmTargetTexture() { return D3D_TextureOfSurface(*(void**)(kRvmTarget + kTargetSurface)); }

void* TextureOf(uintptr_t slot) {
    uint8_t* info = *(uint8_t**)slot;
    uint8_t* plat = info ? *(uint8_t**)info : nullptr;
    return plat ? *(void**)(plat + 0x18) : nullptr;
}

// The HUD's mirror textures, for the d3d9 wrapper to leave out while the mirrors are on.
void UpdateHiddenTextures() {
    if (s_mirrorPatched) D3D_HideTextures(TextureOf(kRvmTexture), TextureOf(kRvmMaskTexture), RvmTargetTexture());
    else D3D_HideTextures(nullptr, nullptr, nullptr);
}

void* RvmSurface() {
    static int logged = -1;
    int active = *(uint8_t*)kRvmViewActive != 0;
    void* surface = *(void**)(kRvmTarget + kTargetSurface);
    if (active != logged) {
        logged = active;
        Log("game: rear view %s (surface %p)", active ? "rendering" : "not rendering", surface);
    }
    return active ? surface : nullptr;
}

// Before and after an eye's scene: the mirrors noted while it is submitted are drawn with their
// models (d3d9proxy); the rear view is copied afterwards for the next eye.
// Driving: in the world, the simulation running, not paused.
bool DrivingUnpaused() {
    if (!((bool(__cdecl*)())0x468380)()) return false;                  // GameFlow: in the world
    if (((int(__cdecl*)())0x6E8D20)() != 3) return false;              // Sim::GetState: running
    return !(*(void**)0x91CAE0 && ((bool(__cdecl*)(bool))0x525B20)(true));   // FEManager::ShouldPauseSimulation
}

// The steering wheel in front of the HUD (d3d9proxy): the seat (the game camera in the seat,
// without the head) as world axes, origin, right, up, forward, and the eye's view and projection.
void BeginWheelHud(void* view, const uint8_t* seatParams) {
    bool on = g_cfg.wheelOverHud > 0.5f && DrivingUnpaused();
    M4 V;
    memcpy(&V, seatParams + kCamMatrix, sizeof(V));
    M4 W = MInvRigid(V);   // rows: right, down, forward, position
    float seat[12] = {W.m[3][0], W.m[3][1], W.m[3][2], W.m[0][0], W.m[0][1], W.m[0][2],
                      -W.m[1][0], -W.m[1][1], -W.m[1][2], W.m[2][0], W.m[2][1], W.m[2][2]};
    void* plat = At<void*>(view, kViewPlatInfo);
    D3D_BeginWheelEye(on, seat, (const float*)((char*)plat + kPlatView), (const float*)((char*)plat + kPlatProjection));
}

void BeginMirrors(void* view) {
    if (!s_mirrorPatched) return;
    void* plat = At<void*>(view, kViewPlatInfo);
    D3D_BeginMirrorEye((const float*)((char*)plat + kPlatView), (const float*)((char*)plat + kPlatProjection),
                       g_cfg.mirrorBrightness);
}

void EndMirrors() {
    if (s_mirrorPatched) D3D_EndMirrorEye(RvmSurface());
}

void SetupPlayerView(void* view, void* cam, float tanX, float tanY) {
    SetupViewFov(view, 1, cam, tanX, tanY, s_setupArgs);
    ForEachCompanion(cam, [&](void* v, int index, const uint32_t* args) { SetupViewFov(v, index, cam, tanX, tanY, args); });
    void* plat = At<void*>(view, kViewPlatInfo);

    float p00 = At<float>(plat, kPlatProjection + 0x00);
    float p11 = At<float>(plat, kPlatProjection + 0x14);
    if (p00 != 0 && p11 != 0) {
        s_tanX = 1.0f / fabsf(p00);
        s_tanY = 1.0f / fabsf(p11);
    }
}

void __cdecl hkRenderScene() {
    s_stereoFrame = false;
    FixGameMirrors();
    UpdateMirrorPatch();
    UpdateHiddenTextures();
    void* view = (void*)kPlayerView;
    void* cam = At<void*>(view, kViewCamera);
    bool active = At<int8_t>(view, kViewActive) != 0;

    bool began = false;
    if (Xr_Available() && s_stereoEnabled && active && cam && s_haveSetupArgs) {
        double tw = D3D_ProfNow();
        began = Xr_BeginFrame();
        D3D_ProfAdd(PROF_WAITFRAME, D3D_ProfNow() - tw);
    }
    if (!began) {
        // Flat screen: the cockpit seat still applies to the single view.
        uint8_t saved[kCameraParamsSize];
        bool seat = false;
        if (active && cam && s_haveSetupArgs) {
            memcpy(saved, cam, sizeof(saved));
            M4 V;
            memcpy(&V, saved + kCamMatrix, sizeof(V));
            if (Cockpit_ApplySeat(V)) {
                uint8_t seated[kCameraParamsSize];
                memcpy(seated, saved, sizeof(seated));
                memcpy(seated + kCamMatrix, &V, sizeof(V));
                EyeView centre = {};
                centre.relOrientation.w = 1;
                ApplyEyeToCamera(cam, seated, centre);
                oViewSetup(At<void*>(view, kViewPlatInfo), nullptr, view,
                           NearArg(cam), s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
                ForEachCompanion(cam, [&](void* v, int, const uint32_t* a) {
                    oViewSetup(At<void*>(v, kViewPlatInfo), nullptr, v, NearArg(cam), a[1], a[2], a[3]);
                });
                seat = true;
            }
        }
        oRenderScene();
        if (seat) {
            memcpy(cam, saved, sizeof(saved));
            oViewSetup(At<void*>(view, kViewPlatInfo), nullptr, view,
                       s_setupArgs[0], s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
            ForEachCompanion(cam, [](void* v, int, const uint32_t* a) { oViewSetup(At<void*>(v, kViewPlatInfo), nullptr, v, a[0], a[1], a[2], a[3]); });
        }
        return;
    }

    const EyeView& L = Xr_Eye(0);
    const EyeView& R = Xr_Eye(1);
    float tanX = std::max(std::max(L.tanLeft, L.tanRight), std::max(R.tanLeft, R.tanRight)) * g_cfg.fovScale;
    float tanY = std::max(std::max(L.tanUp, L.tanDown), std::max(R.tanUp, R.tanDown)) * g_cfg.fovScale;

    uint8_t saved[kCameraParamsSize];
    memcpy(saved, cam, sizeof(saved));

    // The eyes are placed relative to this (optionally levelled) camera.
    uint8_t base[kCameraParamsSize];
    memcpy(base, saved, sizeof(base));
    {
        M4 V;
        memcpy(&V, saved + kCamMatrix, sizeof(V));
        if (Cockpit_ApplySeat(V)) memcpy(base + kCamMatrix, &V, sizeof(V));
        if (s_levelEnabled && (g_cfg.levelRoll > 0 || g_cfg.levelPitch > 0)) {
            V = LevelCamera(V, kWorldUp, g_cfg.levelRoll, g_cfg.levelPitch);
            memcpy(base + kCamMatrix, &V, sizeof(V));
        }
    }

    {
        M4 V;
        memcpy(&V, base + kCamMatrix, sizeof(V));
        M4 W = MInvRigid(V);
        s_envPos[0] = W.m[3][0];
        s_envPos[1] = W.m[3][1];
        s_envPos[2] = W.m[3][2];
        s_envPos[3] = 1.0f;
        s_eyePos = &At<float>(cam, kCamPosition);
        s_envPosSet = true;
    }

    double tEyes = D3D_ProfNow();
    D3D_BeginEyes();
    for (int eye = 0; eye < 2; eye++) {
        if (eye > 0) D3D_ResetForNextEye();
        ApplyEyeToCamera(cam, base, Xr_Eye(eye));
        SetupPlayerView(view, cam, tanX, tanY);
        if (g_cfg.stableReflections) {
            memcpy(&s_eyeView, (const char*)cam + kCamMatrix, sizeof(s_eyeView));
            memcpy(&s_seatView, base + kCamMatrix, sizeof(s_seatView));
            s_stableView = true;
        }
        Cockpit_BeginEye();
        BeginMirrors(view);
        BeginWheelHud(view, base);
        s_secondEye = eye > 0;
        int8_t sharedActive[sizeof(kSharedViews) / sizeof(kSharedViews[0])] = {};
        if (s_secondEye && g_cfg.sharedViewsOnce)
            for (size_t i = 0; i < sizeof(kSharedViews) / sizeof(kSharedViews[0]); i++) {
                int8_t& active = At<int8_t>((void*)(kViews + kSharedViews[i] * kViewStride), kViewActive);
                sharedActive[i] = active;
                active = 0;
            }
        oRenderScene();
        if (s_secondEye && g_cfg.sharedViewsOnce)
            for (size_t i = 0; i < sizeof(kSharedViews) / sizeof(kSharedViews[0]); i++)
                At<int8_t>((void*)(kViews + kSharedViews[i] * kViewStride), kViewActive) = sharedActive[i];
        s_secondEye = false;
        EndMirrors();
        D3D_EndWheelEye();
        if (eye == 0) D3D_CaptureEye(0);
    }
    s_envPosSet = false;
    s_stableView = false;

    memcpy(cam, saved, sizeof(saved));
    oViewSetup(At<void*>(view, kViewPlatInfo), nullptr, view,
               s_setupArgs[0], s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
    ForEachCompanion(cam, [](void* v, int, const uint32_t* a) { oViewSetup(At<void*>(v, kViewPlatInfo), nullptr, v, a[0], a[1], a[2], a[3]); });
    D3D_ProfAdd(PROF_EYES, D3D_ProfNow() - tEyes);
    D3D_BeginHudPhase();
    s_stereoFrame = true;
}

// A totaled car (drag race crash, or an engine blown by the H-shifter mod's engine wear) cuts
// to a third-person crash camera. In the headset that is a sudden jump out of the car, so it is
// skipped while VR is running and the view stays in the cockpit.
void __cdecl hkTotaledCam(void* player) {
    if (Xr_Available()) {
        Log("game: totaled camera skipped (VR)");
        return;
    }
    ((TotaledCamFn)kTotaledCam)(player);
}

// The other cinematic cameras cut away from the cockpit just the same, so they are off in VR too.
// Pursuit, jump and crash cams all check the "Jump Cams" option first; it reads as off.
bool __cdecl hkJumpCamsOn() {
    if (Xr_Available()) return false;
    return ((JumpCamsOnFn)kJumpCamsOn)();
}

// Pursuit breakers etc.: neither the ICE track nor the director's ICE action is started.
bool s_iceSkipped = false;

void __fastcall hkPlayIce(void* ice, void* edx, const char* name, const char* track) {
    s_iceSkipped = Xr_Available();
    if (s_iceSkipped) {
        Log("game: cinematic camera '%s' skipped (VR)", track ? track : "");
        return;
    }
    ((PlayIceFn)kPlayIce)(ice, edx, name, track);
}

void __cdecl hkIceAction(int view, const char* action) {
    if (s_iceSkipped) return;
    ((SetActionFn)kSetAction)(view, action);
}

void __fastcall hkSelectAction(uint8_t* director, void*) {
    float& timer = At<float>(director, kDirectorTrackCopTimer);
    if (Xr_Available() && At<int>(director, kDirectorView) == 1 && timer > 0.0f) {
        Log("game: pursuit breaker / cop camera skipped (VR)");
        timer = 0.0f;
    }
    ((void(__thiscall*)(void*))kSelectAction)(director);

    if (!Xr_Available() || At<int>(director, kDirectorView) != 1 || *(void**)kNisInstance) return;
    const char* action = At<const char*>(director, kDirectorActionName);
    const char* drive = (const char*)kActionDriveName;
    if (!action || !action[0] || action == drive || strcmp(action, drive) == 0 || strncmp(action, "CDActionDebug", 13) == 0)
        return;
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 2000) {
        lastLog = GetTickCount();
        Log("game: camera '%s' turned back to the driving camera (VR)", action);
    }
    struct StringKey { uint32_t hash64[2], hash32; const char* string; } key;
    ((void*(__thiscall*)(StringKey*, const char*))kStringKeyCtor)(&key, drive);
    ((void(__thiscall*)(void*, StringKey))kDirectorSetAction)(director, key);
    At<float>(director, kDirectorTrackCopTimer) = 0.0f;
}

// The busted cutscene without its viewfinder frame while VR runs: the scene is seen as it is.
void __cdecl hkNisOverlay(int id) {
    if (Xr_Available() && (id & 0x7F) == kNisOverlayBusted) {
        Log("game: busted overlay skipped (VR)");
        return;
    }
    ((void(__cdecl*)(int))kNisOverlay)(id);
}

// The event's scene (ETriggerMomentNIS::fSceneName at +8): pursuit breakers are the world moments
// (NIS\Scene_WM*: WMGasStation, WMBoat, WMDriveIn...). Story scenes (the first safehouse visit
// and the like) come the same way and must play, or the career falls back to the main menu.
bool PursuitBreakerScene(const char* scene) {
    if (!scene || IsBadStringPtrA(scene, 64)) return false;
    if (_strnicmp(scene, "Scene_", 6) == 0) scene += 6;
    return _strnicmp(scene, "WM", 2) == 0;
}

void __fastcall hkTriggerMomentNis(void* ev, void*) {
    float* stamp = (float*)kMomentNisTime;
    float saved = *stamp;
    const char* scene = ev ? *(const char**)((uint8_t*)ev + 8) : nullptr;
    bool skip = Xr_Available() && PursuitBreakerScene(scene);
    ((void(__thiscall*)(void*))kTriggerMomentNis)(ev);
    bool started = *stamp != saved;
    if (skip && started) *stamp = saved;
    if (started)
        Log("game: moment cutscene '%s' %s", scene && !IsBadStringPtrA(scene, 64) ? scene : "?",
            skip ? "skipped (VR, pursuit breaker)" : "plays");
}

struct ActionKey { uint32_t hash64[2], hash32; const char* string; };

// Director::SetAction for the player's view while VR runs: anything but the driving camera (or a
// debug camera) becomes the driving camera, except during an NIS.
void __fastcall hkDirectorSetAction(uint8_t* director, void*, ActionKey key) {
    auto set = (void(__thiscall*)(void*, ActionKey))kDirectorSetAction;
    const char* drive = (const char*)kActionDriveName;
    const char* action = key.string;
    if (Xr_Available() && At<int>(director, kDirectorView) == 1 && !*(void**)kNisInstance && action && action[0] &&
        action != drive && strcmp(action, drive) != 0 && strncmp(action, "CDActionDebug", 13) != 0) {
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 2000) {
            lastLog = GetTickCount();
            Log("game: camera '%s' refused, driving camera kept (VR)", action);
        }
        ActionKey driveKey;
        ((void*(__thiscall*)(ActionKey*, const char*))kStringKeyCtor)(&driveKey, drive);
        At<float>(director, kDirectorTrackCopTimer) = 0.0f;
        set(director, driveKey);
        return;
    }
    set(director, key);
}

// Swaps in the seat position when `pos` is the eye camera's (not some other point).
const float* EnvPos(const float* pos) {
    if (!s_envPosSet || !pos || !s_eyePos) return pos;
    float dx = pos[0] - s_eyePos[0], dy = pos[1] - s_eyePos[1], dz = pos[2] - s_eyePos[2];
    return dx * dx + dy * dy + dz * dz < 0.01f ? s_envPos : pos;
}

int __cdecl hkCloneLightContext(void* ctx, void* local, void* worldView, const float* pos, void* view, void* old) {
    return ((int(__cdecl*)(void*, void*, void*, const float*, void*, void*))kCloneLightContext)(
        ctx, local, worldView, EnvPos(pos), view, old);
}

int __cdecl hkSetupLightContext(void* ctx, void* rig, void* local, void* worldView, const float* pos, void* view) {
    return ((int(__cdecl*)(void*, void*, void*, void*, const float*, void*))kSetupLightContext)(
        ctx, rig, local, worldView, EnvPos(pos), view);
}

void __cdecl hkScreenRain(int arg) {
    if (Xr_Available() && !g_cfg.screenRain) return;
    ((void(__cdecl*)(int))kScreenRain)(arg);
}

// Retargets the call at `site` to `hook` if it still calls `target`.
void PatchCall(uintptr_t site, uintptr_t target, void* hook, const char* what) {
    const uint8_t* p = (const uint8_t*)site;
    if (p[0] == 0xE8 && site + 5 + *(const int32_t*)(p + 1) == target) PatchRel32(site, 0xE8, hook);
    else Log("game: unexpected code at the %s call (0x%X), not patched", what, (unsigned)site);
}

// Both eyes render within one frame, so everything ___eDisplayFrame takes from frame memory is
// taken twice before the buffers swap. In a busy scene (traffic, cops, smoke) the second eye ran
// out: eFrameMalloc returns null, car drawing is skipped, and unchecked allocations can crash.
// This is a possible cause of the reported right-eye failure, independent of the XR runtime.
// Reserve 2 MB per buffer before allocation; never rewind the allocator between the eyes, as
// queued GPU work may still use matrices and lights allocated for the first eye.
void EnlargeFrameMemory() {
    if (*(void**)kFrameMemBuffer) {
        Log("game: frame memory already allocated, left at %u KB", kFrameMemGame / 1024);
        return;
    }
    for (uintptr_t a : kFrameMemSizes)
        if (*(const uint32_t*)a != kFrameMemGame) {
            Log("game: unexpected code at the frame memory size (0x%X), not changed", (unsigned)a);
            return;
        }
    for (uintptr_t a : kFrameMemSizes) {
        DWORD old;
        VirtualProtect((void*)a, 4, PAGE_EXECUTE_READWRITE, &old);
        *(uint32_t*)a = kFrameMemVr;
        VirtualProtect((void*)a, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)a, 4);
    }
    Log("game: frame memory %u KB -> %u KB per buffer", kFrameMemGame / 1024, kFrameMemVr / 1024);
}

} // namespace

// Logs frame memory use when it reaches a new high (and any frame that ran out).
void Game_CheckFrameMemory() {
    static int logged;
    static uint32_t loggedFail;
    int largest = *(int*)kFrameMemLargest;
    uint32_t fail = *(uint32_t*)kFrameMemMaxFail;
    if (largest > logged + 32 * 1024) {
        logged = largest;
        Log("game: frame memory peak %d KB", largest / 1024);
    }
    if (fail > loggedFail) {
        loggedFail = fail;
        Log("game: WARNING frame memory ran out (%u KB short in one frame): some objects were not drawn", fail / 1024);
    }
}

bool Game_ValidateHooks() {
    const uint8_t* base = (const uint8_t*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    if ((uintptr_t)base != 0x400000 || nt->OptionalHeader.SizeOfImage < 0x5C7000) {
        Log("game: unsupported host image, VR inactive");
        return false;
    }
    for (auto site : {std::make_pair(kCallViewSetup, kViewSetup), std::make_pair(kCallRenderScene, kRenderScene)}) {
        const uint8_t* p = (const uint8_t*)site.first;
        if (p[0] != 0xE8 || site.first + 5 + *(const int32_t*)(p + 1) != site.second) {
            Log("game: incompatible render hook at 0x%X, VR inactive", (unsigned)site.first);
            return false;
        }
    }
    const uint8_t* res = (const uint8_t*)kGetResolution;
    // Preserve the widescreen fix's existing resolution detour, provided it points to code.
    if (res[0] == 0xE9) {
        uintptr_t target = kGetResolution + 5 + *(const int32_t*)(res + 1);
        MEMORY_BASIC_INFORMATION mem = {};
        if (target == (uintptr_t)&hkGetResolution ||
            !VirtualQuery((const void*)target, &mem, sizeof(mem)) || mem.State != MEM_COMMIT ||
            (mem.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mem.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            Log("game: invalid resolution detour, VR inactive");
            return false;
        }
    } else {
        static const uint8_t expected[] = {0xA1, 0x1C, 0x18, 0x90, 0x00, 0x83, 0xF8, 0x05};
        if (memcmp(res, expected, sizeof(expected)) != 0) {
            Log("game: incompatible resolution function, VR inactive");
            return false;
        }
    }
    return true;
}

bool Game_InstallHooks() {
    s_mirrorPatchOk = !memcmp((void*)kMirrorCamOption, kMirrorCamGame, 3) && !memcmp((void*)kRvmAllowed, kRvmAllowedGame, 3);
    if (!s_mirrorPatchOk) Log("game: unexpected rear view code, side mirrors off");
    if (!Game_ValidateHooks()) return false;
    EnlargeFrameMemory();
    PatchRel32(kCallViewSetup, 0xE8, (void*)&hkViewSetup);
    PatchRel32(kCallRenderScene, 0xE8, (void*)&hkRenderScene);
    const uint8_t* res = (const uint8_t*)kGetResolution;
    if (res[0] == 0xE9) {
        s_prevGetResolution = (GetResolution_t)(kGetResolution + 5 + *(const int32_t*)(res + 1));
        Log("game: resolution function already hooked by another mod, chained");
    }
    PatchRel32(kGetResolution, 0xE9, (void*)&hkGetResolution);
    static const uint8_t kTotaledCall[5] = {0xE8, 0xC4, 0x8C, 0xE4, 0xFF};
    if (memcmp((void*)kCallTotaledCam, kTotaledCall, 5) == 0) PatchRel32(kCallTotaledCam, 0xE8, (void*)&hkTotaledCam);
    else Log("game: unexpected code at the totaled camera call, not patched");
    for (uintptr_t site : kCallsJumpCamsOn) PatchCall(site, kJumpCamsOn, (void*)&hkJumpCamsOn, "jump cams option");
    PatchCall(kCallPlayIce, kPlayIce, (void*)&hkPlayIce, "cinematic camera");
    PatchCall(kCallIceAction, kSetAction, (void*)&hkIceAction, "cinematic camera action");
    PatchCall(kCallSelectAction, kSelectAction, (void*)&hkSelectAction, "camera director");
    for (uintptr_t site : kCallsDirectorSetAction) PatchCall(site, kDirectorSetAction, (void*)&hkDirectorSetAction, "camera director action");
    PatchCall(kCallNisOverlay, kNisOverlay, (void*)&hkNisOverlay, "busted overlay");
    PatchCall(kCallTriggerMomentNis, kTriggerMomentNis, (void*)&hkTriggerMomentNis, "pursuit breaker cutscene");
    PatchCall(kCallScreenRain, kScreenRain, (void*)&hkScreenRain, "raindrops on the screen");
    PatchCall(kCallWorldView, kMatrixMultiply, (void*)&hkWorldViewMultiply, "reflections (WorldView)");
    PatchCall(kCallPreDisplay, kPreDisplay, (void*)&hkPreDisplay, "per-frame display updates");
    for (uintptr_t site : kCallsCloneLightContext)
        PatchCall(site, kCloneLightContext, (void*)&hkCloneLightContext, "car reflections");
    for (uintptr_t site : kCallsSetupLightContext)
        PatchCall(site, kSetupLightContext, (void*)&hkSetupLightContext, "world reflections");
    Log("game: hooks installed");
    return true;
}

bool Game_StereoThisFrame() { return s_stereoFrame; }

void Game_RenderedFov(float* tanX, float* tanY) {
    *tanX = s_tanX;
    *tanY = s_tanY;
}

void Game_EndOfFrame() {
    s_stereoFrame = false;
    Game_CheckFrameMemory();
    Cockpit_EndFrame();
}

void Game_ToggleLevel() {
    s_levelEnabled = !s_levelEnabled;
    Log("game: level horizon %s", s_levelEnabled ? "on" : "off");
}

void Game_ToggleStereo() {
    s_stereoEnabled = !s_stereoEnabled;
    Log("game: stereo %s", s_stereoEnabled ? "on" : "off");
}
