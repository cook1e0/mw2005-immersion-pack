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
    oViewSetup(plat, nullptr, view, NearArg(cam), s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
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
                seat = true;
            }
        }
        oRenderScene();
        if (seat) {
            memcpy(cam, saved, sizeof(saved));
            oViewSetup(At<void*>(view, kViewPlatInfo), nullptr, view,
                       s_setupArgs[0], s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
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

    D3D_BeginEyes();
    for (int eye = 0; eye < 2; eye++) {
        if (eye > 0) D3D_ResetForNextEye();
        ApplyEyeToCamera(cam, base, Xr_Eye(eye));
        SetupPlayerView(view, cam, tanX, tanY);
        oRenderScene();
        if (eye == 0) D3D_CaptureEye(0);
    }

    memcpy(cam, saved, sizeof(saved));
    oViewSetup(At<void*>(view, kViewPlatInfo), nullptr, view,
               s_setupArgs[0], s_setupArgs[1], s_setupArgs[2], s_setupArgs[3]);
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
