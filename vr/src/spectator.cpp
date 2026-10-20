#include "spectator.h"

#include <d3dcompiler.h>
#include <string.h>

namespace {

SpectatorMode s_mode = SpectatorMode::Smooth;
bool s_modeLoaded;
Quat s_smoothed = {0, 0, 0, 1};
LARGE_INTEGER s_lastTime;
IDirect3DPixelShader9* s_reproject;
bool s_shaderFailed;
IDirect3DStateBlock9* s_state;

// Reprojects an eye image into a virtual camera. TEXCOORD0 carries the output
// pixel's NDC; c0..c2 rotate a spectator-space ray into eye space (OpenXR axes,
// -z forward); c3 = (spectator tanX, spectator tanY, 1/eye tanX, 1/eye tanY).
const char* kReprojectHlsl = R"(
sampler2D src : register(s0);
float4 m0 : register(c0);
float4 m1 : register(c1);
float4 m2 : register(c2);
float4 tans : register(c3);
float4 main(float2 ndc : TEXCOORD0) : COLOR {
    float3 ds = float3(ndc.x * tans.x, ndc.y * tans.y, -1);
    float3 de = float3(dot(m0.xyz, ds), dot(m1.xyz, ds), dot(m2.xyz, ds));
    float w = -de.z;
    float2 p = de.xy / max(w, 0.0001) * tans.zw;
    float inside = step(0.0001, w) * step(abs(p.x), 1) * step(abs(p.y), 1);
    float2 uv = float2(0.5 + 0.5 * p.x, 0.5 - 0.5 * p.y);
    return float4(tex2D(src, uv).rgb * inside, 1);
}
)";

void LoadMode() {
    if (s_modeLoaded) return;
    s_modeLoaded = true;
    if (_stricmp(g_cfg.spectatorMode, "eye") == 0) s_mode = SpectatorMode::Eye;
    else if (_stricmp(g_cfg.spectatorMode, "sbs") == 0) s_mode = SpectatorMode::SideBySide;
    else s_mode = SpectatorMode::Smooth;
}

bool EnsureShader(IDirect3DDevice9* dev) {
    if (s_reproject || s_shaderFailed) return s_reproject != nullptr;
    s_shaderFailed = true;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) return false;
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(compile(kReprojectHlsl, strlen(kReprojectHlsl), "spectator", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &err))) {
        Log("spectator: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &s_reproject);
    code->Release();
    s_shaderFailed = s_reproject == nullptr;
    return s_reproject != nullptr;
}

// Draws `tex` into the pixel rectangle [x0,x1)x[y0,y1). TEXCOORD0 is the texture UV,
// or the rectangle's NDC (-1..1, +y up) when `ndcCoords` is set (for the reprojection shader).
void DrawRect(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float x0, float y0, float x1, float y1, bool ndcCoords) {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (ndcCoords) {
        u0 = -1, u1 = 1;
        v0 = 1, v1 = -1;
    }
    struct V { float x, y, z, rhw, u, v; } q[4] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, u0, v0},
                                                  {x1 - 0.5f, y0 - 0.5f, 0, 1, u1, v0},
                                                  {x0 - 0.5f, y1 - 0.5f, 0, 1, u0, v1},
                                                  {x1 - 0.5f, y1 - 0.5f, 0, 1, u1, v1}};
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

// Saves the game's state, targets the back buffer and sets up plain textured drawing.
bool BeginDraw(IDirect3DDevice9* dev, IDirect3DSurface9** prevRT) {
    if (!s_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &s_state))) return false;
    s_state->Capture();
    dev->GetRenderTarget(0, prevRT);
    IDirect3DSurface9* bb = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    dev->SetRenderTarget(0, bb);
    if (bb) bb->Release();
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (int i = 1; i < 8; i++) dev->SetTexture(i, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    return SUCCEEDED(dev->BeginScene());
}

void EndDraw(IDirect3DDevice9* dev, IDirect3DSurface9* prevRT) {
    dev->EndScene();
    s_state->Apply();
    dev->SetRenderTarget(0, prevRT);
    if (prevRT) prevRT->Release();
}

void SetBlend(IDirect3DDevice9* dev, bool on) {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, on);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

float Seconds() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = s_lastTime.QuadPart ? (float)(now.QuadPart - s_lastTime.QuadPart) / freq.QuadPart : 0.0f;
    s_lastTime = now;
    return dt > 0.25f ? 0.25f : dt;
}

} // namespace

SpectatorMode Spectator_Mode() {
    LoadMode();
    return s_mode;
}

void Spectator_CycleMode() {
    LoadMode();
    s_mode = s_mode == SpectatorMode::Smooth ? SpectatorMode::SideBySide
           : s_mode == SpectatorMode::SideBySide ? SpectatorMode::Eye
                                                 : SpectatorMode::Smooth;
    Log("spectator: mode %s", s_mode == SpectatorMode::Smooth ? "smooth" : s_mode == SpectatorMode::SideBySide ? "sbs" : "eye");
}

// Aspect of one spectator view: configured, or the headset's own (tanX/tanY).
static float s_viewAspect = 1.0f;
static bool s_resize;

static float ViewAspect(float hmdTanX, float hmdTanY) {
    if (g_cfg.spectatorAspect > 0) return g_cfg.spectatorAspect;
    return hmdTanX > 0 && hmdTanY > 0 ? hmdTanX / hmdTanY : 1.0f;
}

float Spectator_WindowAspect(int bbWidth, int bbHeight) {
    switch (Spectator_Mode()) {
    case SpectatorMode::Smooth: return s_viewAspect;
    case SpectatorMode::SideBySide: return 2 * s_viewAspect;
    default: return bbHeight ? (float)bbWidth / bbHeight : 1.0f;
    }
}

bool Spectator_ConsumeResize() {
    bool r = s_resize;
    s_resize = false;
    return r;
}

void Spectator_BlendFullscreen(IDirect3DDevice9* dev, IDirect3DTexture9* tex, int bbWidth, int bbHeight) {
    IDirect3DSurface9* prev = nullptr;
    if (!BeginDraw(dev, &prev)) {
        if (s_state) s_state->Apply();
        if (prev) prev->Release();
        return;
    }
    SetBlend(dev, true);
    DrawRect(dev, tex, 0, 0, (float)bbWidth, (float)bbHeight, false);
    EndDraw(dev, prev);
}

// Draws a flat panel (the HUD or the menu screen) the way the headset shows it: a
// quad at `center` (origin space, facing the viewer) projected through the
// spectator camera. Pre-transformed vertices with rhw = 1/w give perspective-correct texturing.
static void DrawPanel(IDirect3DDevice9* dev, IDirect3DTexture9* tex, V3 center, float width, float height, V3 camPos,
                      Quat camRot, float tanX, float tanY, float vx0, float vx1, float H) {
    const V3 corners[4] = {{-0.5f, 0.5f, 0}, {0.5f, 0.5f, 0}, {-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}};
    const float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    struct V { float x, y, z, rhw, u, v; } q[4];
    Quat inv = QConj(camRot);
    for (int i = 0; i < 4; i++) {
        V3 p = {center.x + corners[i].x * width, center.y + corners[i].y * height, center.z};
        V3 c = QRotate(inv, p - camPos);
        float w = -c.z;
        if (w < 0.05f) return; // panel partly behind the camera: skip rather than draw garbage
        float xn = c.x / w / tanX, yn = c.y / w / tanY;
        q[i] = {vx0 + (0.5f + 0.5f * xn) * (vx1 - vx0) - 0.5f, (0.5f - 0.5f * yn) * H - 0.5f, 0.5f, 1.0f / w, uv[i][0], uv[i][1]};
    }
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

void Spectator_Draw(IDirect3DDevice9* dev, const SpectatorFrame& f) {
    LoadMode();
    // Smoothly follow the head (or stay locked forward).
    float dt = Seconds();
    Quat target = g_cfg.spectatorFollowHead ? f.eyeRot[0] : Quat{0, 0, 0, 1};
    float tau = g_cfg.spectatorSmoothing;
    float a = tau > 0 ? 1.0f - expf(-dt / tau) : 1.0f;
    s_smoothed = QSlerp(s_smoothed, target, a);

    // Field of view: configured, or slightly inside the headset's so smoothing lag
    // rarely reaches past the rendered image.
    float aspect = ViewAspect(f.hmdTanX, f.hmdTanY);
    if (fabsf(aspect - s_viewAspect) > 0.01f * s_viewAspect) {
        s_viewAspect = aspect;
        s_resize = true;
    }
    float tanX = g_cfg.spectatorFov > 0 ? tanf(g_cfg.spectatorFov * 0.5f * 3.14159265f / 180.0f) : f.hmdTanX * 0.95f;
    if (tanX <= 0) tanX = 1.0f;
    float tanY = tanX / aspect;

    IDirect3DSurface9* prev = nullptr;
    if (!BeginDraw(dev, &prev)) {
        if (s_state) s_state->Apply();
        if (prev) prev->Release();
        return;
    }
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);

    // The back buffer is stretched to the window, so views are laid out in normalised window space.
    const int views = s_mode == SpectatorMode::SideBySide ? 2 : 1;
    const float W = (float)f.bbWidth, H = (float)f.bbHeight;
    const float panelAspect = 3.0f / 4.0f; // frontend/HUD are authored for 4:3 (matches xr.cpp)

    for (int v = 0; v < views; v++) {
        float vx0 = W * v / views, vx1 = W * (v + 1) / views;
        int eye = views == 2 ? v : g_cfg.spectatorEye;
        V3 camPos = f.headPos;
        if (views == 2) camPos = camPos + QRotate(s_smoothed, {eye == 0 ? -0.032f : 0.032f, 0, 0});

        if (f.stereo && EnsureShader(dev)) {
            float R[3][3];
            QToMat3(QMul(QConj(f.eyeRot[eye]), s_smoothed), R);
            float c[16] = {R[0][0], R[0][1], R[0][2], 0, R[1][0], R[1][1], R[1][2], 0,
                           R[2][0], R[2][1], R[2][2], 0, tanX, tanY, 1.0f / f.eyeTanX, 1.0f / f.eyeTanY};
            dev->SetPixelShader(s_reproject);
            dev->SetPixelShaderConstantF(0, c, 4);
            DrawRect(dev, f.eye[eye], vx0, 0, vx1, H, true);
            dev->SetPixelShader(nullptr);
            if (f.hud) {
                SetBlend(dev, true);
                DrawPanel(dev, f.hud, {g_cfg.hudRight, g_cfg.hudHeight, -g_cfg.hudDistance}, g_cfg.hudWidth, g_cfg.hudWidth * panelAspect,
                          camPos, s_smoothed, tanX, tanY, vx0, vx1, H);
                SetBlend(dev, false);
            }
        } else if (!f.stereo) {
            // Menus/movies: the virtual screen, as placed in the headset.
            DrawPanel(dev, f.eye[0], {0, 0, -g_cfg.screenDistance}, g_cfg.screenWidth, g_cfg.screenWidth * panelAspect, camPos,
                      s_smoothed, tanX, tanY, vx0, vx1, H);
        }
    }
    EndDraw(dev, prev);
}
