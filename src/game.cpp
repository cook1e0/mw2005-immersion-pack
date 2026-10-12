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
constexpr uintptr_t kPlayerAspectScale = 0x8AF994; // float 1.15 applied to player views' aspect

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
constexpr int kCamFov = 0xC4;
constexpr int kCameraParamsSize = 0xD4;

constexpr float kTwoPi = 6.28318530718f;
constexpr V3 kWorldUp = {0, 0, 1}; // NFSMW world space is Z-up (verified from live camera matrices)

typedef void(__fastcall* ViewSetupFn)(void* platInfo, void* edx, void* view, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
typedef void(__cdecl* RenderSceneFn)();

ViewSetupFn oViewSetup = (ViewSetupFn)kViewSetup;
RenderSceneFn oRenderScene = (RenderSceneFn)kRenderScene;

uint32_t s_setupArgs[4];
bool s_haveSetupArgs = false;
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

// Replaces the 6-entry resolution table with the headset's per-eye size.
void __stdcall hkGetResolution(int* w, int* h) {
    static const int kTable[6][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1280, 1024}, {1600, 1200}};
    int index = *(int*)kResolutionIndex;
    if (index < 0 || index > 5) index = 2;
    *w = kTable[index][0];
    *h = kTable[index][1];

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

void __fastcall hkViewSetup(void* platInfo, void* edx, void* view, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
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
void SetupPlayerView(void* view, void* cam, float tanX, float tanY) {
    float hx = atanf(tanX), hy = atanf(tanY);
    int fov = (int)(2 * hx / kTwoPi * 65536.0f + 0.5f);
    At<uint16_t>(cam, kCamFov) = (uint16_t)std::min(fov, 65000);

    void* rt = At<void*>(view, kViewRenderTarget);
    int savedW = 0, savedH = 0;
    if (rt) {
        savedW = At<int>(rt, kRtWidth);
        savedH = At<int>(rt, kRtHeight);
        float scale = *(float*)kPlayerAspectScale;
        At<int>(rt, kRtWidth) = 100000;
        At<int>(rt, kRtHeight) = (int)(100000.0f * (hy / hx) / scale + 0.5f);
    }
    void* plat = At<void*>(view, kViewPlatInfo);
    oViewSetup(plat, nullptr, view, s_setupArgs[0], s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
    if (rt) {
        At<int>(rt, kRtWidth) = savedW;
        At<int>(rt, kRtHeight) = savedH;
    }

    float p00 = At<float>(plat, kPlatProjection + 0x00);
    float p11 = At<float>(plat, kPlatProjection + 0x14);
    if (p00 != 0 && p11 != 0) {
        s_tanX = 1.0f / fabsf(p00);
        s_tanY = 1.0f / fabsf(p11);
    }
}

void __cdecl hkRenderScene() {
    s_stereoFrame = false;
    void* view = (void*)kPlayerView;
    void* cam = At<void*>(view, kViewCamera);
    bool active = At<int8_t>(view, kViewActive) != 0;

    if (!Xr_Available() || !s_stereoEnabled || !active || !cam || !s_haveSetupArgs || !Xr_BeginFrame()) {
        oRenderScene();
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
        if (s_levelEnabled && (g_cfg.levelRoll > 0 || g_cfg.levelPitch > 0)) {
            V = LevelCamera(V, kWorldUp, g_cfg.levelRoll, g_cfg.levelPitch);
            memcpy(base + kCamMatrix, &V, sizeof(V));
        }
    }

    D3D_BeginEyes();
    for (int eye = 0; eye < 2; eye++) {
        if (eye > 0) D3D_ResetForNextEye();
        ApplyEyeToCamera(cam, base, Xr_Eye(eye));
        SetupPlayerView(view, cam, tanX, tanY);
        oRenderScene();
        if (eye == 0) D3D_CaptureEye(0);
    }

    memcpy(cam, saved, sizeof(saved));
    D3D_BeginHudPhase();
    s_stereoFrame = true;
}

} // namespace

void Game_InstallHooks() {
    PatchRel32(kCallViewSetup, 0xE8, (void*)&hkViewSetup);
    PatchRel32(kCallRenderScene, 0xE8, (void*)&hkRenderScene);
    PatchRel32(kGetResolution, 0xE9, (void*)&hkGetResolution);
    Log("game: hooks installed");
}

bool Game_StereoThisFrame() { return s_stereoFrame; }

void Game_RenderedFov(float* tanX, float* tanY) {
    *tanX = s_tanX;
    *tanY = s_tanY;
}

void Game_EndOfFrame() { s_stereoFrame = false; }

void Game_ToggleLevel() {
    s_levelEnabled = !s_levelEnabled;
    Log("game: level horizon %s", s_levelEnabled ? "on" : "off");
}

void Game_ToggleStereo() {
    s_stereoEnabled = !s_stereoEnabled;
    Log("game: stereo %s", s_stereoEnabled ? "on" : "off");
}
