// SMAA on the eye pictures (see smaa.h). The reference SMAA.hlsl is compiled as is, with
// SMAA_HLSL_3 and the chosen SMAA_PRESET_*; the entry points below only adapt it to shader model
// 3's separate vertex outputs and D3D9's half-pixel offset.
#include "smaa.h"
#include "common.h"
#include <d3dcompiler.h>
#include <string.h>
#include <string>

#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"
#include "../third_party/smaa/SMAA_hlsl.inc"

namespace {

// Registers: c0 = SMAA_RT_METRICS (1/w, 1/h, w, h); s0 colour, s1 edges, s2 blend weights, s3 area,
// s4 search.
const char* kEntries =
    "float4 rtMetrics : register(c0);\n"
    "#define SMAA_RT_METRICS rtMetrics\n"
    "#define SMAA_HLSL_3\n"
    "#include \"SMAA.hlsl\"\n"
    "sampler2D colorTex : register(s0);\n"
    "sampler2D edgesTex : register(s1);\n"
    "sampler2D blendTex : register(s2);\n"
    "sampler2D areaTex : register(s3);\n"
    "sampler2D searchTex : register(s4);\n"
    // D3D9 maps pixel centres half a pixel off: the quad moves by half a pixel to meet them.
    "float4 Quad(float4 p) { return float4(p.xy + float2(-rtMetrics.x, rtMetrics.y), 0, 1); }\n"
    "void EdgeVS(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0,\n"
    "            out float4 o0 : TEXCOORD1, out float4 o1 : TEXCOORD2, out float4 o2 : TEXCOORD3) {\n"
    "  op = Quad(p); ot = t; float4 o[3]; SMAAEdgeDetectionVS(t, o); o0 = o[0]; o1 = o[1]; o2 = o[2]; }\n"
    "float4 EdgePS(float2 t : TEXCOORD0, float4 o0 : TEXCOORD1, float4 o1 : TEXCOORD2, float4 o2 : TEXCOORD3) : COLOR {\n"
    "  float4 o[3] = {o0, o1, o2}; return float4(SMAALumaEdgeDetectionPS(t, o, colorTex), 0, 0); }\n"
    "void WeightVS(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0,\n"
    "              out float2 pix : TEXCOORD1, out float4 o0 : TEXCOORD2, out float4 o1 : TEXCOORD3, out float4 o2 : TEXCOORD4) {\n"
    "  op = Quad(p); ot = t; float4 o[3]; SMAABlendingWeightCalculationVS(t, pix, o); o0 = o[0]; o1 = o[1]; o2 = o[2]; }\n"
    "float4 WeightPS(float2 t : TEXCOORD0, float2 pix : TEXCOORD1, float4 o0 : TEXCOORD2, float4 o1 : TEXCOORD3,\n"
    "                float4 o2 : TEXCOORD4) : COLOR {\n"
    "  float4 o[3] = {o0, o1, o2};\n"
    "  return SMAABlendingWeightCalculationPS(t, pix, o, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0)); }\n"
    "void BlendVS(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0,\n"
    "             out float4 o : TEXCOORD1) {\n"
    "  op = Quad(p); ot = t; SMAANeighborhoodBlendingVS(t, o); }\n"
    "float4 BlendPS(float2 t : TEXCOORD0, float4 o : TEXCOORD1) : COLOR {\n"
    "  return SMAANeighborhoodBlendingPS(t, o, colorTex, blendTex); }\n";

struct Include : ID3DInclude {
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        if (_stricmp(name, "SMAA.hlsl") != 0) return E_FAIL;
        *data = kSmaaHlsl;
        *bytes = (UINT)strlen(kSmaaHlsl);
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
};

struct Pass {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
};
struct Preset {
    Pass edge, weight, blend;
    bool tried = false, ok = false;
};
Preset s_preset[4];                                 // low, medium, high, ultra
const char* const kPresetMacro[4] = {"SMAA_PRESET_LOW", "SMAA_PRESET_MEDIUM", "SMAA_PRESET_HIGH", "SMAA_PRESET_ULTRA"};
const char* const kPresetName[4] = {"low", "medium", "high", "ultra"};
IDirect3DTexture9 *s_area = nullptr, *s_search = nullptr;
IDirect3DTexture9 *s_color = nullptr, *s_edges = nullptr, *s_weights = nullptr;
IDirect3DSurface9 *s_colorSurf = nullptr, *s_edgesSurf = nullptr, *s_weightsSurf = nullptr;
int s_w = 0, s_h = 0;
IDirect3DStateBlock9* s_state = nullptr;
bool s_tried = false, s_ready = false;            // the textures and state block

template <class T> void Drop(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

typedef HRESULT(WINAPI* Compile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

bool CompilePass(IDirect3DDevice9* dev, Compile_t compile, const char* preset, const char* vsEntry, const char* psEntry,
                 Pass& p) {
    Include inc;
    const D3D_SHADER_MACRO macros[] = {{preset, "1"}, {nullptr, nullptr}};
    for (int stage = 0; stage < 2; stage++) {
        const char* entry = stage ? psEntry : vsEntry;
        ID3DBlob *code = nullptr, *errors = nullptr;
        HRESULT hr = compile(kEntries, strlen(kEntries), "smaa", macros, &inc, entry, stage ? "ps_3_0" : "vs_3_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (SUCCEEDED(hr) && code) {
            const DWORD* fn = (const DWORD*)code->GetBufferPointer();
            hr = stage ? dev->CreatePixelShader(fn, &p.ps) : dev->CreateVertexShader(fn, &p.vs);
        }
        if (FAILED(hr) || !code)
            Log("smaa: %s did not compile: %s", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        if (code) code->Release();
        if (errors) errors->Release();
        if (FAILED(hr)) return false;
    }
    return true;
}

// A texture filled from rows of `pitch` bytes (dynamic: D3D9Ex has no managed pool).
IDirect3DTexture9* Upload(IDirect3DDevice9* dev, int w, int h, D3DFORMAT fmt, const unsigned char* bytes, int pitch) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, fmt, D3DPOOL_DEFAULT, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r;
    if (FAILED(t->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) {
        t->Release();
        return nullptr;
    }
    for (int y = 0; y < h; y++) memcpy((unsigned char*)r.pBits + y * r.Pitch, bytes + y * pitch, pitch);
    t->UnlockRect(0);
    return t;
}

bool EnsureTargets(IDirect3DDevice9* dev, int w, int h) {
    if (s_color && w == s_w && h == s_h) return true;
    Drop(s_colorSurf), Drop(s_edgesSurf), Drop(s_weightsSurf);
    Drop(s_color), Drop(s_edges), Drop(s_weights);
    IDirect3DTexture9** tex[3] = {&s_color, &s_edges, &s_weights};
    IDirect3DSurface9** surf[3] = {&s_colorSurf, &s_edgesSurf, &s_weightsSurf};
    for (int i = 0; i < 3; i++) {
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex[i], nullptr)) ||
            FAILED((*tex[i])->GetSurfaceLevel(0, surf[i]))) {
            Log("smaa: %dx%d targets could not be made", w, h);
            return false;
        }
    }
    s_w = w;
    s_h = h;
    return true;
}

void Sampler(IDirect3DDevice9* dev, DWORD s, IDirect3DBaseTexture9* t, bool linear) {
    dev->SetTexture(s, t);
    D3DTEXTUREFILTERTYPE f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
}

void Draw(IDirect3DDevice9* dev, const Pass& p, IDirect3DSurface9* target, bool clear) {
    dev->SetRenderTarget(0, target);
    if (clear) dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetVertexShader(p.vs);
    dev->SetPixelShader(p.ps);
    // Clip space corners and texture coordinates: x, y, z, u, v.
    const float q[4][5] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

}  // namespace

bool Smaa_Prepare(IDirect3DDevice9* dev, int preset) {
    if (!dev || preset < 1 || preset > 4) return false;
    Preset& p = s_preset[preset - 1];
    if (s_ready && p.ok) return true;
    static Compile_t compile;
    if (!compile) {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        compile = dc ? (Compile_t)GetProcAddress(dc, "D3DCompile") : nullptr;
    }
    if (!compile) {
        static bool logged;
        if (!logged) Log("smaa: d3dcompiler_47 not available, SMAA off");
        logged = true;
        return false;
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (!s_tried) {
        s_tried = true;
        // SMAA's lookup textures: area (two channels: A8L8, read .ra) and search (one: L8, read .r).
        s_area = Upload(dev, AREATEX_WIDTH, AREATEX_HEIGHT, D3DFMT_A8L8, areaTexBytes, AREATEX_PITCH);
        s_search = Upload(dev, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, D3DFMT_L8, searchTexBytes, SEARCHTEX_PITCH);
        s_ready = s_area && s_search && SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &s_state));
        if (!s_ready) Log("smaa: lookup textures or state block could not be made, SMAA off");
    }
    if (!s_ready) return false;
    if (!p.tried) {
        p.tried = true;
        const char* m = kPresetMacro[preset - 1];
        p.ok = CompilePass(dev, compile, m, "EdgeVS", "EdgePS", p.edge) && CompilePass(dev, compile, m, "WeightVS", "WeightPS", p.weight) &&
               CompilePass(dev, compile, m, "BlendVS", "BlendPS", p.blend);
        QueryPerformanceCounter(&t1);
        Log("smaa: %s preset %s (%.0f ms)", kPresetName[preset - 1], p.ok ? "ready" : "unavailable",
            (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    }
    return p.ok;
}

bool Smaa_Apply(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst, int w, int h, int preset,
                bool showEdges) {
    if (!dev || !src || !dst || w <= 0 || h <= 0 || !Smaa_Prepare(dev, preset) || !EnsureTargets(dev, w, h)) return false;
    const Preset& pr = s_preset[preset - 1];
    if (FAILED(dev->StretchRect(src, nullptr, s_colorSurf, nullptr, D3DTEXF_NONE))) return false;
    s_state->Capture();
    IDirect3DSurface9 *rt0 = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt0);
    dev->GetDepthStencilSurface(&ds);
    dev->SetDepthStencilSurface(nullptr);
    for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
    D3DVIEWPORT9 vp = {0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE},
                           {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_STENCILENABLE, FALSE},
                           {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_FILLMODE, D3DFILL_SOLID}};
    for (auto& r : rs) dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    float metrics[4] = {1.0f / w, 1.0f / h, (float)w, (float)h};
    dev->SetVertexShaderConstantF(0, metrics, 1);
    dev->SetPixelShaderConstantF(0, metrics, 1);
    for (DWORD s = 0; s < 5; s++) dev->SetTexture(s, nullptr);
    // 1. edges from the luma of the picture.
    Sampler(dev, 0, s_color, true);
    Draw(dev, pr.edge, s_edgesSurf, true);
    if (showEdges) {
        // The edges alone, to see SMAA working (red: left edges, green: top edges, black: none).
        for (DWORD s = 0; s < 5; s++) dev->SetTexture(s, nullptr);
        dev->SetRenderTarget(0, rt0);
        dev->SetDepthStencilSurface(ds);
        if (rt0) rt0->Release();
        if (ds) ds->Release();
        s_state->Apply();
        dev->StretchRect(s_edgesSurf, nullptr, dst, nullptr, D3DTEXF_NONE);
        return true;
    }
    // 2. blending weights along the edges.
    Sampler(dev, 0, nullptr, true);
    Sampler(dev, 1, s_edges, true);
    Sampler(dev, 3, s_area, true);
    Sampler(dev, 4, s_search, false);
    Draw(dev, pr.weight, s_weightsSurf, true);
    // 3. the picture blended with its neighbours, into dst.
    Sampler(dev, 1, nullptr, true);
    Sampler(dev, 3, nullptr, true);
    Sampler(dev, 4, nullptr, true);
    Sampler(dev, 0, s_color, true);
    Sampler(dev, 2, s_weights, true);
    Draw(dev, pr.blend, dst, false);
    for (DWORD s = 0; s < 5; s++) dev->SetTexture(s, nullptr);
    dev->SetRenderTarget(0, rt0);
    dev->SetDepthStencilSurface(ds);
    if (rt0) rt0->Release();
    if (ds) ds->Release();
    s_state->Apply();
    return true;
}

void Smaa_Release() {
    Drop(s_colorSurf), Drop(s_edgesSurf), Drop(s_weightsSurf);
    Drop(s_color), Drop(s_edges), Drop(s_weights);
    Drop(s_area), Drop(s_search);
    for (Preset& p : s_preset) {
        Drop(p.edge.vs), Drop(p.edge.ps), Drop(p.weight.vs), Drop(p.weight.ps), Drop(p.blend.vs), Drop(p.blend.ps);
        p.tried = p.ok = false;
    }
    Drop(s_state);
    s_w = s_h = 0;
    s_ready = s_tried = false;
}
