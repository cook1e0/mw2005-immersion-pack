// Required-hook validation uses a synthetic v1.3 address layout and never launches the game.
#include "../src/game.cpp"
#include <assert.h>
Config g_cfg;
volatile uint8_t gameAddressPadding[0x600000];
void Log(const char*, ...) {}
bool D3D_EnsureXr() { return false; }
void D3D_BeginEyes() {}
void D3D_ResetForNextEye() {}
void D3D_CaptureEye(int) {}
void D3D_BeginHudPhase() {}
bool Xr_Available() { return false; }
bool Xr_BeginFrame() { return false; }
void Xr_RecommendedSize(int* w, int* h) { *w = *h = 0; }
const EyeView& Xr_Eye(int) { static EyeView e; return e; }
bool Cockpit_ApplySeat(M4&) { return false; }
void Cockpit_EndFrame() {}
void Cockpit_BeginEye() {}
void D3D_BeginMirrorEye(const float*, const float*, float) {}
void D3D_AddMirror(void*, const float*, int, const float*, int) {}
void D3D_RegisterMirrorGlass(void*, int, int, const float*, const uint16_t*, int) {}
void D3D_SetGlassCorners(void*, int, const float*) {}
void D3D_AddMirrorGlassPart(void*, int, const float*, const uint16_t*, int) {}
void D3D_EndMirrorEye(void*) {}
void D3D_BeginWheelEye(bool, const float*, const float*, const float*) {}
void D3D_EndWheelEye() {}
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
