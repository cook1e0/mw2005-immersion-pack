// d3d9.dll proxy (or an .asi plugin that redirects the game's Direct3DCreate9 import).
//
// * Upgrades the game's IDirect3D9 / device to the Ex versions so render targets
//   can be shared with the D3D11 device that feeds OpenXR (no CPU copies).
// * D3D9Ex has no MANAGED pool, so managed resources are moved to DEFAULT
//   (textures become DYNAMIC to stay lockable).
// * Hooks Present to hand finished eye images to OpenXR.
#include "common.h"
#include "d3d9hook.h"
#include "game.h"
#include "xr.h"
#include "spectator.h"
#include "cockpit.h"
#include "crash.h"

#include <d3d9.h>
#include "d3d9device_gen.h"
#include <d3dcommon.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <intrin.h>

// ---------------------------------------------------------------------------
// Real d3d9.dll
// ---------------------------------------------------------------------------
// As the d3d9.dll proxy this is System32's; as an .asi it is whatever the game's
// import resolved to (System32's or another wrapper's), set by PatchImport.
static HMODULE s_real;
template <typename T> static T Real(const char* name) {
    if (!s_real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\d3d9.dll");
        s_real = LoadLibraryA(path);
    }
    return s_real ? (T)GetProcAddress(s_real, name) : nullptr;
}

// ---------------------------------------------------------------------------
// Vtable patching
// ---------------------------------------------------------------------------
template <typename T> static void PatchVtable(void* obj, int index, T hook, T* original) {
    void** vt = *(void***)obj;
    if (vt[index] == (void*)hook) return;
    *original = (T)vt[index];
    DWORD old;
    VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vt[index] = (void*)hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
}

// IDirect3D9 vtable slot (patched once, right before the game creates its device)
enum { kD3D_CreateDevice = 16 };
typedef HRESULT(WINAPI* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t oCreateDevice;

// ---------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------
static IDirect3DDevice9Ex* s_dev;
static bool s_xr;
static bool s_active;                     // this copy runs the mod (see DllMain)
static IDirect3DTexture9* s_eyeTex[3];    // left, right, HUD (shared with D3D11)
static IDirect3DSurface9* s_eyeSurf[3];
static IDirect3DSurface9* s_hudRT;        // HUD is drawn here (matches the back buffer's MSAA)
static IDirect3DSurface9* s_backBuffer;   // held while the HUD phase is active
static bool s_hudPhase, s_hudValid;
static int s_hudInEyes;                   // eyes this frame with the HUD drawn into them (steering wheel over the HUD)
static bool s_skipPostPass;               // another mod's full-screen pass on the HUD layer, until EndScene
static int s_eyeW, s_eyeH;
static bool s_eyeOk;
static IDirect3DQuery9* s_fence;
static int s_dumpRequested; // frames left to dump
static int s_dumpIndex;

static void* s_hideTex[3];               // draws sampling these are left out (the HUD mirror)
static void* s_boundTex[4];              // textures bound to stages 0..3
static bool s_texHidden;                 // one of them is hidden

static bool IsHidden(void* t) { return t && (t == s_hideTex[0] || t == s_hideTex[1] || t == s_hideTex[2]); }

void D3D_HideTextures(void* a, void* b, void* c) {
    s_hideTex[0] = a;
    s_hideTex[1] = b;
    s_hideTex[2] = c;
    s_texHidden = false;
    for (void* t : s_boundTex) s_texHidden = s_texHidden || IsHidden(t);
}

static IDirect3DTexture9* s_mirrorTex;        // the graded picture the mirrors show
static IDirect3DSurface9* s_mirrorSurf;
static IDirect3DTexture9* s_mirrorRaw;        // the rear view copied as rendered
static IDirect3DSurface9* s_mirrorRawSurf;
static IDirect3DPixelShader9* s_gradeShader;
static UINT s_mirrorW, s_mirrorH;

static void ReleaseEyeTargets() {
    for (int i = 0; i < 3; i++) {
        if (s_eyeSurf[i]) s_eyeSurf[i]->Release();
        if (s_eyeTex[i]) s_eyeTex[i]->Release();
        s_eyeSurf[i] = nullptr;
        s_eyeTex[i] = nullptr;
    }
    if (s_hudRT) s_hudRT->Release();
    s_hudRT = nullptr;
    if (s_mirrorSurf) s_mirrorSurf->Release();
    if (s_mirrorTex) s_mirrorTex->Release();
    if (s_mirrorRawSurf) s_mirrorRawSurf->Release();
    if (s_mirrorRaw) s_mirrorRaw->Release();
    if (s_gradeShader) s_gradeShader->Release();
    s_mirrorSurf = s_mirrorRawSurf = nullptr;
    s_mirrorTex = s_mirrorRaw = nullptr;
    s_gradeShader = nullptr;
    s_mirrorW = s_mirrorH = 0;
    s_eyeW = s_eyeH = 0;
    s_eyeOk = false;
}

static bool EnsureEyeTargets(int w, int h, D3DMULTISAMPLE_TYPE ms, DWORD msQuality) {
    if (s_eyeTex[0] && w == s_eyeW && h == s_eyeH) return s_eyeOk;
    ReleaseEyeTargets();
    HANDLE handles[3] = {};
    for (int i = 0; i < 3; i++) {
        HRESULT hr = s_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                          &s_eyeTex[i], &handles[i]);
        if (FAILED(hr)) {
            Log("d3d9: creating shared eye texture %dx%d failed 0x%08lx", w, h, hr);
            return false;
        }
        s_eyeTex[i]->GetSurfaceLevel(0, &s_eyeSurf[i]);
    }
    if (FAILED(s_dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, ms, msQuality, FALSE, &s_hudRT, nullptr)))
        Log("d3d9: creating HUD render target failed - HUD will stay in the right eye");
    s_eyeW = w;
    s_eyeH = h;
    s_eyeOk = Xr_SetSharedTextures(handles[0], handles[1], handles[2], w, h);
    Log("d3d9: eye targets %dx%d (%s)", w, h, s_eyeOk ? "shared with XR" : "XR attach failed");
    return s_eyeOk;
}

void D3D_CaptureEye(int eye) {
    if (!s_dev) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(s_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (EnsureEyeTargets(d.Width, d.Height, d.MultiSampleType, d.MultiSampleQuality)) {
        HRESULT hr = s_dev->StretchRect(bb, nullptr, s_eyeSurf[eye], nullptr, D3DTEXF_NONE);
        static bool warned;
        if (FAILED(hr) && !warned) {
            warned = true;
            Log("d3d9: StretchRect back buffer -> eye failed 0x%08lx (format %d, msaa %d)", hr, d.Format, d.MultiSampleType);
        }
    }
    bb->Release();
}

static IDirect3DSurface9* s_frameRT;
static IDirect3DSurface9* s_frameDS;

void D3D_BeginEyes() {
    if (!s_dev) return;
    if (s_frameRT) s_frameRT->Release();
    if (s_frameDS) s_frameDS->Release();
    s_frameRT = s_frameDS = nullptr;
    s_hudInEyes = 0;
    s_dev->GetRenderTarget(0, &s_frameRT);
    s_dev->GetDepthStencilSurface(&s_frameDS);
}

void D3D_ResetForNextEye() {
    if (!s_dev || !s_frameRT) return;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    D3DVIEWPORT9 vp;
    s_dev->GetRenderTarget(0, &rt);
    s_dev->GetDepthStencilSurface(&ds);
    s_dev->GetViewport(&vp);
    s_dev->SetRenderTarget(0, s_frameRT);
    s_dev->SetDepthStencilSurface(s_frameDS);
    // Same as the game's Clear after Present (eDisplayFrame @ 0x6E763B).
    DWORD flags = D3DCLEAR_TARGET;
    if (s_frameDS) {
        D3DSURFACE_DESC d;
        s_frameDS->GetDesc(&d);
        flags |= D3DCLEAR_ZBUFFER;
        if (d.Format == D3DFMT_D24S8 || d.Format == D3DFMT_D24X4S4 || d.Format == D3DFMT_D15S1 || d.Format == D3DFMT_D24FS8)
            flags |= D3DCLEAR_STENCIL;
    }
    s_dev->Clear(0, nullptr, flags, 0, 1.0f, 0);
    s_dev->SetRenderTarget(0, rt);
    s_dev->SetDepthStencilSurface(ds);
    s_dev->SetViewport(&vp);
    if (rt) rt->Release();
    if (ds) ds->Release();
}


// The mirror picture's brightness and contrast: a pixel shader compiled once with the system's
// d3dcompiler_47 (c0.x contrast, c0.y brightness).
static IDirect3DPixelShader9* GradeShader() {
    static bool tried;
    if (s_gradeShader || tried) return s_gradeShader;
    tried = true;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    typedef HRESULT(WINAPI * Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                         ID3DBlob**);
    auto compile = dc ? (Compile_t)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) {
        Log("d3d9: d3dcompiler_47 not available - mirror brightness and contrast off");
        return nullptr;
    }
    const char* src =
        "sampler s : register(s0); float4 k : register(c0);"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {"
        "  float3 c = tex2D(s, uv).rgb; c = saturate(((c - 0.5) * k.x + 0.5) * k.y); return float4(c, 1); }";
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(src, strlen(src), "mirror", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &errors);
    if (SUCCEEDED(hr) && code)
        s_dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &s_gradeShader);
    else
        Log("d3d9: mirror shader did not compile: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
    if (code) code->Release();
    if (errors) errors->Release();
    return s_gradeShader;
}

// The side mirror glass's own pixel shader: the rear view as it is (already graded), without the
// car shader's lighting, gloss and environment reflection, which washed it out.
static bool DrawingPlayerView();
static IDirect3DPixelShader9* s_glassShader;
static IDirect3DPixelShader9* GlassShader() {
    static bool tried;
    if (s_glassShader || tried) return s_glassShader;
    tried = true;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    typedef HRESULT(WINAPI * Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                         ID3DBlob**);
    auto compile = dc ? (Compile_t)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) return nullptr;
    const char* src =
        "sampler s : register(s0);"
        "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(tex2D(s, uv).rgb, 1); }";
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(src, strlen(src), "glass", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &errors);
    if (SUCCEEDED(hr) && code)
        s_dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &s_glassShader);
    else
        Log("d3d9: mirror glass shader did not compile: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
    if (code) code->Release();
    if (errors) errors->Release();
    return s_glassShader;
}

// Once: what the game's vertex shader for the glass outputs (the glass shader reads TEXCOORD0).
static void LogGlassVertexShader(IDirect3DDevice9Ex* real) {
    static bool done;
    if (done) return;
    done = true;
    IDirect3DVertexShader9* vs = nullptr;
    if (FAILED(real->GetVertexShader(&vs)) || !vs) {
        Log("d3d9: side mirror glass: fixed-function vertex processing");
        return;
    }
    UINT size = 0;
    std::vector<uint8_t> fn;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size) {
        fn.resize(size);
        vs->GetFunction(fn.data(), &size);
    }
    vs->Release();
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    typedef HRESULT(WINAPI * Disasm_t)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    auto disasm = dc ? (Disasm_t)GetProcAddress(dc, "D3DDisassemble") : nullptr;
    ID3DBlob* text = nullptr;
    if (fn.empty() || !disasm || FAILED(disasm(fn.data(), fn.size(), 0, nullptr, &text)) || !text) return;
    std::string all((const char*)text->GetBufferPointer(), text->GetBufferSize());
    text->Release();
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find('\n', pos);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(pos, end - pos);
        if (line.find("vs_") != std::string::npos || line.find("dcl") != std::string::npos || line.find("oT") != std::string::npos ||
            line.find("oD") != std::string::npos)
            Log("d3d9: glass vs: %s", line.c_str());
        pos = end + 1;
    }
}

// The rear view render target copied into a texture of our own (it may be multisampled, or a
// plain surface no texture owns), then graded into the texture the mirrors show.
static IDirect3DTexture9* MirrorTexture(IDirect3DSurface9* source) {
    D3DSURFACE_DESC d;
    if (FAILED(source->GetDesc(&d)) || d.Width == 0 || d.Height == 0) return nullptr;
    if (!s_mirrorTex || d.Width != s_mirrorW || d.Height != s_mirrorH) {
        for (IUnknown** p : {(IUnknown**)&s_mirrorSurf, (IUnknown**)&s_mirrorTex, (IUnknown**)&s_mirrorRawSurf, (IUnknown**)&s_mirrorRaw})
            if (*p) { (*p)->Release(); *p = nullptr; }
        if (FAILED(s_dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                        &s_mirrorTex, nullptr)) ||
            FAILED(s_dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                        &s_mirrorRaw, nullptr))) {
            Log("d3d9: mirror textures %ux%u could not be created", d.Width, d.Height);
            if (s_mirrorTex) { s_mirrorTex->Release(); s_mirrorTex = nullptr; }
            return nullptr;
        }
        s_mirrorTex->GetSurfaceLevel(0, &s_mirrorSurf);
        s_mirrorRaw->GetSurfaceLevel(0, &s_mirrorRawSurf);
        s_mirrorW = d.Width;
        s_mirrorH = d.Height;
        Log("d3d9: rear view %ux%u (format %d, msaa %d) copied for the mirrors", d.Width, d.Height, d.Format, d.MultiSampleType);
    }
    HRESULT hr = s_dev->StretchRect(source, nullptr, s_mirrorRawSurf, nullptr, D3DTEXF_LINEAR);
    static bool warned;
    if (FAILED(hr)) {
        if (!warned) {
            warned = true;
            Log("d3d9: copying the rear view failed 0x%08lx", hr);
        }
        return nullptr;
    }
    IDirect3DPixelShader9* ps = GradeShader();
    if (!ps) {
        s_dev->StretchRect(s_mirrorRawSurf, nullptr, s_mirrorSurf, nullptr, D3DTEXF_NONE);
        return s_mirrorTex;
    }
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(s_dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return s_mirrorTex;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    s_dev->GetRenderTarget(0, &rt);
    s_dev->GetDepthStencilSurface(&ds);
    s_dev->SetRenderTarget(0, s_mirrorSurf);
    s_dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, s_mirrorW, s_mirrorH, 0.0f, 1.0f};
    s_dev->SetViewport(&vp);
    s_dev->SetVertexShader(nullptr);
    s_dev->SetPixelShader(ps);
    float k[4] = {g_cfg.mirrorContrast, g_cfg.mirrorBrightness, 0, 0};
    s_dev->SetPixelShaderConstantF(0, k, 1);
    s_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    s_dev->SetTexture(0, s_mirrorRaw);
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    float w = (float)s_mirrorW, h = (float)s_mirrorH;
    const float quad[4][6] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0},
                              {w - 0.5f, h - 0.5f, 0, 1, 1, 1}, {-0.5f, h - 0.5f, 0, 1, 0, 1}};
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(quad[0]));
    saved->Apply();
    saved->Release();
    s_dev->SetRenderTarget(0, rt);
    s_dev->SetDepthStencilSurface(ds);
    if (rt) rt->Release();
    if (ds) ds->Release();
    return s_mirrorTex;
}

void* D3D_TextureOfSurface(void* surface) {
    static void* lastSurface;
    static void* lastTexture;
    if (!surface) return nullptr;
    if (surface != lastSurface) {
        lastSurface = surface;
        lastTexture = nullptr;
        IDirect3DTexture9* t = nullptr;
        if (SUCCEEDED(((IDirect3DSurface9*)surface)->GetContainer(__uuidof(IDirect3DTexture9), (void**)&t)) && t) {
            lastTexture = t;
            t->Release();
        }
    }
    return lastTexture;
}

static void* s_curIndices;            // index buffer the game last set

// --- the game's side mirror glass with the rear view ---------------------------------------------
struct MirrorGlassJob {
    void* ib;
    UINT first, tris;
    float basis[14];
    std::vector<uint16_t> vertices;               // the glass's own vertices (index values)
    std::vector<std::pair<void*, INT>> mapped;    // vertex buffer, base vertex already given texture coordinates
};
static std::vector<MirrorGlassJob> s_glassJobs;

void D3D_RegisterMirrorGlass(void* indexBuffer, int firstIndex, int triangles, const float basis[14], const uint16_t* vertices,
                             int vertexCount) {
    MirrorGlassJob* job = nullptr;
    for (MirrorGlassJob& j : s_glassJobs)
        if (j.ib == indexBuffer && j.first == (UINT)firstIndex) job = &j;
    if (!job) {
        s_glassJobs.push_back(MirrorGlassJob());
        job = &s_glassJobs.back();
    }
    job->ib = indexBuffer;
    job->first = firstIndex;
    job->tris = triangles;
    memcpy(job->basis, basis, sizeof(job->basis));
    job->vertices.assign(vertices, vertices + vertexCount);
    job->mapped.clear();
}

// Texture coordinates across the glass for the glass's own vertices (in the game's own vertex
// buffer, once per buffer and base): u along the glass within the job's range, v top to bottom.
static void MapGlassVertices(MirrorGlassJob& j, INT base) {
    if (j.vertices.empty()) return;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(s_dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb) return;
    for (auto& m : j.mapped)
        if (m.first == vb && m.second == base) {
            vb->Release();
            return;
        }
    j.mapped.push_back({vb, base});
    int posOfs = -1, uvOfs = -1;
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(s_dev->GetVertexDeclaration(&decl)) && decl) {
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
        UINT n = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(decl->GetDeclaration(el, &n)))
            for (UINT i = 0; i < n && el[i].Stream != 0xFF; i++) {
                if (el[i].Stream != 0) continue;
                if (el[i].Usage == D3DDECLUSAGE_POSITION && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_FLOAT3) posOfs = el[i].Offset;
                if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_FLOAT2) uvOfs = el[i].Offset;
            }
        decl->Release();
    }
    if (posOfs < 0 || uvOfs < 0 || stride < 20) {
        Log("d3d9: side mirror glass: unexpected vertex layout (position %d, uv %d, stride %u)", posOfs, uvOfs, stride);
        vb->Release();
        return;
    }
    uint16_t lo = 0xFFFF, hi = 0;
    for (uint16_t v : j.vertices) {
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    UINT firstVertex = (UINT)(base + lo), count = (UINT)(hi - lo + 1);
    uint8_t* p = nullptr;
    if (FAILED(vb->Lock(offset + firstVertex * stride, count * stride, (void**)&p, 0)) || !p) {
        Log("d3d9: side mirror glass: vertex buffer lock failed");
        vb->Release();
        return;
    }
    const float* b = j.basis;
    float cu = b[0] * b[3] + b[1] * b[4] + b[2] * b[5], cv = b[0] * b[6] + b[1] * b[7] + b[2] * b[8];
    for (uint16_t v : j.vertices) {
        UINT i = v - lo;
        const float* pos = (const float*)(p + i * stride + posOfs);
        float* uv = (float*)(p + i * stride + uvOfs);
        float pu = pos[0] * b[3] + pos[1] * b[4] + pos[2] * b[5], pv = pos[0] * b[6] + pos[1] * b[7] + pos[2] * b[8];
        float s = b[9] > 0 ? (pu - (cu - b[9])) / (2 * b[9]) : 0.5f, t = b[10] > 0 ? ((cv + b[10]) - pv) / (2 * b[10]) : 0.5f;
        s = s < 0 ? 0 : s > 1 ? 1 : s;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        uv[0] = b[11] + (b[12] - b[11]) * s;
        uv[1] = t;
    }
    vb->Unlock();
    vb->Release();
    static int logged;
    if (logged++ < 4) Log("d3d9: side mirror glass: %u vertices given the rear view's coordinates", (unsigned)j.vertices.size());
}


// The glass's draw: its own texture (sampler 0, a 2D texture; the car shader's environment cube is
// on sampler 1) is the rear view for this draw.
static bool DrawMirrorGlassNative(IDirect3DDevice9Ex* real, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT verts,
                                  UINT start, UINT count, HRESULT* hr) {
    if (s_glassJobs.empty() || !s_mirrorTex) return false;
    for (MirrorGlassJob& j : s_glassJobs) {
        if (j.ib != s_curIndices || j.first != start) continue;
        MapGlassVertices(j, base);
        int stage = -1;
        IDirect3DBaseTexture9* t0 = nullptr;
        real->GetTexture(0, &t0);
        if (t0) {
            if (t0->GetType() == D3DRTYPE_TEXTURE) stage = 0;
            t0->Release();
        }
        if (stage < 0) {
            static bool logged;
            if (!logged) {
                logged = true;
                for (DWORD st = 0; st < 4; st++) {
                    DWORD tci = 0, op = 0;
                    IDirect3DBaseTexture9* t = nullptr;
                    real->GetTextureStageState(st, D3DTSS_TEXCOORDINDEX, &tci);
                    real->GetTextureStageState(st, D3DTSS_COLOROP, &op);
                    real->GetTexture(st, &t);
                    Log("d3d9: side mirror glass: stage %lu texcoord %lu op %lu texture type %d", st, tci, op, t ? (int)t->GetType() : -1);
                    if (t) t->Release();
                }
            }
            return false;
        }
        IDirect3DBaseTexture9* orig = nullptr;
        real->GetTexture(stage, &orig);
        real->SetTexture(stage, s_mirrorTex);
        LogGlassVertexShader(real);
        IDirect3DPixelShader9* glass = stage == 0 && DrawingPlayerView() ? GlassShader() : nullptr;
        IDirect3DPixelShader9* oldPs = nullptr;
        if (glass) {
            real->GetPixelShader(&oldPs);
            real->SetPixelShader(glass);
        }
        *hr = real->DrawIndexedPrimitive(type, base, minIndex, verts, start, count);
        if (glass) {
            real->SetPixelShader(oldPs);
            if (oldPs) oldPs->Release();
        }
        real->SetTexture(stage, orig);
        if (orig) orig->Release();
        static bool logged;
        if (!logged) {
            logged = true;
            Log("d3d9: side mirror glass drawn with the rear view (stage %d)", stage);
        }
        return true;
    }
    return false;
}

// --- mirrors with the models -------------------------------------------------------------------
struct PendingMirror {
    void* ib;
    std::vector<float> verts, frame;
    bool drawn;
};
static std::vector<PendingMirror> s_pendingMirrors;
static D3DMATRIX s_mirrorView, s_mirrorProj;
static float s_mirrorBrightness = 0.85f;
static bool s_mirrorEye;

void D3D_BeginMirrorEye(const float* view, const float* proj, float brightness) {
    s_pendingMirrors.clear();
    memcpy(&s_mirrorView, view, sizeof(s_mirrorView));
    memcpy(&s_mirrorProj, proj, sizeof(s_mirrorProj));
    s_mirrorBrightness = brightness;
    s_mirrorEye = true;
}

void D3D_AddMirror(void* indexBuffer, const float* verts, int vertexCount, const float* frame, int frameCount) {
    if (!s_mirrorEye || !indexBuffer || vertexCount < 3) return;
    for (PendingMirror& p : s_pendingMirrors)
        if (p.ib == indexBuffer && p.verts.size() == (size_t)vertexCount * 5 && !memcmp(p.verts.data(), verts, 5 * sizeof(float)))
            return;
    PendingMirror m;
    m.ib = indexBuffer;
    m.verts.assign(verts, verts + vertexCount * 5);
    if (frame && frameCount >= 3) m.frame.assign(frame, frame + frameCount * 5);
    m.drawn = false;
    s_pendingMirrors.push_back(m);
}

// Draws one mirror's glass into whatever the game is drawing to now, with its depth buffer.
static void DrawMirrorGlass(const PendingMirror& m, bool depth) {
    if (!s_mirrorTex) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(s_dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
    s_dev->SetVertexShader(nullptr);
    s_dev->SetPixelShader(nullptr);
    s_dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    s_dev->SetTransform(D3DTS_WORLD, &identity);
    s_dev->SetTransform(D3DTS_VIEW, &s_mirrorView);
    s_dev->SetTransform(D3DTS_PROJECTION, &s_mirrorProj);
    s_dev->SetRenderState(D3DRS_ZENABLE, depth ? D3DZB_TRUE : D3DZB_FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, depth ? TRUE : FALSE);
    s_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    s_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    // Pulled toward the camera in depth too, so the game's own glass under it never shows through.
    float bias = -0.00002f, slope = -1.0f;
    s_dev->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&bias);
    s_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD*)&slope);
    BYTE k = 255;   // brightness and contrast are in the picture (MirrorTexture)
    s_dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, k, k, k));
    s_dev->SetTexture(0, s_mirrorTex);
    for (DWORD st = 1; st < 8; st++) s_dev->SetTexture(st, nullptr);
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    s_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    s_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    s_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    s_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    if (!m.frame.empty()) {
        // The rim: plain near-black, the housing's plastic around the glass. It only tests depth,
        // so the picture drawn next over it can't fight it.
        s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        s_dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, 10, 10, 12));
        s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(m.frame.size() / 15), m.frame.data(), 5 * sizeof(float));
        s_dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, k, k, k));
        s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        s_dev->SetRenderState(D3DRS_ZWRITEENABLE, depth ? TRUE : FALSE);
    }
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(m.verts.size() / 15), m.verts.data(), 5 * sizeof(float));
    saved->Apply();
    saved->Release();
}

// --- the steering wheel in front of the HUD -------------------------------------------------------
static std::vector<std::pair<void*, UINT>> s_wheelDraws;   // index buffer, first index
static struct {
    bool active, marked;
    float seat[12];
    D3DMATRIX view, proj;
    IDirect3DSurface9* ds;                // the depth/stencil the wheel was marked in
} s_wheelEye;
static float s_uiMenuOpen;                // live from the gameplay mod: its F4 menu is open
static bool DrawingPlayerView();
static const DWORD kWheelStencil = 0x80;

void D3D_RegisterWheelDraw(void* ib, int first) {
    for (auto& d : s_wheelDraws)
        if (d.first == ib && d.second == (UINT)first) return;
    if (s_wheelDraws.size() >= 64) s_wheelDraws.clear();
    s_wheelDraws.push_back({ib, (UINT)first});
}

void D3D_BeginWheelEye(bool on, const float seat[12], const float* view, const float* proj) {
    s_wheelEye.active = on && s_uiMenuOpen < 0.5f && s_eyeTex[2] && s_hudValid;
    s_wheelEye.marked = false;
    if (s_wheelEye.ds) s_wheelEye.ds->Release();
    s_wheelEye.ds = nullptr;
    memcpy(s_wheelEye.seat, seat, sizeof(s_wheelEye.seat));
    memcpy(&s_wheelEye.view, view, sizeof(s_wheelEye.view));
    memcpy(&s_wheelEye.proj, proj, sizeof(s_wheelEye.proj));
}

static bool HasStencil(D3DFORMAT f) {
    return f == D3DFMT_D24S8 || f == D3DFMT_D24X4S4 || f == D3DFMT_D15S1 || f == D3DFMT_D24FS8;
}

// A wheel draw into the eye: drawn as the game set it up, also writing the wheel bit of the stencil
// where it ends up visible.
static bool DrawWheelMarked(IDirect3DDevice9Ex* real, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT verts, UINT start,
                            UINT count, HRESULT* hr) {
    if (!s_wheelEye.active || s_wheelDraws.empty()) return false;
    bool match = false;
    for (auto& d : s_wheelDraws) match = match || (d.first == s_curIndices && d.second == start);
    if (!match || !DrawingPlayerView()) return false;
    IDirect3DSurface9* ds = nullptr;
    D3DSURFACE_DESC dd;
    if (FAILED(real->GetDepthStencilSurface(&ds)) || !ds || FAILED(ds->GetDesc(&dd)) || !HasStencil(dd.Format)) {
        static bool logged;
        if (!logged) {
            logged = true;
            Log("d3d9: steering wheel over the HUD: the depth buffer has no stencil (format %d), not available", ds ? (int)dd.Format : -1);
        }
        if (ds) ds->Release();
        return false;
    }
    const D3DRENDERSTATETYPE states[] = {D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK,
                                         D3DRS_STENCILWRITEMASK, D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
                                         D3DRS_TWOSIDEDSTENCILMODE};
    const DWORD values[] = {TRUE, D3DCMP_ALWAYS, kWheelStencil, kWheelStencil, kWheelStencil, D3DSTENCILOP_REPLACE,
                            D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, FALSE};
    DWORD saved[9];
    for (int i = 0; i < 9; i++) {
        real->GetRenderState(states[i], &saved[i]);
        real->SetRenderState(states[i], values[i]);
    }
    *hr = real->DrawIndexedPrimitive(type, base, minIndex, verts, start, count);
    for (int i = 0; i < 9; i++) real->SetRenderState(states[i], saved[i]);
    if (s_wheelEye.ds != ds) {
        if (s_wheelEye.ds) s_wheelEye.ds->Release();
        s_wheelEye.ds = ds;
    } else {
        ds->Release();
    }
    s_wheelEye.marked = true;
    return true;
}

struct PanelVertex {
    float x, y, z;
    float u, v;
};

// After the eye: the HUD panel over the scene, but not over the wheel's pixels.
void D3D_EndWheelEye() {
    if (!s_wheelEye.active) return;
    s_wheelEye.active = false;
    IDirect3DSurface9* ds = s_wheelEye.ds;
    s_wheelEye.ds = nullptr;
    if (!s_wheelEye.marked || !ds) {
        // No wheel this eye (an outside camera): the headset's HUD layer as usual.
        if (ds) ds->Release();
        return;
    }
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *curDs = nullptr;
    D3DSURFACE_DESC b, d;
    if (FAILED(s_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb || FAILED(bb->GetDesc(&b)) || FAILED(ds->GetDesc(&d)) ||
        d.Width < b.Width || d.Height < b.Height || d.MultiSampleType != b.MultiSampleType) {
        static bool logged;
        if (!logged) {
            logged = true;
            Log("d3d9: steering wheel over the HUD: the stencil doesn't fit the back buffer, not available");
        }
        if (bb) bb->Release();
        ds->Release();
        return;
    }
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(s_dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
        bb->Release();
        ds->Release();
        return;
    }
    s_dev->GetRenderTarget(0, &rt);
    s_dev->GetDepthStencilSurface(&curDs);
    s_dev->SetRenderTarget(0, bb);
    s_dev->SetDepthStencilSurface(ds);

    // The panel: as the headset's HUD layer places it (xr.cpp), 4:3.
    const float* s = s_wheelEye.seat;   // origin, right, up, forward
    float w = g_cfg.hudWidth, h = g_cfg.hudWidth * 0.75f, cx = g_cfg.hudRight, cy = g_cfg.hudHeight, cz = g_cfg.hudDistance;
    auto at = [&](float x, float y, float u, float v) {
        PanelVertex p;
        p.x = s[0] + s[3] * x + s[6] * y + s[9] * cz;
        p.y = s[1] + s[4] * x + s[7] * y + s[10] * cz;
        p.z = s[2] + s[5] * x + s[8] * y + s[11] * cz;
        p.u = u;
        p.v = v;
        return p;
    };
    PanelVertex a = at(cx - w / 2, cy + h / 2, 0, 0), c = at(cx + w / 2, cy + h / 2, 1, 0), e = at(cx + w / 2, cy - h / 2, 1, 1),
                f = at(cx - w / 2, cy - h / 2, 0, 1);
    PanelVertex quad[6] = {a, c, e, a, e, f};

    s_dev->SetVertexShader(nullptr);
    s_dev->SetPixelShader(nullptr);
    s_dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    s_dev->SetTransform(D3DTS_WORLD, &identity);
    s_dev->SetTransform(D3DTS_VIEW, &s_wheelEye.view);
    s_dev->SetTransform(D3DTS_PROJECTION, &s_wheelEye.proj);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    s_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    s_dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    s_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
    s_dev->SetRenderState(D3DRS_STENCILREF, kWheelStencil);
    s_dev->SetRenderState(D3DRS_STENCILMASK, kWheelStencil);
    s_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
    s_dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    s_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    s_dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    s_dev->SetTexture(0, s_eyeTex[2]);
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    s_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    s_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    s_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    HRESULT hr = s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(PanelVertex));

    s_dev->SetRenderTarget(0, rt);
    s_dev->SetDepthStencilSurface(curDs);
    saved->Apply();
    saved->Release();
    if (rt) rt->Release();
    if (curDs) curDs->Release();
    bb->Release();
    ds->Release();
    if (SUCCEEDED(hr)) s_hudInEyes++;
    static int logged;
    if (logged < 2) {
        logged++;
        Log("d3d9: HUD drawn into the eye behind the steering wheel (0x%08lx)", hr);
    }
}

// After the game's draw from `ib`: the mirror glass of that model, once per eye.
// The car is also drawn into shadow maps, the environment map and the rear view: only the player
// view's draw counts, the one into a target the size of the back buffer, with a depth buffer.
static bool DrawingPlayerView() {
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr, *bb = nullptr;
    bool ok = false;
    if (SUCCEEDED(s_dev->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(s_dev->GetDepthStencilSurface(&ds)) && ds &&
        SUCCEEDED(s_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC r, b;
        ok = SUCCEEDED(rt->GetDesc(&r)) && SUCCEEDED(bb->GetDesc(&b)) && r.Width == b.Width && r.Height == b.Height;
    }
    if (rt) rt->Release();
    if (ds) ds->Release();
    if (bb) bb->Release();
    return ok;
}

static void AfterModelDraw() {
    if (!s_mirrorEye || s_pendingMirrors.empty()) return;
    bool checked = false, playerView = false;
    for (PendingMirror& m : s_pendingMirrors) {
        if (m.drawn || m.ib != s_curIndices) continue;
        if (!checked) {
            checked = true;
            playerView = DrawingPlayerView();
        }
        if (!playerView) return;
        m.drawn = true;
        DrawMirrorGlass(m, true);
        static int logged;
        if (logged < 3) {
            logged++;
            Log("d3d9: mirror drawn with its model (%u triangles)", (unsigned)(m.verts.size() / 15));
        }
    }
}

void D3D_EndMirrorEye(void* rearViewSurface) {
    if (!s_mirrorEye) return;
    s_mirrorEye = false;
    for (PendingMirror& m : s_pendingMirrors) {
        if (m.drawn) continue;
        static bool logged;
        if (!logged) {
            logged = true;
            Log("d3d9: a mirror's model was not seen being drawn - drawn over the scene instead");
        }
        DrawMirrorGlass(m, false);
    }
    s_pendingMirrors.clear();
    if (rearViewSurface) MirrorTexture((IDirect3DSurface9*)rearViewSurface);
}

// True for a return address inside speed.exe (as opposed to another mod's DLL).
static bool FromGame(void* addr) {
    static uintptr_t end;
    if (!end) {
        uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        end = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    }
    uintptr_t a = (uintptr_t)addr;
    return a >= (uintptr_t)GetModuleHandleA(nullptr) && a < end;
}

// Colour writes in the HUD phase include alpha (see DeviceWrap::SetRenderState).
static DWORD HudColorWrite(DWORD mask) {
    return mask & (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE)
               ? mask | D3DCOLORWRITEENABLE_ALPHA : mask;
}

// After the last eye's scene render: keep the scene, then point everything the game
// draws next (HUD, frontend) at a transparent render target.
void D3D_BeginHudPhase() {
    D3D_CaptureEye(1);
    if (!s_dev || !s_hudRT || s_hudPhase) return;
    if (FAILED(s_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s_backBuffer))) return;
    D3DVIEWPORT9 vp;
    s_dev->GetViewport(&vp);
    IDirect3DSurface9* cur = nullptr;
    s_dev->GetRenderTarget(0, &cur);
    s_dev->SetRenderTarget(0, s_hudRT);
    s_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    s_dev->SetRenderTarget(0, cur == s_backBuffer ? s_hudRT : cur);
    if (cur) cur->Release();
    s_dev->SetViewport(&vp);
    DWORD mask;
    if (SUCCEEDED(s_dev->GetRenderState(D3DRS_COLORWRITEENABLE, &mask))) s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, HudColorWrite(mask));
    s_hudPhase = true;
}

static void EndHudPhase() {
    if (s_frameRT) s_frameRT->Release();
    if (s_frameDS) s_frameDS->Release();
    s_frameRT = s_frameDS = nullptr;
    s_skipPostPass = false;
    if (!s_hudPhase) return;
    s_hudPhase = false;
    IDirect3DSurface9* cur = nullptr;
    s_dev->GetRenderTarget(0, &cur);
    if (cur == s_hudRT) s_dev->SetRenderTarget(0, s_backBuffer);
    if (cur) cur->Release();
    s_hudValid = s_eyeSurf[2] && SUCCEEDED(s_dev->StretchRect(s_hudRT, nullptr, s_eyeSurf[2], nullptr, D3DTEXF_NONE));
    if (s_hudValid && s_hudInEyes < 2 && Spectator_Mode() == SpectatorMode::Eye)
        Spectator_BlendFullscreen(s_dev, s_eyeTex[2], s_eyeW, s_eyeH);
    s_backBuffer->Release();
    s_backBuffer = nullptr;
}

// --- frame timing ---------------------------------------------------------------------------------
static struct {
    double sum[PROF_COUNT], max[PROF_COUNT], frameMax, last, since;
    int frames;
} s_prof;

double D3D_ProfNow() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void D3D_ProfAdd(int slot, double seconds) {
    if (slot < 0 || slot >= PROF_COUNT) return;
    s_prof.sum[slot] += seconds;
    if (seconds > s_prof.max[slot]) s_prof.max[slot] = seconds;
}

// Once per Present: the frame's length, and every 5 s the breakdown.
static void ProfFrame() {
    double now = D3D_ProfNow();
    if (s_prof.last > 0) {
        double frame = now - s_prof.last;
        s_prof.frames++;
        if (frame > s_prof.frameMax) s_prof.frameMax = frame;
    } else {
        s_prof.since = now;
    }
    s_prof.last = now;
    double span = now - s_prof.since;
    if (span < 5.0 || s_prof.frames == 0) return;
    static const char* names[PROF_COUNT] = {"headset wait", "eyes (CPU)", "GPU wait", "submit", "spectator", "present"};
    char line[512];
    int n = snprintf(line, sizeof(line), "perf: %.1f fps (frame %.1f ms avg, %.1f max):", s_prof.frames / span,
                     span / s_prof.frames * 1000.0, s_prof.frameMax * 1000.0);
    double known = 0;
    for (int i = 0; i < PROF_COUNT && n < (int)sizeof(line); i++) {
        known += s_prof.sum[i];
        n += snprintf(line + n, sizeof(line) - n, " %s %.1f/%.1f", names[i], s_prof.sum[i] / s_prof.frames * 1000.0,
                      s_prof.max[i] * 1000.0);
    }
    if (n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - n, ", game %.1f ms (avg/max ms)", (span - known) / s_prof.frames * 1000.0);
    Log("%s", line);
    memset(&s_prof, 0, sizeof(s_prof));
    s_prof.last = now;
    s_prof.since = now;
}

// D3D9Ex has no keyed mutex, so wait for the GPU before D3D11 reads the shared textures.
static void WaitForGpu() {
    if (!s_fence && FAILED(s_dev->CreateQuery(D3DQUERYTYPE_EVENT, &s_fence))) return;
    s_fence->Issue(D3DISSUE_END);
    while (s_fence->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) YieldProcessor();
}

static void DumpEyes() {
    for (int i = 0; i < 3; i++) {
        if (!s_eyeSurf[i]) continue;
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(s_dev->CreateOffscreenPlainSurface(s_eyeW, s_eyeH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr))) continue;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s_dev->GetRenderTargetData(s_eyeSurf[i], sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%svr_dump_%03d_%s.bmp", GameDir(), s_dumpIndex, i == 0 ? "left" : i == 1 ? "right" : "hud");
            FILE* f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof(ih);
                ih.biWidth = s_eyeW;
                ih.biHeight = -s_eyeH; // top-down
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + s_eyeW * s_eyeH * 4;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (int y = 0; y < s_eyeH; y++) fwrite((char*)lr.pBits + y * lr.Pitch, 4, s_eyeW, f);
                fclose(f);
                Log("d3d9: wrote %s", path);
            }
            sys->UnlockRect();
        }
        sys->Release();
    }
    s_dumpIndex++;
}

static bool KeyPressed(int vk) {
    if (vk <= 0 || vk > 255) return false;
    static bool down[256];
    bool now = (GetAsyncKeyState(vk) & 0x8001) != 0; // bit 0 catches taps between frames
    bool pressed = now && !down[vk & 0xff];
    down[vk & 0xff] = now;
    return pressed;
}

static void AdjustPresentParams(D3DPRESENT_PARAMETERS* pp) {
    if (g_cfg.forceWindowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    if (g_cfg.disableVSync) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    Log("d3d9: present params %ux%u fmt=%d windowed=%d msaa=%d interval=0x%x", pp->BackBufferWidth, pp->BackBufferHeight,
        pp->BackBufferFormat, pp->Windowed, pp->MultiSampleType, pp->PresentationInterval);
}

static HWND s_wnd;
static UINT s_bbW, s_bbH;
static void FitWindow(HWND wnd, UINT w, UINT h);

static void OnPresent() {
    if (KeyPressed(g_cfg.keySpectator)) {
        Spectator_CycleMode();
        FitWindow(s_wnd, s_bbW, s_bbH);
    }
    if (KeyPressed(g_cfg.keyRecenter)) Xr_Recenter();
    if (KeyPressed(g_cfg.keyToggle)) Game_ToggleStereo();
    if (KeyPressed(g_cfg.keyLevel)) Game_ToggleLevel();
    if (KeyPressed(g_cfg.keyDump)) s_dumpRequested = g_cfg.dumpFrames > 0 ? g_cfg.dumpFrames : 1;
    Cockpit_Keys();

    if (s_xr) {
        bool stereo = Game_StereoThisFrame(), captured = false;
        if (stereo) {
            captured = true;
            EndHudPhase();
            double t0 = D3D_ProfNow();
            WaitForGpu();
            double t1 = D3D_ProfNow();
            float tx, ty;
            Game_RenderedFov(&tx, &ty);
            // With the steering wheel in front of the HUD, the HUD is already in both eyes.
            Xr_EndFrame(LayerMode::Stereo, tx, ty, s_hudValid && s_hudInEyes < 2);
            D3D_ProfAdd(PROF_GPUWAIT, t1 - t0);
            D3D_ProfAdd(PROF_SUBMIT, D3D_ProfNow() - t1);
        } else if (Xr_BeginFrame()) {
            // Menus, movies, loading screens: show the flat frame on a virtual screen.
            D3D_CaptureEye(0);
            captured = true;
            WaitForGpu();
            Xr_EndFrame(LayerMode::Screen, 0, 0, false);
        } else {
            Xr_EndFrame(LayerMode::None, 0, 0, false);
        }
        if (captured && s_eyeOk && Spectator_Mode() != SpectatorMode::Eye) {
            SpectatorFrame sf = {};
            sf.eye[0] = s_eyeTex[0];
            sf.eye[1] = s_eyeTex[1];
            sf.hud = stereo && s_hudValid && s_hudInEyes < 2 ? s_eyeTex[2] : nullptr;
            sf.stereo = stereo;
            Game_RenderedFov(&sf.eyeTanX, &sf.eyeTanY);
            const EyeView& e0 = Xr_Eye(0);
            const EyeView& e1 = Xr_Eye(1);
            sf.eyeRot[0] = e0.relOrientation;
            sf.eyeRot[1] = e1.relOrientation;
            sf.headPos = (e0.relPosition + e1.relPosition) * 0.5f;
            sf.hmdTanX = std::max(std::max(e0.tanLeft, e0.tanRight), std::max(e1.tanLeft, e1.tanRight));
            sf.hmdTanY = std::max(std::max(e0.tanUp, e0.tanDown), std::max(e1.tanUp, e1.tanDown));
            sf.bbWidth = s_eyeW;
            sf.bbHeight = s_eyeH;
            double ts = D3D_ProfNow();
            Spectator_Draw(s_dev, sf);
            D3D_ProfAdd(PROF_SPECTATOR, D3D_ProfNow() - ts);
            if (Spectator_ConsumeResize()) FitWindow(s_wnd, s_bbW, s_bbH);
        }
        if (s_dumpRequested > 0) {
            s_dumpRequested--;
            DumpEyes();
        }
    }
    Game_EndOfFrame();
}

static void ConvertPool(D3DPOOL& pool, DWORD& usage, bool texture) {
    if (pool != D3DPOOL_MANAGED) return;
    pool = D3DPOOL_DEFAULT;
    if (texture) usage |= D3DUSAGE_DYNAMIC;
}

// ---------------------------------------------------------------------------
// Device wrapper. d3d9 rewrites its per-device vtable (e.g. around Reset), so
// in-place vtable patches don't stick; the game gets this forwarding object instead.
// ---------------------------------------------------------------------------
class DeviceWrap : public DeviceWrapBase {
    ULONG refs = 1;

public:
    using DeviceWrapBase::DeviceWrapBase;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return real->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (r == 0) {
            Log("d3d9: device released");
            ReleaseEyeTargets();
            if (s_fence) s_fence->Release();
            s_fence = nullptr;
            real->Release();
            s_dev = nullptr;
            delete this;
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override {
        EndHudPhase();
        ReleaseEyeTargets();
        AdjustPresentParams(pp);
        HRESULT hr = real->Reset(pp);
        Log("d3d9: Reset -> 0x%08lx", hr);
        if (SUCCEEDED(hr)) FitWindow(pp->hDeviceWindow ? pp->hDeviceWindow : s_wnd, pp->BackBufferWidth, pp->BackBufferHeight);
        return hr;
    }

    // The HUD layer's alpha is its coverage in the headset. The game's masked HUD images (nitro
    // and speedbreaker arcs, IDI_FE_MASK_FX) never get FEColorWriteFlag set, so their effect
    // writes colour without alpha and they would be invisible there.
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override {
        if (s_hudPhase && state == D3DRS_COLORWRITEENABLE) value = HudColorWrite(value);
        return real->SetRenderState(state, value);
    }

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index, IDirect3DSurface9* surf) override {
        if (s_hudPhase && index == 0 && surf == s_backBuffer) surf = s_hudRT;
        return real->SetRenderTarget(index, surf);
    }

    // Post-processing from other mods at the end of the frame (the widescreen fix's ConsoleGamma
    // and SMAA) copies the current render target and draws it back full-screen. In a stereo frame
    // that target is the transparent HUD layer: the pass would fill the HUD panel with black, and
    // the eyes were captured before it anyway. Such a copy of the HUD layer by code outside
    // speed.exe skips the draws that follow, up to EndScene. Flat frames (menus on the virtual
    // screen, movies) keep the pass.
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src, CONST RECT* srcRect, IDirect3DSurface9* dst,
                                          CONST RECT* dstRect, D3DTEXTUREFILTERTYPE filter) override {
        if (s_hudPhase && src == s_hudRT && !s_skipPostPass && !FromGame(_ReturnAddress())) {
            s_skipPostPass = true;
            static bool logged;
            if (!logged) Log("d3d9: skipping another mod's post-processing pass on the HUD layer (e.g. widescreen fix ConsoleGamma)");
            logged = true;
        }
        return real->StretchRect(src, srcRect, dst, dstRect, filter);
    }
    // Same for other mods' colour clears on the HUD layer (the widescreen fix's HideAABug clears
    // 1px black lines along the screen edges, which framed the HUD panel).
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, CONST D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                                    DWORD stencil) override {
        if (s_hudPhase && (flags & D3DCLEAR_TARGET) && !FromGame(_ReturnAddress())) {
            static bool logged;
            if (!logged) Log("d3d9: skipping another mod's clear of the HUD layer (e.g. widescreen fix HideAABug)");
            logged = true;
            flags &= ~D3DCLEAR_TARGET;
            if (!flags) return D3D_OK;
        }
        return real->Clear(count, rects, flags, color, z, stencil);
    }
    HRESULT STDMETHODCALLTYPE EndScene() override {
        s_skipPostPass = false;
        return real->EndScene();
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* tex) override {
        if (stage < 4) {
            s_boundTex[stage] = tex;
            s_texHidden = false;
            for (void* t : s_boundTex) s_texHidden = s_texHidden || IsHidden(t);
        }
        return real->SetTexture(stage, tex);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override {
        return s_skipPostPass || s_texHidden ? D3D_OK : real->DrawPrimitive(type, start, count);
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* ib) override {
        s_curIndices = ib;
        return real->SetIndices(ib);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT verts, UINT start,
                                                   UINT count) override {
        if (s_skipPostPass || s_texHidden) return D3D_OK;
        HRESULT hr;
        if (!DrawMirrorGlassNative(real, type, base, minIndex, verts, start, count, &hr) &&
            !DrawWheelMarked(real, type, base, minIndex, verts, start, count, &hr))
            hr = real->DrawIndexedPrimitive(type, base, minIndex, verts, start, count);
        AfterModelDraw();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, CONST void* data, UINT stride) override {
        return s_skipPostPass || s_texHidden ? D3D_OK : real->DrawPrimitiveUP(type, count, data, stride);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT verts, UINT count,
                                                     CONST void* indices, D3DFORMAT fmt, CONST void* data,
                                                     UINT stride) override {
        return s_skipPostPass || s_texHidden ? D3D_OK
                                             : real->DrawIndexedPrimitiveUP(type, minIndex, verts, count, indices, fmt, data, stride);
    }

    HRESULT STDMETHODCALLTYPE Present(CONST RECT* src, CONST RECT* dst, HWND wnd, CONST RGNDATA* dirty) override {
        OnPresent();
        double tp = D3D_ProfNow();
        HRESULT hr = real->Present(src, dst, wnd, dirty);
        D3D_ProfAdd(PROF_PRESENT, D3D_ProfNow() - tp);
        ProfFrame();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                            IDirect3DTexture9** tex, HANDLE* shared) override {
        D3DPOOL p = pool;
        DWORD u = usage;
        ConvertPool(p, u, true);
        HRESULT hr = real->CreateTexture(w, h, levels, u, fmt, p, tex, shared);
        if (FAILED(hr) && p != pool) {
            Log("d3d9: dynamic texture %ux%u fmt=%d failed 0x%08lx, retrying static", w, h, fmt, hr);
            hr = real->CreateTexture(w, h, levels, usage, fmt, p, tex, shared);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                  IDirect3DVolumeTexture9** tex, HANDLE* shared) override {
        ConvertPool(pool, usage, true);
        return real->CreateVolumeTexture(w, h, d, levels, usage, fmt, pool, tex, shared);
    }

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                IDirect3DCubeTexture9** tex, HANDLE* shared) override {
        ConvertPool(pool, usage, true);
        return real->CreateCubeTexture(edge, levels, usage, fmt, pool, tex, shared);
    }

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** vb,
                                                 HANDLE* shared) override {
        D3DPOOL p = pool;
        ConvertPool(p, usage, false);
        HRESULT hr = real->CreateVertexBuffer(len, usage, fvf, p, vb, shared);
        if (FAILED(hr)) Log("d3d9: CreateVertexBuffer(%u, usage 0x%lx, pool %d->%d) failed 0x%08lx", len, usage, pool, p, hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** ib,
                                                HANDLE* shared) override {
        D3DPOOL p = pool;
        ConvertPool(p, usage, false);
        HRESULT hr = real->CreateIndexBuffer(len, usage, fmt, p, ib, shared);
        if (FAILED(hr)) Log("d3d9: CreateIndexBuffer(%u, usage 0x%lx, pool %d->%d) failed 0x%08lx", len, usage, pool, p, hr);
        return hr;
    }
};

// Starts OpenXR once, before the game picks its resolution (the headset's
// recommended per-eye size feeds the resolution hook in game.cpp).
bool D3D_EnsureXr() {
    static bool attempted;
    if (attempted || !g_cfg.enabled) return s_xr;
    attempted = true;
    Crash_Initialize();
    LUID luid = {};
    auto create9ex = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
    IDirect3D9Ex* ex = nullptr;
    if (create9ex && SUCCEEDED(create9ex(D3D_SDK_VERSION, &ex))) {
        ex->GetAdapterLUID(D3DADAPTER_DEFAULT, &luid);
        ex->Release();
    }
    s_xr = Xr_Init(luid);
    Log("d3d9: VR %s", s_xr ? "active" : "unavailable - running flat");
    return s_xr;
}

// Sizes the game window for the spectator view: its aspect (16:9 / 32:9 / back
// buffer), limited to SpectatorWidth and the monitor. Present scales the back buffer
// to the client area.
static void FitWindow(HWND wnd, UINT w, UINT h) {
    if (!wnd || !g_cfg.forceWindowed || !w || !h) return;
    s_wnd = wnd;
    s_bbW = w;
    s_bbH = h;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    float maxW = (mi.rcWork.right - mi.rcWork.left) * 0.9f, maxH = (mi.rcWork.bottom - mi.rcWork.top) * 0.9f;
    float aspect = s_xr ? Spectator_WindowAspect((int)w, (int)h) : (float)w / h;
    float cw = s_xr && Spectator_Mode() != SpectatorMode::Eye ? (float)g_cfg.spectatorWidth : (float)w;
    cw = std::min(cw, std::min(maxW, maxH * aspect));
    float ch = cw / aspect;
    RECT r = {0, 0, (LONG)cw, (LONG)ch};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(wnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongA(wnd, GWL_EXSTYLE));
    SetWindowPos(wnd, nullptr, mi.rcWork.left, mi.rcWork.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Log("d3d9: window client %.0fx%.0f", cw, ch);
}

static HRESULT WINAPI hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                     D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    IDirect3D9Ex* ex = (IDirect3D9Ex*)d3d;
    AdjustPresentParams(pp);
    D3DDISPLAYMODEEX mode = {sizeof(mode)};
    if (!pp->Windowed) {
        mode.Width = pp->BackBufferWidth;
        mode.Height = pp->BackBufferHeight;
        mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
        mode.Format = pp->BackBufferFormat;
        mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    IDirect3DDevice9Ex* dev = nullptr;
    HRESULT hr = ex->CreateDeviceEx(adapter, type, focus, flags, pp, pp->Windowed ? nullptr : &mode, &dev);
    Log("d3d9: CreateDeviceEx(adapter %u, flags 0x%lx) -> 0x%08lx", adapter, flags, hr);
    if (FAILED(hr)) return hr;

    s_dev = dev;
    *out = new DeviceWrap(dev);
    D3D_EnsureXr();
    FitWindow(pp->hDeviceWindow ? pp->hDeviceWindow : focus, pp->BackBufferWidth, pp->BackBufferHeight);
    return hr;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
extern "C" {

IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk) {
    auto create9 = Real<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
    if (!g_cfg.enabled) return create9 ? create9(sdk) : nullptr;
    auto create9ex = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
    IDirect3D9Ex* ex = nullptr;
    if (!create9ex || FAILED(create9ex(sdk, &ex))) {
        Log("d3d9: Direct3DCreate9Ex unavailable, VR disabled");
        return create9 ? create9(sdk) : nullptr;
    }
    PatchVtable(ex, kD3D_CreateDevice, (CreateDevice_t)hkCreateDevice, &oCreateDevice);
    D3D_EnsureXr();
    return ex;
}

HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    return Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex")(sdk, out);
}

int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) {
    auto f = Real<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent");
    return f ? f(c, n) : 0;
}
int WINAPI Proxy_D3DPERF_EndEvent() {
    auto f = Real<int(WINAPI*)()>("D3DPERF_EndEvent");
    return f ? f() : 0;
}
void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {
    auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker");
    if (f) f(c, n);
}
void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) {
    auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion");
    if (f) f(c, n);
}
BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame() {
    auto f = Real<BOOL(WINAPI*)()>("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}
void WINAPI Proxy_D3DPERF_SetOptions(DWORD o) {
    auto f = Real<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions");
    if (f) f(o);
}
DWORD WINAPI Proxy_D3DPERF_GetStatus() {
    auto f = Real<DWORD(WINAPI*)()>("D3DPERF_GetStatus");
    return f ? f() : 0;
}

// Marker: lets an .asi copy of this mod see that the d3d9.dll proxy is installed too.
int WINAPI Proxy_NfsmwVr_Loaded() { return 1; }

// For the gameplay mod's F4 menu (found by these exports, like the FFB mod's NfsmwFfb_Setting).
// Live [VR] values: HudWidth, HudDistance, HudHeight, ScreenWidth, ScreenDistance, NearZ, and the
// gameplay mod's SteeringWheelAngle (written every frame, not saved). Null if
// unknown, or in a copy that isn't the one running (an .asi next to the d3d9.dll proxy).
float* __cdecl Proxy_NfsmwVr_Setting(const char* name) {
    if (!s_active || !name) return nullptr;
    if (!strcmp(name, "HudWidth")) return &g_cfg.hudWidth;
    if (!strcmp(name, "HudDistance")) return &g_cfg.hudDistance;
    if (!strcmp(name, "HudHeight")) return &g_cfg.hudHeight;
    if (!strcmp(name, "HudRight")) return &g_cfg.hudRight;
    if (!strcmp(name, "ScreenWidth")) return &g_cfg.screenWidth;
    if (!strcmp(name, "ScreenDistance")) return &g_cfg.screenDistance;
    if (!strcmp(name, "NearZ")) return &g_cfg.nearZ;
    if (!strcmp(name, "SteeringWheelAngle")) return &g_steeringWheelAngle;
    if (!strcmp(name, "HideArms")) return &g_cfg.hideArms;
    if (!strcmp(name, "WheelOverHud")) return &g_cfg.wheelOverHud;
    if (!strcmp(name, "HudPerCar")) return &g_cfg.hudPerCar;
    if (!strcmp(name, "MenuOpen")) return &s_uiMenuOpen;
    if (!strcmp(name, "MirrorBrightness")) return &g_cfg.mirrorBrightness;
    if (!strcmp(name, "MirrorContrast")) return &g_cfg.mirrorContrast;
    if (!strcmp(name, "SeatRight")) return &g_seatInput[0];
    if (!strcmp(name, "SeatUp")) return &g_seatInput[1];
    if (!strcmp(name, "SeatForward")) return &g_seatInput[2];
    if (!strcmp(name, "SeatReset")) return &g_seatInput[3];
    return nullptr;
}

// Same as the recentre key. 0 when no headset session is running.
int __cdecl Proxy_NfsmwVr_Recenter() {
    if (!s_active || !s_xr) return 0;
    Xr_Recenter();
    Log("d3d9: recentred from the gameplay mod's menu");
    return 1;
}

} // extern "C"

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    Log("CRASH at %08lx: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx", c->Eip, c->Eax,
        c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
    DWORD* sp = (DWORD*)c->Esp;
    Log("  stack: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", sp[0], sp[1], sp[2], sp[3], sp[4],
        sp[5], sp[6], sp[7], sp[8], sp[9], sp[10], sp[11]);
    void* game = *(void**)0x982bdc;
    if (game) {
        void** vt = *(void***)game;
        Log("  game device %p vt=%p, real device %p", game, vt, s_dev);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool LoadedAsProxy(HINSTANCE inst) {
    char path[MAX_PATH];
    GetModuleFileNameA(inst, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    return _stricmp(name ? name + 1 : path, "d3d9.dll") == 0;
}

// .asi mode: redirect the game's D3D9.dll!Direct3DCreate9 IAT slot to the proxy export.
static bool PatchImport() {
    void** slot = (void**)0x8904A8;
    HMODULE target = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)*slot, &target) ||
        GetProcAddress(target, "Direct3DCreate9") != *slot) {
        Log("asi mode: Direct3DCreate9 import not found (not speed.exe v1.3?), VR inactive");
        return false;
    }
    s_real = target;
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (void*)&Proxy_Direct3DCreate9;
    VirtualProtect(slot, 4, old, &old);
    char path[MAX_PATH];
    GetModuleFileNameA(target, path, MAX_PATH);
    Log("asi mode: redirected Direct3DCreate9 import (real d3d9: %s)", path);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Installed both as d3d9.dll and as an .asi: the proxy runs, this copy stays out of the
        // way (and out of the log the proxy already has open).
        HMODULE proxy = GetModuleHandleA("d3d9.dll");
        if (!LoadedAsProxy(inst) && proxy && GetProcAddress(proxy, "NfsmwVr_Loaded")) return TRUE;
        LogInit();
        Log("NFSMW VR mod loaded");
        LoadConfig();
        if (g_cfg.enabled && !Game_ValidateHooks()) g_cfg.enabled = false;
        if (g_cfg.enabled && !LoadedAsProxy(inst) && !PatchImport()) g_cfg.enabled = false;
        if (g_cfg.enabled) {
            if (Game_InstallHooks()) {
                s_active = true;
                AddVectoredExceptionHandler(0, CrashLogger);
                Cockpit_Install();
            } else g_cfg.enabled = false;
        }
    }
    return TRUE;
}
