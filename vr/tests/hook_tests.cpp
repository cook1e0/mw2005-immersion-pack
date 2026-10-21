// Required-hook validation uses a synthetic v1.3 address layout and never launches the game.
#include "../src/game.cpp"
#include <assert.h>
Config g_cfg;
volatile uint8_t gameAddressPadding[0x600000];
void Log(const char*, ...) {}
bool D3D_EnsureXr() { return false; }
void D3D_BeginEyes() {}
void D3D_EyePassBegin(int) {}
void D3D_EyeRenderProfile(int, double) {}
void D3D_ResetForNextEye() {}
void D3D_CaptureEye(int) {}
void D3D_BeginHudPhase() {}
void D3D_DropDraws(bool) {}
void D3D_ProfUpdateDone() {}
bool Xr_Available() { return false; }
bool Xr_BeginFrame() { return false; }
void Xr_RecommendedSize(int* w, int* h) { *w = *h = 0; }
const EyeView& Xr_Eye(int) { static EyeView e; return e; }
bool Cockpit_ApplySeat(M4&) { return false; }
void Cockpit_EndFrame() {}
void Cockpit_BeginEye() {}
bool Cockpit_IsPlayerCarMatrix(const void*) { return false; }
int Cockpit_PlayerCarMatrices() { return 0; }
void Cockpit_SetGaze(const float*, const float*) {}
void Cockpit_EndEye() {}
void D3D_BeginMirrorEye(const float*, const float*, float) {}
void D3D_AddMirror(void*, const float*, int, const float*, int) {}
void D3D_RegisterMirrorGlass(void*, int, int, const float*, const uint16_t*, int) {}
void D3D_SetGlassCorners(void*, int, const float*) {}
void D3D_AddMirrorGlassPart(void*, int, const float*, const uint16_t*, int) {}
void D3D_EndMirrorEye(void*) {}
void D3D_BeginWheelEye(bool, const float*, const float*, const float*) {}
void D3D_EndWheelEye() {}
void D3D_EndDashEye() {}
void D3D_BeginDashEye(bool, const float*, const float*) {}
double D3D_ProfNow() { return 0; }
void D3D_ProfAdd(int, double) {}
void D3D_HideTextures(void*, void*, void*) {}
void* D3D_TextureOfSurface(void*) { return nullptr; }
static void __stdcall Resolution(int* w, int* h) { *w = 1920; *h = 1080; }
static void Call(uintptr_t site, uintptr_t target) {
    *(uint8_t*)site = 0xE8;
    *(int32_t*)(site + 1) = (int32_t)(target - site - 5);
}
int main() {
    static_assert(sizeof(void*) == 4, "test game layout");
    assert((uintptr_t)gameAddressPadding < kGetResolution &&
           (uintptr_t)gameAddressPadding + sizeof(gameAddressPadding) > kPlayerView);
    assert(!IsSharedViewForBothEyes(0));
    assert(!IsSharedViewForBothEyes(1));
    assert(IsSharedViewForBothEyes(3));
    assert(!IsSharedViewForBothEyes(15));
    assert(IsSharedViewForBothEyes(18));
    assert(IsSharedViewForBothEyes(23));
    // Environment map faces in turn: every face refreshed once per 6 / N frames, N a frame; only
    // eye-independent views are ever skipped; the rear view every RearViewEvery'th frame.
    for (int n = 1; n <= 6; n++) {
        g_cfg.envMapFacesPerFrame = n;
        g_cfg.rearViewEvery = 2;
        int refreshed[kEnvFaces] = {}, rearDrawn = 0;
        for (int f = 0; f < 12; f++) {
            PickViewsThisFrame();
            int faces = 0;
            for (int i = 0; i < kNumViews; i++)
                if (s_viewSkipped[i]) assert(IsSharedViewForBothEyes(i));
            for (int i = 0; i < kEnvFaces; i++)
                if (!s_viewSkipped[kFirstEnvFace + i]) faces++, refreshed[i]++;
            assert(faces == n);
            rearDrawn += !s_viewSkipped[kRearView];
        }
        for (int i = 0; i < kEnvFaces; i++) assert(refreshed[i] == 12 * n / kEnvFaces);
        assert(rearDrawn == 6);
    }
    g_cfg = Config();
    // Depth split: a D3D perspective projection (near 0.3, far 3000) times a view; the world ends up
    // in [kCarDepth, 1] with its planes kept, the car's variant in [0, kCarDepth] with NearZ.
    {
        g_cfg.nearZ = 0.09f;
        float n = 0.3f, f = 3000.0f;
        M4 P = {};
        P.m[0][0] = 1.2f; P.m[1][1] = 1.1f;
        P.m[2][2] = f / (f - n); P.m[2][3] = 1.0f; P.m[3][2] = -n * f / (f - n);
        M4 V = {};   // a rotation about y and a translation
        V.m[0][0] = 0.8f; V.m[0][2] = -0.6f; V.m[1][1] = 1.0f; V.m[2][0] = 0.6f; V.m[2][2] = 0.8f;
        V.m[3][0] = 5.0f; V.m[3][1] = -2.0f; V.m[3][2] = 7.0f; V.m[3][3] = 1.0f;
        M4 VP = MMul(V, P);
        assert(ZColumnIs(P, P.m[2][2], P.m[3][2]) && ZColumnIs(VP, P.m[2][2], P.m[3][2]));
        M4 car = CarDepthVariant(VP), world = VP;
        WorldDepthRemap(world);
        auto depthAt = [&](const M4& m, float d) {   // z/w of a world point at view depth d
            // world point whose view-space position is (0.1, 0.2, d): solve with the inverse view
            M4 W = MInvRigid(V);
            float x = 0.1f, y = 0.2f, p[4];
            for (int j = 0; j < 4; j++) p[j] = x * W.m[0][j] + y * W.m[1][j] + d * W.m[2][j] + W.m[3][j];
            float z = 0, w = 0;
            for (int i = 0; i < 4; i++) z += p[i] * m.m[i][2], w += p[i] * m.m[i][3];
            return z / w;
        };
        assert(fabsf(depthAt(world, n) - kCarDepth) < 1e-4f);
        assert(fabsf(depthAt(world, f) - 1.0f) < 1e-4f);
        assert(fabsf(depthAt(car, g_cfg.nearZ)) < 1e-4f);
        assert(fabsf(depthAt(car, kCarFarZ) - kCarDepth) < 1e-4f);
        assert(depthAt(car, 2.0f) < depthAt(world, n));
        g_cfg = Config();
    }
    // Shared shadow map guard: identical states pass, a 3 cm eye offset in a shadow matrix does not,
    // a NaN on one side only does not.
    {
        static ShadowState a, b;
        memset(&a, 0, sizeof(a));
        for (int i = 0; i < 16; i++) a.tex[i] = 0.1f * i;
        a.plat[12] = 1234.5f;
        b = a;
        assert(ShadowStateDiff(a, b) == 0.0f);
        b.plat[12] += 0.03f;
        int at = -2;
        assert(ShadowStateDiff(a, b, &at) > 1e-5f && at == 12);
        char what[64];
        DescribeShadowField(at, what, sizeof(what));
        assert(strstr(what, "view [3][0]"));
        b = a;
        b.tex[3] += 0.001f;
        assert(ShadowStateDiff(a, b) > 1e-4f);
        b = a;
        float nan = 0.0f;
        nan = nan / nan;
        b.light[1] = nan;
        assert(ShadowStateDiff(a, b) > 1.0f);
    }
    uint8_t flares[3 * kFlareSize] = {};
    uint32_t exclusions[3] = {4, 5, 6};
    flares[kFlareTypeOffset] = 0x15;
    flares[kFlareSize + kFlareTypeOffset] = 0x03;
    flares[2 * kFlareSize + kFlareTypeOffset] = 0x15;
    assert(RemoveSunLensFlares(flares, exclusions, 3) == 1);
    assert(flares[kFlareTypeOffset] == 0x03 && exclusions[0] == 5);
    Call(kCallViewSetup, kViewSetup); Call(kCallRenderScene, kRenderScene);
    const uint8_t original[] = {0xA1, 0x1C, 0x18, 0x90, 0x00, 0x83, 0xF8, 0x05};
    memcpy((void*)kGetResolution, original, sizeof(original));
    assert(Game_ValidateHooks());
    *(uint8_t*)kCallRenderScene = 0x90;
    uint8_t saved[5]; memcpy(saved, (void*)kCallViewSetup, sizeof(saved));
    assert(!Game_InstallHooks());
    assert(memcmp(saved, (void*)kCallViewSetup, sizeof(saved)) == 0);
    assert(memcmp(original, (void*)kGetResolution, sizeof(original)) == 0);
    Call(kCallRenderScene, kRenderScene);
    *(uint8_t*)kGetResolution = 0xE9;
    *(int32_t*)(kGetResolution + 1) = (int32_t)((uintptr_t)&Resolution - kGetResolution - 5);
    assert(Game_ValidateHooks());
    s_prevGetResolution = Resolution;
    strcpy(g_cfg.resolution, "game");
    int w, h; hkGetResolution(&w, &h);
    assert(w == 1920 && h == 1080);
    *(int32_t*)(kGetResolution + 1) = (int32_t)(1 - kGetResolution - 5);
    assert(!Game_ValidateHooks());
    memcpy((void*)kGetResolution, original, sizeof(original));
    *(uint8_t*)kGetResolution = 0x90;
    assert(!Game_ValidateHooks());
    puts("VR hook validation tests passed");
}
