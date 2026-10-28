#include "common.h"
#include "hud_probe.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static_assert(sizeof(HudProbeVertex) == 32, "v1.3 FE vertex stride");

void HudProbeAtlas::Release() {
    if (state_) state_->Release();
    if (target_) target_->Release();
    if (surface_) surface_->Release();
    if (texture_) texture_->Release();
    state_ = nullptr; target_ = surface_ = nullptr; texture_ = nullptr; valid_ = false;
}

HRESULT HudProbeAtlas::Begin(IDirect3DDevice9* dev, D3DMULTISAMPLE_TYPE samples, DWORD quality) {
    valid_ = false; failed_ = false;
    HRESULT hr = S_OK;
    if (!texture_ || samples_ != samples || quality_ != quality) {
        Release(); samples_ = samples; quality_ = quality;
        hr = dev->CreateTexture(Size, Size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                D3DPOOL_DEFAULT, &texture_, nullptr);
        if (SUCCEEDED(hr)) hr = texture_->GetSurfaceLevel(0, &surface_);
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTarget(Size, Size, D3DFMT_A8R8G8B8,
                                                       samples, quality, FALSE, &target_, nullptr);
        if (SUCCEEDED(hr)) hr = dev->CreateStateBlock(D3DSBT_ALL, &state_);
        if (FAILED(hr)) { Release(); return hr; }
    }
    hr = dev->ColorFill(target_, nullptr, 0);
    failed_ = FAILED(hr);
    return hr;
}

HRESULT HudProbeAtlas::Duplicate(IDirect3DDevice9* dev, const HudProbeVertex* quad,
                                 float scaleX, float scaleY, const HudProbeRect& source, bool* outside,
                                 const HudProbeRect* clip, float shiftX) {
    *outside = false;
    if (!target_ || failed_) return E_FAIL;
    float w = source.right - source.left, h = source.bottom - source.top;
    if (!(w > 0 && h > 0 && scaleX > 0 && scaleY > 0) ||
        !std::isfinite(w + h + scaleX + scaleY)) return E_INVALIDARG;
    HudProbeVertex vertices[4];
    memcpy(vertices, quad, sizeof(vertices));
    const float fit = 2.0f / std::max(w, h);
    for (auto& v : vertices) {
        float x = (v.x + 1) / scaleX - shiftX, y = (1 - v.y) / scaleY;
        if (!std::isfinite(x + y)) return E_INVALIDARG;
        *outside |= x < source.left - 0.5f || x > source.right + 0.5f ||
                    y < source.top - 0.5f || y > source.bottom + 0.5f;
        v.x = (x - (source.left + source.right) * 0.5f) * fit;
        v.y = ((source.top + source.bottom) * 0.5f - y) * fit;
    }

    HRESULT hr = state_->Capture();
    if (FAILED(hr)) { failed_ = true; return hr; }
    IDirect3DSurface9 *previous = nullptr, *depth = nullptr;
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {}; DWORD scissorOn = 0;
    hr = dev->GetRenderTarget(0, &previous);
    HRESULT dsHr = dev->GetDepthStencilSurface(&depth);
    if (SUCCEEDED(hr) && FAILED(dsHr) && dsHr != D3DERR_NOTFOUND) hr = dsHr;
    if (SUCCEEDED(hr)) hr = dev->GetViewport(&viewport);
    if (SUCCEEDED(hr)) hr = dev->GetScissorRect(&scissor);
    if (SUCCEEDED(hr)) hr = dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorOn);
    if (FAILED(hr)) {
        if (previous) previous->Release(); if (depth) depth->Release();
        failed_ = true; return hr;
    }
    RECT cell = {0, 0, (LONG)Size, (LONG)Size};
    if (scissorOn) {
        // D3D's scissor is in source viewport pixels. Map it into the same canonical UI space.
        auto mapX = [&](LONG p) { return (LONG)std::floor(((((p - (float)viewport.X) /
            viewport.Width) * 2) / scaleX - shiftX - (source.left + source.right) * 0.5f) * fit * Size / 2 + Size / 2); };
        auto mapY = [&](LONG p) { return (LONG)std::floor(((((p - (float)viewport.Y) /
            viewport.Height) * 2) / scaleY - (source.top + source.bottom) * 0.5f) * fit * Size / 2 + Size / 2); };
        cell = {std::max(0L, mapX(scissor.left)), std::max(0L, mapY(scissor.top)),
                std::min((LONG)Size, mapX(scissor.right)), std::min((LONG)Size, mapY(scissor.bottom))};
    }
    if (clip) {
        // The clip is in layout units, like the source: into the cell the same way as the vertices.
        auto cx = [&](float x) { return (LONG)std::floor((x - (source.left + source.right) * 0.5f) * fit * Size / 2 + Size / 2); };
        auto cy = [&](float y) { return (LONG)std::floor((y - (source.top + source.bottom) * 0.5f) * fit * Size / 2 + Size / 2); };
        cell = {std::max(cell.left, cx(clip->left)), std::max(cell.top, cy(clip->top)),
                std::min(cell.right, cx(clip->right)), std::min(cell.bottom, cy(clip->bottom))};
    }
    if (cell.left < cell.right && cell.top < cell.bottom) {
        hr = dev->SetDepthStencilSurface(nullptr);
        if (SUCCEEDED(hr)) hr = dev->SetRenderTarget(0, target_);
        D3DVIEWPORT9 atlasViewport = {0, 0, Size, Size, 0, 1};
        if (SUCCEEDED(hr)) hr = dev->SetViewport(&atlasViewport);
        if (SUCCEEDED(hr)) hr = dev->SetScissorRect(&cell);
        if (SUCCEEDED(hr)) hr = dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(vertices[0]));
    }
    // Restore targets before the state block, which contains the source viewport.
    HRESULT restore = dev->SetRenderTarget(0, previous);
    if (FAILED(restore) && SUCCEEDED(hr)) hr = restore;
    restore = dev->SetDepthStencilSurface(depth);
    if (FAILED(restore) && SUCCEEDED(hr)) hr = restore;
    restore = state_->Apply();
    if (FAILED(restore) && SUCCEEDED(hr)) hr = restore;
    previous->Release(); if (depth) depth->Release();
    failed_ |= FAILED(hr);
    return hr;
}

HRESULT HudProbeAtlas::End(IDirect3DDevice9* dev) {
    if (!target_ || failed_) { valid_ = false; return E_FAIL; }
    HRESULT hr = dev->StretchRect(target_, nullptr, surface_, nullptr, D3DTEXF_NONE);
    valid_ = SUCCEEDED(hr); return hr;
}

namespace {
constexpr uintptr_t kCall = 0x516685, kAdd = 0x5A46A0, kFeng = 0x91CADC, kFind = 0x516AF0;
constexpr int kMaxNodes = 4096, kMaxRows = 65536;
struct Object {
    uintptr_t address, package;
    uint32_t hash, parent, group, flags, type;
    float x, y, w, h;
    bool speedo;
    int component = -1;
    float ax = 0, ay = 0;
};
struct Frame {
    unsigned draws = 0, inside = 0, recognized = 0, duplicates = 0, skipped = 0, outside = 0;
    unsigned clips = 0, changedClips = 0, cached = 0, nodes = 0;
    double treeUs = 0, duplicateUs = 0;
};
HudProbeAtlas atlas;
HudProbeAtlas components[DASH_EL_COUNT];
HudProbeRect componentSource[DASH_EL_COUNT];
HudProbeRect componentClip[DASH_EL_COUNT];   // what is kept of a part's draws (zero size: all of it)
// The widescreen fix's FixHUD widens the HUD's canvas (its x scale, the constant CanvasScale reads,
// times 3/4 for 16:9: 853 layout units instead of 640). Against the 640-wide layout the left parts
// stay, the centre ones move half the extra width and the right ones all of it (measured: minimap
// 88 -> 88, radar 203 -> 310, speedometer 457 -> 670): componentShift, for the crops. Of that, the
// right group's anchor in the tree carries half (529.7 on the first frame, 636.3 after); the left
// (93.7) and centre (313.5) anchors stay. The rectangles follow the anchors as they are, so a
// part's draws lose what its anchor doesn't carry (drawShift): 0 left, half the extra elsewhere.
float componentShift[DASH_EL_COUNT], drawShift[DASH_EL_COUNT], canvasExtra;
bool directActive, directOk, directReady;
unsigned directFailure;
unsigned componentDraws[DASH_EL_COUNT], completedDraws[DASH_EL_COUNT];
IDirect3DSurface9 *remainingTarget, *remainingSurface;
IDirect3DTexture9* remainingTexture;
D3DSURFACE_DESC remainingDesc = {};
void ReleaseRemaining() {
    if (remainingTarget) remainingTarget->Release();
    if (remainingSurface) remainingSurface->Release();
    if (remainingTexture) remainingTexture->Release();
    remainingTarget = remainingSurface = nullptr; remainingTexture = nullptr;
}
bool BeginRemaining(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d) {
    if (!remainingTarget || d.Width != remainingDesc.Width || d.Height != remainingDesc.Height ||
        d.MultiSampleType != remainingDesc.MultiSampleType || d.MultiSampleQuality != remainingDesc.MultiSampleQuality) {
        ReleaseRemaining(); remainingDesc = d;
        HRESULT hr = dev->CreateTexture(d.Width,d.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT,&remainingTexture,nullptr);
        if (SUCCEEDED(hr)) hr = remainingTexture->GetSurfaceLevel(0,&remainingSurface);
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTarget(d.Width,d.Height,D3DFMT_A8R8G8B8,
                        d.MultiSampleType,d.MultiSampleQuality,FALSE,&remainingTarget,nullptr);
        if (FAILED(hr)) { ReleaseRemaining(); return false; }
    }
    return SUCCEEDED(dev->ColorFill(remainingTarget,nullptr,0));
}
HRESULT DuplicateRemaining(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    IDirect3DSurface9 *rt=nullptr,*ds=nullptr;
    D3DVIEWPORT9 vp = {}; dev->GetViewport(&vp);
    HRESULT hr=dev->GetRenderTarget(0,&rt); dev->GetDepthStencilSurface(&ds);
    if (SUCCEEDED(hr)) hr=dev->SetDepthStencilSurface(nullptr);
    if (SUCCEEDED(hr)) hr=dev->SetRenderTarget(0,remainingTarget);
    if (SUCCEEDED(hr)) hr=dev->SetViewport(&vp);
    if (SUCCEEDED(hr)) hr=dev->DrawPrimitiveUP(type,count,data,stride);
    HRESULT restore=dev->SetRenderTarget(0,rt);
    if (FAILED(restore)) hr=restore;
    restore=dev->SetDepthStencilSurface(ds); if (FAILED(restore)) hr=restore;
    restore=dev->SetViewport(&vp); if (FAILED(restore)) hr=restore;
    if (rt) rt->Release(); if (ds) ds->Release();
    return hr;
}
std::unordered_map<const void*, Object> objects;
std::unordered_set<uintptr_t> visited;
std::vector<Object> tree;
const Object* current;
bool currentCached;
DWORD thread;
Frame frame;
bool installed, active, requested, treeOk, atlasOk;
unsigned frameId, captureId, framesLeft, captureRows, autoCaptures;
uint64_t lastSignature;
double lastCapture;
FILE *drawFile, *frameFile, *objectFile, *clipFile;
float scaleX, scaleY;
HudProbeRect source;
int width, height, fixHud;
DWORD samples, quality;
IDirect3DSurface9* expectedHud;

double Now() {
    LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / f.QuadPart;
}
bool Copy(void* dst, uintptr_t src, size_t bytes) {
    __try { memcpy(dst, (const void*)src, bytes); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> T Read(uintptr_t p) { T v = {}; Copy(&v, p, sizeof(v)); return v; }
uint32_t Hash(const char* name) {
    uint32_t h = ~0u;
    while (*name) { unsigned char c = (unsigned char)*name++; if (c >= 'a' && c <= 'z') c -= 32; h = h * 33 + c; }
    return h;
}
bool IsSpeedo(uint32_t h) {
    // Groups registered by Speedometer, Tachometer, SpeedBreakerMeter, TurboMeter and
    // NitrousGauge. Gameplay's heat/warning clones remain descendants of the meter group.
    return h == Hash("SpeedometerGroup") || h == Hash("GaugeCluster") ||
           h == Hash("Speedbreaker_Meter_Group") || h == Hash("TURBO_GROUP") ||
           h == 0x87c38e97 || h == 0x82d60021 ||
           h == Hash("Speedbreaker_Meter_Multi_Image") || h == 0x27ddf583 || h == 0xedfb6d37;
}
// l..b on the canvas (canvasW x canvasH); s in the layout, shift units left of the canvas (FixHUD).
bool VisibleOutside(float l,float t,float r,float b,const HudProbeRect& s,float canvasW,float canvasH,float shift=0) {
    l=std::max(0.0f,l); t=std::max(0.0f,t); r=std::min(canvasW,r); b=std::min(canvasH,b);
    if (r<=l || b<=t) return false; // the game's viewport already discards off-screen animation quads
    l-=shift; r-=shift;
    return l<s.left-.5f || t<s.top-.5f || r>s.right+.5f || b>s.bottom+.5f;
}
int Component(uint32_t h) {
    if (h == 0x82d60021) return DASH_EL_PURSUIT; // activation/status bar lives at bottom centre
    if (IsSpeedo(h)) return DASH_EL_SPEEDO;
    if (h == 0x1603009e || h == 0x00a1a862 || h == Hash("TRACK_MAP") || h == Hash("PLAYERCARINDICATOR") ||
        h == Hash("PLAYERCARINDICATOR2") || h == Hash("MINIMAP_NORTH_INDICATOR")) return DASH_EL_MAP;
    if (h == 0x062743f5) return DASH_EL_RADAR;
    // The radar's pill (its capsule backing, 5630E0B9 in two halves at x 192..448, y 21..108 of the
    // layout): the radar's own source rectangle holds it, and it is part of how the radar reads.
    if (h == 0x9ee06631) return DASH_EL_RADAR;
    if (h == 0x7b422ba3 || h == 0x84a226ec || h == 0xe0b1430b) return DASH_EL_PURSUIT;
    if (h == 0xde89e070 || h == Hash("RaceInformationGroup") || h == Hash("LeaderBoardGroup") ||
        h == 0x2269d232) return DASH_EL_BOARD;
    if (h == 0xe0ba07ec) return DASH_EL_MSG;
    return -1;
}
// Bounds in the package's 640x480 layout, relative to its stable layout containers.
// Animated map tiles can exceed their mask; the cell scissor, rather than their quad bounds,
// limits visible content. These rectangles never grow in response to a draw.
bool ResolveSources() {
    const Object *left=nullptr,*right=nullptr,*centre=nullptr;
    for (const auto& o : tree) {
        if (o.hash==0x157c5b37) left=&o;
        if (o.hash==0x5d0101f1) right=&o;
        if (o.hash==0xab9c1d4d) centre=&o;
    }
    if (!left || !right || !centre) return false;
    auto rect=[](const Object* o,float l,float t,float r,float b) {
        return HudProbeRect{o->ax+l,o->ay+t,o->ax+r,o->ay+b};
    };
    componentSource[DASH_EL_MAP] = rect(left,-120,0,128,258);
    // Up and right of the gauge: room for the gameplay mod's ABS / TC icons (above the car icon,
    // TC right of ABS) moved by its [HUD] AidIconRight / AidIconUp, about 50 units either way.
    componentSource[DASH_EL_SPEEDO] = rect(right,-118,-130,200,222);
    componentSource[DASH_EL_RADAR] = rect(centre,-130,-260,142,12);
    componentSource[DASH_EL_PURSUIT] = rect(centre,-132,136,144,256);
    componentSource[DASH_EL_BOARD] = rect(right,-110,-282,120,-120);
    componentSource[DASH_EL_MSG] = rect(centre,-214,-126,226,-24);
    // The radar's strip (the dome on a dotted bar, x 230..412, y 86..108 with the centre group
    // at 313, 219) is the bottom edge of one texture with the HUD rear-view mirror's empty frame
    // above it (9EE06631): only the strip is kept.
    for (auto& c : componentClip) c = HudProbeRect{};
    componentClip[DASH_EL_RADAR] = rect(centre,-84,-134,100,-110);
    static bool logged;
    if (!logged) { logged=true; Log("hud: groups at left %.1f, centre %.1f %.1f, right %.1f (canvas %.1f wider), radar strip kept at %.0f..%.0f x %.0f..%.0f",
        left->ax, centre->ax, centre->ay, right->ax, canvasExtra,
        componentClip[DASH_EL_RADAR].left, componentClip[DASH_EL_RADAR].right, componentClip[DASH_EL_RADAR].top, componentClip[DASH_EL_RADAR].bottom); }
    return true;
}
uintptr_t FindPackage(uintptr_t feng, const char* name) {
    __try { return (uintptr_t)((void*(__thiscall*)(void*, const char*))kFind)((void*)feng, name); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void Walk(uintptr_t list, uintptr_t package, uint32_t parent, uint32_t group, bool speedo, unsigned depth, int component = -1, float ax = 320, float ay = 240) {
    uintptr_t p = Read<uintptr_t>(list + 8);
    while (p) {
        if (depth > 16 || tree.size() >= kMaxNodes || !visited.insert(p).second) { treeOk = false; return; }
        uint8_t bytes[0x60];
        if (!Copy(bytes, p, sizeof(bytes))) { treeOk = false; return; }
        auto u = [&](int off) { uint32_t value; memcpy(&value, bytes + off, 4); return value; };
        Object o = {p, package, u(0x10), parent, group, u(0x1c), u(0x18), 0, 0, 0, 0, speedo};
        o.speedo |= IsSpeedo(o.hash);
        o.component = Component(o.hash);
        if (o.component < 0) o.component = component;
        if (o.type == 5 && !parent) o.group = o.hash;
        float data[16] = {};
        if (!Copy(data, u(0x2c), sizeof(data))) { treeOk = false; return; }
        o.x = data[7]; o.y = data[8]; o.w = data[14]; o.h = data[15];
        o.ax = ax + o.x; o.ay = ay + o.y;
        tree.push_back(o); objects[(void*)p] = o;
        if (o.type == 5) Walk(p + 0x5c, package, o.hash, o.group, o.speedo, depth + 1, o.component, o.ax, o.ay);
        p = u(4);
    }
}
void CloseCapture() {
    for (FILE** f : {&drawFile, &frameFile, &objectFile, &clipFile}) if (*f) { fclose(*f); *f = nullptr; }
    framesLeft = 0;
}
void StartCapture(const char* reason) {
    CloseCapture();
    char base[MAX_PATH], file[MAX_PATH];
    do { snprintf(base, sizeof(base), "%snfsmw_hudprobe_%lu_%03u", GameDir(), GetCurrentProcessId(), captureId++);
         snprintf(file, sizeof(file), "%s_frames.csv", base);
    } while (GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES && captureId < 10000);
    frameFile = fopen(file, "w");
    snprintf(file, sizeof(file), "%s_draws.csv", base); drawFile = fopen(file, "w");
    snprintf(file, sizeof(file), "%s_objects.csv", base); objectFile = fopen(file, "w");
    snprintf(file, sizeof(file), "%s_clips.csv", base); clipFile = fopen(file, "w");
    if (!frameFile || !drawFile || !objectFile || !clipFile) { CloseCapture(); Log("hudprobe: capture open failed"); return; }
    fprintf(frameFile, "frame,width,height,fixhud,samples,quality,nodes,tree_ok,draws,inside,recognized,duplicates,skipped,outside,clips,changed_clips,cached,tree_cpu_us,duplicate_cpu_us,scale_x,scale_y\n");
    fprintf(drawFile, "frame,object,package,hash,parent,group,type,flags,cached,speedo,draw_kind,primitive,count,stride,fvf,vs,ps,z,stencil,scissor,alpha,srcblend,dstblend,colorwrite,viewport_x,viewport_y,viewport_w,viewport_h,min_x,min_y,max_x,max_y,canvas_l,canvas_t,canvas_r,canvas_b,duplicate_hr,outside\n");
    fprintf(objectFile, "frame,package,object,hash,parent,group,type,flags,speedo,x,y,w,h\n");
    fprintf(clipFile, "frame,object,aligned,output_vertices,changed,input_l,input_t,input_r,input_b,output_l,output_t,output_r,output_b\n");
    framesLeft = 45; captureRows = 0; lastCapture = Now();
    Log("hudprobe: %s capture %s (45 HUD frames, at most %d detail rows)", reason, base, kMaxRows);
}
bool PatchCall(uintptr_t site, uintptr_t target, const void* hook) {
    uint8_t bytes[5];
    if (!Copy(bytes, site, 5) || bytes[0] != 0xe8) return false;
    int32_t delta; memcpy(&delta, bytes + 1, 4);
    if (site + 5 + delta != target) return false;
    DWORD old;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(site + 1) = (int32_t)((uintptr_t)hook - site - 5);
    VirtualProtect((void*)site, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    return true;
}
void __fastcall Add(void* self, void*, void* object) {
    const Object* previous = current;
    const bool previousCached = currentCached;
    Object unknown = {};
    if (active && GetCurrentThreadId() == thread) {
        auto it = objects.find(object);
        unknown.address = (uintptr_t)object;
        unknown.package = Read<uintptr_t>(0x91cfac);
        unknown.hash = Read<uint32_t>((uintptr_t)object + 0x10);
        unknown.type = Read<uint32_t>((uintptr_t)object + 0x18);
        unknown.flags = Read<uint32_t>((uintptr_t)object + 0x1c);
        current = it != objects.end() && it->second.package == unknown.package ? &it->second : &unknown;
        currentCached = Read<uintptr_t>((uintptr_t)object + 0x58) && !(unknown.flags & 0x2000000);
        frame.cached += currentCached;
    }
    ((void(__thiscall*)(void*, void*))kAdd)(self, object);
    current = previous;
    currentCached = previousCached;
}
using ClipFn = unsigned(__thiscall*)(void*, void*, void*, void*, void*, void*, void*, void*);
std::array<float, 4> Bounds(const float* v, unsigned n) {
    std::array<float, 4> b = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (unsigned i = 0; i < n; i++) { b[0] = std::min(b[0], v[i*3]); b[1] = std::min(b[1], v[i*3+1]);
                                     b[2] = std::max(b[2], v[i*3]); b[3] = std::max(b[3], v[i*3+1]); }
    return b;
}
unsigned Clip(bool aligned, void* self, void* clip, void* inV, void* inUV, void* inColor,
              void* outV, void* outUV, void* outColor) {
    float before[12] = {}; bool trace = active && current && GetCurrentThreadId() == thread;
    if (trace) Copy(before, (uintptr_t)inV, sizeof(before));
    unsigned n = ((ClipFn)(aligned ? 0x584800 : 0x582d20))(self, clip, inV, inUV, inColor, outV, outUV, outColor);
    if (trace) {
        float after[24] = {}; Copy(after, (uintptr_t)outV, std::min(n, 8u) * 3 * sizeof(float));
        bool changed = n != 4;
        if (!changed) for (unsigned i = 0; i < 4; i++) {
            bool same = false;
            for (unsigned j = 0; j < 4; j++) same |= std::fabs(before[i*3] - after[j*3]) < 0.01f &&
                                                                    std::fabs(before[i*3+1] - after[j*3+1]) < 0.01f;
            changed |= !same;
        }
        frame.clips++; frame.changedClips += changed;
        if (clipFile && captureRows++ < kMaxRows) {
            auto a = Bounds(before, 4), b = Bounds(after, std::min(n, 8u));
            fprintf(clipFile, "%u,%08X,%d,%u,%d,%g,%g,%g,%g,%g,%g,%g,%g\n", frameId,
                (unsigned)current->address, aligned, n, changed, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
    }
    return n;
}
unsigned __fastcall ClipAligned(void* self, void*, void* c, void* a, void* b, void* d, void* e, void* f, void* g) {
    return Clip(true, self, c, a, b, d, e, f, g);
}
unsigned __fastcall ClipGeneral(void* self, void*, void* c, void* a, void* b, void* d, void* e, void* f, void* g) {
    return Clip(false, self, c, a, b, d, e, f, g);
}
float CanvasScale(uintptr_t instruction) {
    uint8_t op[6];
    if (!Copy(op, instruction, 6) || op[0] != 0xd8 || op[1] != 0x0d) return 0;
    uintptr_t address; memcpy(&address, op + 2, 4);
    float s = Read<float>(address); return std::isfinite(s) && s > 0 && s < 1 ? s : 0;
}
} // namespace

void HudProbe_Install() {
    if ((!g_cfg.hudProbe && g_cfg.dashRenderer < 0.5f) || installed) return;
    installed = PatchCall(kCall, kAdd, &Add);
    if (!installed) { Log("hudprobe: incompatible AddToRenderList call, prototype inactive"); return; }
    bool a = PatchCall(0x5917bc, 0x584800, &ClipAligned);
    bool b = PatchCall(0x5917c7, 0x582d20, &ClipGeneral);
    Log("hud: component renderer installed; CPU clip trace aligned=%d general=%d", a, b);
}

void HudProbe_Begin(IDirect3DDevice9* dev, IDirect3DSurface9* hud, int w, int h, bool allowDirect) {
    directReady = directActive = false;
    if ((!g_cfg.hudProbe && g_cfg.dashRenderer < 0.5f) || !installed) return;
    thread = GetCurrentThreadId(); current = nullptr; currentCached = false; active = true; frame = {}; frameId++;
    width = w; height = h; expectedHud = hud;
    D3DSURFACE_DESC desc = {}; hud->GetDesc(&desc); samples = desc.MultiSampleType; quality = desc.MultiSampleQuality;
    scaleX = CanvasScale(0x6d12c0); scaleY = CanvasScale(0x6d12d3);
    source = {g_cfg.hudProbeSource[0], g_cfg.hudProbeSource[1], g_cfg.hudProbeSource[2], g_cfg.hudProbeSource[3]};
    double start = Now(); objects.clear(); visited.clear(); tree.clear(); treeOk = true;
    uintptr_t feng = Read<uintptr_t>(kFeng);
    uint64_t signature = 1469598103934665603ull;
    if (feng) for (const char* name : {"HUD_SingleRace.fng", "HUD_Drag.fng"}) {
        uintptr_t pkg = FindPackage(feng, name);
        if (pkg) Walk(pkg + 0x44, pkg, 0, 0, false, 0);
    }
    for (const Object& o : tree) {
        signature ^= o.address; signature *= 1099511628211ull;
        signature ^= o.hash ^ (o.flags & 1); signature *= 1099511628211ull;
    }
    frame.nodes = (unsigned)tree.size(); frame.treeUs = (Now() - start) * 1e6;
    if (!treeOk) objects.clear();
    if (g_cfg.hudProbe && (requested || (signature != lastSignature && autoCaptures < 6 && Now() - lastCapture > 5))) {
        char ini[MAX_PATH]; ConfigFile("NFSMostWanted.WidescreenFix.ini", ini, false);
        fixHud = (int)GetPrivateProfileIntA("MAIN", "FixHUD", -1, ini);
        StartCapture(requested ? "F9" : "UI tree change"); if (!requested) autoCaptures++;
        requested = false;
    }
    lastSignature = signature;
    if (objectFile) for (const Object& o : tree) if (captureRows++ < kMaxRows)
        fprintf(objectFile, "%u,%08X,%08X,%08X,%08X,%08X,%u,%08X,%d,%g,%g,%g,%g\n", frameId,
            (unsigned)o.package, (unsigned)o.address, o.hash, o.parent, o.group, o.type, o.flags, o.speedo, o.x, o.y, o.w, o.h);
    atlasOk = g_cfg.hudProbe && SUCCEEDED(atlas.Begin(dev, desc.MultiSampleType, desc.MultiSampleQuality));
    char ini[MAX_PATH]; ConfigFile("NFSMostWanted.WidescreenFix.ini",ini,false);
    fixHud = (int)GetPrivateProfileIntA("MAIN","FixHUD",0,ini);
    // The canvas's extra width over the 640-unit layout (0 without FixHUD); a narrower one is
    // nothing we know, and is left to the crops.
    float extra = scaleX > 0 ? 2 / scaleX - 640 : -1;
    canvasExtra = extra > 0.5f ? extra : 0;
    for (int e=0;e<DASH_EL_COUNT;e++)
        componentShift[e] = e==DASH_EL_MAP ? 0 : e==DASH_EL_SPEEDO || e==DASH_EL_BOARD ? canvasExtra : canvasExtra/2,
        drawShift[e] = e==DASH_EL_MAP ? 0 : canvasExtra/2;
    directActive = allowDirect && g_cfg.dashRenderer > 0.5f && treeOk && extra > -0.5f && ResolveSources();
    directOk = directActive; directFailure = 0;
    memset(componentDraws,0,sizeof(componentDraws));
    if (directActive) {
        directOk = BeginRemaining(dev,desc);
        for (auto& c : components) directOk &= SUCCEEDED(c.Begin(dev,desc.MultiSampleType,desc.MultiSampleQuality));
    }
}

void HudProbe_Draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (!active || GetCurrentThreadId() != thread) return;
    frame.draws++; frame.inside += current != nullptr;
    const bool speedo = current && current->speedo; frame.recognized += speedo;
    DWORD fvf = 0; dev->GetFVF(&fvf);
    bool format = type == D3DPT_TRIANGLEFAN && count == 2 && stride == sizeof(HudProbeVertex) &&
                  (fvf == 0x20142 || fvf == 0x10142 || fvf == 0x142 || fvf == 0x242);
    HudProbeVertex quad[4] = {};
    if (format) memcpy(quad, data, sizeof(quad));
    bool outside = false; HRESULT hr = S_FALSE;
    if (speedo && atlasOk && treeOk && format && scaleX && scaleY) {
        IDirect3DSurface9* target = nullptr; dev->GetRenderTarget(0, &target);
        DWORD z = 0, stencil = 0; dev->GetRenderState(D3DRS_ZENABLE, &z); dev->GetRenderState(D3DRS_STENCILENABLE, &stencil);
        if (target == expectedHud && !z && !stencil) {
            double t = Now(); hr = atlas.Duplicate(dev, quad, scaleX, scaleY, source, &outside);
            frame.duplicateUs += (Now() - t) * 1e6;
            if (SUCCEEDED(hr)) frame.duplicates++; else atlasOk = false;
            frame.outside += outside;
        }
        if (target) target->Release();
    }
    if (directActive && directOk) {
        IDirect3DSurface9* target=nullptr; dev->GetRenderTarget(0,&target);
        if (target==expectedHud) {
            DWORD z=0,stencil=0; dev->GetRenderState(D3DRS_ZENABLE,&z); dev->GetRenderState(D3DRS_STENCILENABLE,&stencil);
            int element = current ? current->component : -1;
            if (z || stencil || (element>=0 && element<DASH_EL_COUNT && !format)) { directOk=false; directFailure=1; }
            else if (element==DASH_EL_COUNT) { /* surround replaced by the physical display */ }
            else if (element>=0) {
                bool clipped=false;
                const HudProbeRect& keep=componentClip[element];
                bool cut=keep.right>keep.left && keep.bottom>keep.top;   // outside the clip is meant to go
                const float shift=drawShift[element];
                directOk = SUCCEEDED(components[element].Duplicate(dev,quad,scaleX,scaleY,componentSource[element],&clipped,cut?&keep:nullptr,shift));
                if (!directOk) directFailure=2;
                if (clipped && element!=DASH_EL_MAP && !cut) {
                    float l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;
                    for (const auto& v:quad) { float x=(v.x+1)/scaleX,y=(1-v.y)/scaleY;
                        l=std::min(l,x); t=std::min(t,y); r=std::max(r,x); b=std::max(b,y); }
                    if (VisibleOutside(l,t,r,b,componentSource[element],2/scaleX,2/scaleY,shift)) {
                        directOk=false; directFailure=4;
                        static int logged;
                        const HudProbeRect& s=componentSource[element];
                        if (logged++ < 3) Log("hud: part %d drawn outside its rectangle (object %08X): %.1f..%.1f x %.1f..%.1f, "
                            "rectangle %.1f..%.1f x %.1f..%.1f, shift %.1f", element, current ? current->hash : 0,
                            l-shift, r-shift, t, b, s.left, s.right, s.top, s.bottom, shift);
                    }
                }
                componentDraws[element]++;
            } else directOk = SUCCEEDED(DuplicateRemaining(dev,type,count,data,stride));
        }
        if (target) target->Release();
    }
    frame.skipped += speedo && hr == S_FALSE;
    if (!drawFile || captureRows++ >= kMaxRows) return;
    DWORD state[7] = {};
    const D3DRENDERSTATETYPE keys[] = {D3DRS_ZENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
        D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_COLORWRITEENABLE};
    for (int i = 0; i < 7; i++) dev->GetRenderState(keys[i], &state[i]);
    IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
    dev->GetVertexShader(&vs); dev->GetPixelShader(&ps);
    D3DVIEWPORT9 vp = {}; dev->GetViewport(&vp);
    float l = INFINITY, t = INFINITY, r = -INFINITY, b = -INFINITY;
    if (format) for (const auto& v : quad) { l = std::min(l, v.x); t = std::min(t, v.y); r = std::max(r, v.x); b = std::max(b, v.y); }
    const Object o = current ? *current : Object{};
    fprintf(drawFile, "%u,%08X,%08X,%08X,%08X,%08X,%u,%08X,%d,%d,up,%d,%u,%u,%08lX,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%g,%g,%g,%g,%g,%g,%g,%g,%08lX,%d\n",
        frameId, (unsigned)o.address, (unsigned)o.package, o.hash, o.parent, o.group, o.type, o.flags,
        currentCached, speedo,
        type, count, stride, fvf, vs != nullptr, ps != nullptr, state[0], state[1], state[2], state[3], state[4], state[5], state[6],
        vp.X, vp.Y, vp.Width, vp.Height, l, t, r, b,
        scaleX ? (l+1)/scaleX : NAN, scaleY ? (1-b)/scaleY : NAN,
        scaleX ? (r+1)/scaleX : NAN, scaleY ? (1-t)/scaleY : NAN, hr, outside);
    if (vs) vs->Release(); if (ps) ps->Release();
}

void HudProbe_OtherDraw(IDirect3DDevice9* dev) { if (active && GetCurrentThreadId() == thread) {
    frame.draws++; frame.inside += current != nullptr;
    // This path has no replay payload. A complete ordinary HUD still exists for a whole-frame fallback.
    IDirect3DSurface9* rt=nullptr; dev->GetRenderTarget(0,&rt);
    if (rt==expectedHud) { directOk=false; directFailure=3; }
    if (rt) rt->Release();
} }
void HudProbe_End(IDirect3DDevice9* dev) {
    if (!active) return;
    active = false; current = nullptr; expectedHud = nullptr;
    if (atlasOk) atlas.End(dev);
    directReady = directActive && directOk;
    if (directReady) {
        for (auto& c : components) directReady &= SUCCEEDED(c.End(dev));
        directReady &= SUCCEEDED(dev->StretchRect(remainingTarget,nullptr,remainingSurface,nullptr,D3DTEXF_NONE));
    }
    memcpy(completedDraws,componentDraws,sizeof(completedDraws));
    static int lastPath=-1;
    if (lastPath != (int)directReady) { lastPath=directReady;
        Log("hud: %s, target %dx%d, FixHUD=%d, cells=%u/%u/%u/%u/%u/%u, fallback reason=%u",
            directReady?"direct components":"complete HUD crop fallback",width,height,fixHud,
            completedDraws[0],completedDraws[1],completedDraws[2],completedDraws[3],completedDraws[4],completedDraws[5],directFailure);
    }
    if (frameFile) {
        fprintf(frameFile, "%u,%d,%d,%d,%lu,%lu,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%.3f,%g,%g\n", frameId,
            width, height, fixHud, samples, quality, frame.nodes, treeOk, frame.draws, frame.inside, frame.recognized,
            frame.duplicates, frame.skipped, frame.outside, frame.clips, frame.changedClips, frame.cached, frame.treeUs,
            frame.duplicateUs, scaleX, scaleY);
        if (!--framesLeft) { CloseCapture(); Log("hudprobe: bounded capture complete"); }
    }
}
void HudProbe_RequestCapture() { if (g_cfg.hudProbe) requested = true; }
void HudProbe_Release() { active = false; current = nullptr; expectedHud = nullptr; atlas.Release(); for (auto& c : components) c.Release(); ReleaseRemaining(); directReady=directActive=false; CloseCapture(); }
IDirect3DTexture9* HudProbe_Texture() { return g_cfg.hudProbe ? atlas.Texture() : nullptr; }

bool HudComponents_Ready() { return directReady; }
IDirect3DTexture9* HudComponents_Texture(int element) {
    return directReady && element>=0 && element<DASH_EL_COUNT && completedDraws[element] ? components[element].Texture() : nullptr;
}
IDirect3DSurface9* HudComponents_Remaining() { return directReady ? remainingSurface : nullptr; }
float HudCanvasExtra() { return canvasExtra; }
float HudCanvasShift(int element) { return element>=0 && element<DASH_EL_COUNT ? componentShift[element] : 0; }
bool HudComponents_Coverage(int element, float out[4]) {
    if (!directReady || element<0 || element>=DASH_EL_COUNT || !(scaleX>0 && scaleY>0)) return false;
    // The cell is the square around the source rectangle (Duplicate's fit), in the 640 x 480 layout
    // (FixHUD's widening taken off): shares of the HUD picture as it is without FixHUD, the picture
    // the crops are shares of.
    const HudProbeRect& s=componentSource[element];
    float cx=(s.left+s.right)*.5f, cy=(s.top+s.bottom)*.5f, half=std::max(s.right-s.left,s.bottom-s.top)*.5f;
    cx+=drawShift[element]-componentShift[element];   // what the anchor carries of FixHUD's move
    out[0]=(cx-half)/640; out[1]=(cy-half)/480;
    out[2]=(cx+half)/640; out[3]=(cy+half)/480;
    return true;
}
