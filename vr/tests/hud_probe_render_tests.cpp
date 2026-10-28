#include "../src/hud_probe.cpp"
#include <cassert>
#include <cstdarg>
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <string>
#include <map>

Config g_cfg;
void Log(const char* fmt, ...) { va_list args; va_start(args, fmt); vprintf(fmt, args); puts(""); va_end(args); }
const char* GameDir() { return "..\\dist\\validation\\"; }
void ConfigFile(const char*, char* out, bool) { strcpy(out, "missing-test.ini"); }

static void Good(HRESULT hr) { if (FAILED(hr)) { printf("D3D failure %08lX\n", hr); abort(); } }
static std::vector<DWORD> Pixels(IDirect3DDevice9* dev, IDirect3DSurface9* surface) {
    D3DSURFACE_DESC desc; Good(surface->GetDesc(&desc));
    IDirect3DSurface9* sys = nullptr;
    Good(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    Good(dev->GetRenderTargetData(surface, sys));
    D3DLOCKED_RECT lock; Good(sys->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    std::vector<DWORD> p(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; y++) memcpy(p.data() + y * desc.Width, (BYTE*)lock.pBits + y * lock.Pitch, desc.Width * 4);
    Good(sys->UnlockRect()); sys->Release(); return p;
}
static void Save(const char* path, const std::vector<DWORD>& p) {
    FILE* f = fopen(path, "wb"); assert(f);
    BITMAPFILEHEADER file = {}; BITMAPINFOHEADER info = {};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + (DWORD)p.size() * 4;
    info.biSize = sizeof(info); info.biWidth = HudProbeAtlas::Size; info.biHeight = -(LONG)HudProbeAtlas::Size;
    info.biPlanes = 1; info.biBitCount = 32;
    fwrite(&file, sizeof(file), 1, f); fwrite(&info, sizeof(info), 1, f); fwrite(p.data(), 4, p.size(), f); fclose(f);
}
static void TreeCases() {
    // The live-tree walker must handle a nested replacement without a change in child count.
    std::array<BYTE, 0x70> pkg = {}, group = {}, child = {}, replacement = {};
    float data[16] = {};
    auto ptr = [](BYTE* p, uintptr_t value) { memcpy(p, &value, 4); };
    auto init = [&](auto& o, uint32_t hash, uint32_t type) {
        ptr(o.data()+0x10, hash); ptr(o.data()+0x18, type); ptr(o.data()+0x2c, (uintptr_t)data);
    };
    init(group, Hash("GaugeCluster"), 5); init(child, 123, 1); init(replacement, 456, 1);
    ptr(pkg.data()+0x4c, (uintptr_t)group.data()); ptr(group.data()+0x64, (uintptr_t)child.data());
    auto walk = [&]() { tree.clear(); objects.clear(); visited.clear(); treeOk = true;
                       Walk((uintptr_t)pkg.data()+0x44, (uintptr_t)pkg.data(), 0, 0, false, 0); };
    walk(); assert(treeOk && objects.size() == 2 && objects.at(child.data()).speedo);
    ptr(group.data()+0x64, (uintptr_t)replacement.data()); walk();
    assert(treeOk && !objects.count(child.data()) && objects.at(replacement.data()).hash == 456);
    ptr(replacement.data()+4, (uintptr_t)group.data()); walk(); assert(!treeOk);
    assert(Hash("GaugeCluster") == Hash("GAUGECLUSTER"));
    assert(IsSpeedo(Hash("Speedbreaker_Meter_Group")) && IsSpeedo(Hash("TURBO_GROUP")));
    assert(IsSpeedo(0x87c38e97) && IsSpeedo(0x82d60021));
    init(group, Hash("Speedbreaker_Meter_Group"), 5); ptr(replacement.data()+4, 0); walk();
    assert(treeOk && objects.at(replacement.data()).speedo);
    init(group, Hash("UnrelatedGroup"), 5);
    init(replacement, Hash("Speedbreaker_Meter_Multi_Image"), 12); walk();
    assert(treeOk && !objects.at(group.data()).speedo && objects.at(replacement.data()).speedo);
    assert(!VisibleOutside(400,-200,450,-150,{400,0,500,100},640,480));
    assert(VisibleOutside(400,0,450,150,{400,0,500,100},640,480));
    assert(Component(0x82d60021)==DASH_EL_PURSUIT);
    assert(Component(0x00a1a862)==DASH_EL_MAP); // procedural minimap markers, outside the frame group
    assert(Component(0x9ee06631)==DASH_EL_COUNT); // camera-dependent surround is not an instrument
    assert(Component(0xde89e070)==DASH_EL_BOARD && Component(0x2269d232)==DASH_EL_BOARD);
    assert(Component(0x7b422ba3)==DASH_EL_PURSUIT && Component(0xe0ba07ec)==DASH_EL_MSG);
    puts("live tree: nested replacement, component inheritance and cycle rejection passed");
}
static std::vector<std::string> Columns(const std::string& line) {
    std::vector<std::string> out; std::stringstream s(line); std::string v;
    while (std::getline(s,v,',')) out.push_back(v); return out;
}
static int Replay(const char* prefix) {
    std::map<unsigned,std::vector<Object>> frames;
    std::ifstream input(std::string(prefix)+"_objects.csv"); assert(input);
    std::string line; std::getline(input,line);
    while (std::getline(input,line)) {
        auto c=Columns(line); if (c.size()!=13) continue;
        Object o={}; o.address=std::stoul(c[2],nullptr,16); o.hash=std::stoul(c[3],nullptr,16);
        o.parent=std::stoul(c[4],nullptr,16); o.type=std::stoul(c[6]);
        o.x=std::stof(c[9]); o.y=std::stof(c[10]); o.ax=320+o.x; o.ay=240+o.y;
        frames[std::stoul(c[0])].push_back(o);
    }
    std::ifstream draws(std::string(prefix)+"_draws.csv"); assert(draws); std::getline(draws,line);
    unsigned previous=~0u, checked=0, outside=0, counts[6]={};
    std::unordered_map<uintptr_t,int> membership;
    bool resolved=false; std::unordered_set<uint32_t> reported;
    while (std::getline(draws,line)) {
        auto c=Columns(line); if(c.size()!=38) continue;
        unsigned f=std::stoul(c[0]);
        if (f!=previous) {
            previous=f; tree=frames[f]; resolved=ResolveSources(); membership.clear();
            std::unordered_map<uint32_t,const Object*> groups;
            for (const auto& o:tree) if(o.type==5) groups[o.hash]=&o;
            for (const auto& o:tree) {
                int e=Component(o.hash); uint32_t parent=o.parent;
                for(int depth=0;e<0 && parent && depth<16;depth++) {
                    e=Component(parent); auto it=groups.find(parent);
                    parent=it!=groups.end()?it->second->parent:0;
                }
                membership[o.address]=e;
            }
        }
        if (!resolved) continue;
        auto it=membership.find(std::stoul(c[1],nullptr,16));
        if(it==membership.end() || it->second<0 || it->second>=DASH_EL_COUNT) continue;
        int e=it->second; counts[e]++; checked++;
        if(e==DASH_EL_MAP) continue; // map polygons intentionally extend past their circular UV mask
        float l=std::stof(c[32]),t=std::stof(c[33]),r=std::stof(c[34]),b=std::stof(c[35]);
        const auto& s=componentSource[e];
        if(VisibleOutside(l,t,r,b,s,640,480)) {
            outside++; uint32_t hash=std::stoul(c[3],nullptr,16);
            if(reported.insert(hash).second) printf("outside element %d hash %08X: %g,%g,%g,%g source %g,%g,%g,%g\n",
                                  e,hash,l,t,r,b,s.left,s.top,s.right,s.bottom);
        }
    }
    printf("capture replay: %u component draws, counts %u/%u/%u/%u/%u/%u, outside %u\n",checked,
                counts[0],counts[1],counts[2],counts[3],counts[4],counts[5],outside);
    return checked ? (outside?1:0) : 2;
}
int main(int argc,char** argv) {
    if(argc==3 && !strcmp(argv[1],"--replay")) return Replay(argv[2]);

    TreeCases();
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(nullptr); wc.lpszClassName = "HudProbeTest";
    assert(RegisterClassA(&wc));
    HWND wnd = CreateWindowA(wc.lpszClassName, "HUD probe test", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    assert(wnd);
    IDirect3D9Ex* api = nullptr; Good(Direct3DCreate9Ex(D3D_SDK_VERSION, &api));
    D3DPRESENT_PARAMETERS pp = {}; pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.BackBufferCount = 1; pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
    IDirect3DDevice9Ex* dev = nullptr;
    Good(api->CreateDeviceEx(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &dev));
    IDirect3DSurface9 *original = nullptr, *depth = nullptr;
    Good(dev->GetRenderTarget(0, &original));
    Good(dev->CreateDepthStencilSurface(800, 600, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr));
    Good(dev->SetDepthStencilSurface(depth));
    D3DMATRIX identity = {}; identity._11 = identity._22 = identity._33 = identity._44 = 1;
    for (auto type : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION}) Good(dev->SetTransform(type, &identity));
    Good(dev->SetFVF(0x20142));
    Good(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    Good(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    Good(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    Good(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    Good(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    Good(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll"); assert(compiler);
    auto compile = (pD3DCompile)GetProcAddress(compiler, "D3DCompile"); assert(compile);
    const char* shader = R"(
struct V { float3 pos : POSITION0; float4 color : COLOR0; float2 uv : TEXCOORD0; float2 mask : TEXCOORD1; };
struct P { float4 pos : POSITION0; float4 color : COLOR0; float2 uv : TEXCOORD0; float2 mask : TEXCOORD1; };
P vs(V v) { P p; p.pos=float4(v.pos,1); p.color=v.color; p.uv=v.uv; p.mask=v.mask; return p; }
float4 ps(P p) : COLOR0 { return p.color * step(p.mask.x,0.85) * step(p.uv.y,0.85); }
)";
    ID3DBlob *vsCode = nullptr, *psCode = nullptr, *errors = nullptr;
    Good(compile(shader, strlen(shader), "hudprobe", nullptr, nullptr, "vs", "vs_2_0", 0, 0, &vsCode, &errors));
    if (errors) { errors->Release(); errors = nullptr; }
    Good(compile(shader, strlen(shader), "hudprobe", nullptr, nullptr, "ps", "ps_2_0", 0, 0, &psCode, &errors));
    if (errors) errors->Release();
    IDirect3DVertexShader9* vertexShader = nullptr; IDirect3DPixelShader9* pixelShader = nullptr;
    Good(dev->CreateVertexShader((DWORD*)vsCode->GetBufferPointer(), &vertexShader));
    Good(dev->CreatePixelShader((DWORD*)psCode->GetBufferPointer(), &pixelShader));
    vsCode->Release(); psCode->Release();
    const HudProbeRect bounds = {432, 280, 640, 480};
    unsigned cases = 0;
    for (D3DMULTISAMPLE_TYPE ms : {D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES}) {
        DWORD levels = 0;
        if (ms && FAILED(api->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, ms, &levels))) continue;
        for (bool clip : {false, true}) for (bool programmable : {false, true}) {
            HudProbeAtlas a; Good(a.Begin(dev, ms, 0));
            Good(dev->ColorFill(original, nullptr, 0xff123456));
            Good(dev->BeginScene());
            D3DVIEWPORT9 vp = {12, 20, 640, 480, 0.1f, 0.9f}; Good(dev->SetViewport(&vp));
            RECT scissor = {12, 20, 12+550, 20+480}; Good(dev->SetScissorRect(&scissor));
            Good(dev->SetRenderState(D3DRS_SCISSORTESTENABLE, clip));
            Good(dev->SetRenderState(D3DRS_ZENABLE, TRUE));
            Good(dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
            Good(dev->SetRenderState(D3DRS_STENCILENABLE, TRUE));
            Good(dev->SetFVF(programmable ? 0x242 : 0x20142));
            Good(dev->SetVertexShader(programmable ? vertexShader : nullptr));
            Good(dev->SetPixelShader(programmable ? pixelShader : nullptr));
            HudProbeVertex q[4] = {};
            float coords[4][2] = {{464, 320}, {624, 320}, {624, 464}, {464, 464}};
            for (int i = 0; i < 4; i++) { q[i].x = coords[i][0]/320-1; q[i].y = 1-coords[i][1]/240;
                                          q[i].z = 0.5f; q[i].color = 0xff30d060;
                                          q[i].uv[0] = q[i].uv[2] = i == 1 || i == 2 ? 1.0f : 0.0f;
                                          q[i].uv[1] = q[i].uv[3] = i >= 2 ? 1.0f : 0.0f; }
            auto unchanged = q[0]; bool outside = true;
            Good(a.Duplicate(dev, q, 1.0f/320, 1.0f/240, bounds, &outside)); assert(!outside);
            assert(!memcmp(&unchanged, &q[0], sizeof(unchanged)));
            D3DVIEWPORT9 after; Good(dev->GetViewport(&after)); assert(!memcmp(&vp, &after, sizeof(vp)));
            RECT afterScissor; Good(dev->GetScissorRect(&afterScissor)); assert(EqualRect(&scissor, &afterScissor));
            IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
            Good(dev->GetRenderTarget(0, &rt)); Good(dev->GetDepthStencilSurface(&ds)); assert(rt==original && ds==depth);
            rt->Release(); ds->Release();
            DWORD value; Good(dev->GetRenderState(D3DRS_STENCILENABLE, &value)); assert(value);
            Good(dev->GetRenderState(D3DRS_ZENABLE, &value)); assert(value);
            Good(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &value)); assert(value==(DWORD)clip);
            IDirect3DVertexShader9* afterVS = nullptr; IDirect3DPixelShader9* afterPS = nullptr;
            Good(dev->GetVertexShader(&afterVS)); Good(dev->GetPixelShader(&afterPS));
            assert(afterVS == (programmable ? vertexShader : nullptr));
            assert(afterPS == (programmable ? pixelShader : nullptr));
            if (afterVS) afterVS->Release(); if (afterPS) afterPS->Release();
            Good(dev->EndScene()); Good(a.End(dev)); assert(a.Texture());
            auto untouched = Pixels(dev, original);
            for (DWORD p : untouched) assert(p == 0xff123456);
            IDirect3DSurface9* atlasSurface = nullptr; Good(a.Texture()->GetSurfaceLevel(0, &atlasSurface));
            auto result = Pixels(dev, atlasSurface);
            unsigned colored = 0; for (DWORD p : result) colored += (p >> 24) != 0;
            assert(colored > 10000 && colored < 200000);
            assert((result[256*512+256] & 0xffffff) == 0x30d060);
            if (!ms && !clip && programmable) Save("..\\dist\\validation\\hudprobe-gpu.bmp", result);
            Good(a.Begin(dev, ms, 0)); Good(a.End(dev));
            for (DWORD p : Pixels(dev, atlasSurface)) assert(p == 0);
            atlasSurface->Release(); a.Release();
            assert(!a.Texture()); cases++;
        }
    }
    printf("GPU: %u MSAA/scissor/fixed/shader cases passed; ordinary target unchanged; state restored; atlas clears\n", cases);
    // Exercise the production dispatch as well as the atlas helper. The installed shader path
    // reports TEX2 (0x242), unlike the older fixed-function declaration (0x20142).
    Good(dev->SetFVF(0x242)); Good(dev->SetVertexShader(vertexShader)); Good(dev->SetPixelShader(pixelShader));
    Good(dev->SetRenderState(D3DRS_ZENABLE, FALSE)); Good(dev->SetRenderState(D3DRS_STENCILENABLE, FALSE));
    Good(dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    Object obj = {}; obj.speedo = true; obj.type = 1;
    current = &obj; active = treeOk = atlasOk = true; thread = GetCurrentThreadId(); expectedHud = original;
    source = bounds; scaleX = 1.0f/320; scaleY = 1.0f/240; frame = {};
    Good(atlas.Begin(dev, D3DMULTISAMPLE_NONE, 0));
    HudProbeVertex liveQuad[4] = {};
    const float xy[4][2] = {{464, 320}, {624, 320}, {624, 464}, {464, 464}};
    for (int i = 0; i < 4; i++) { liveQuad[i].x = xy[i][0]/320-1; liveQuad[i].y = 1-xy[i][1]/240;
                                  liveQuad[i].z = 0.5f; liveQuad[i].color = 0xff30d060; }
    Good(dev->BeginScene()); HudProbe_Draw(dev, D3DPT_TRIANGLEFAN, 2, liveQuad, sizeof(liveQuad[0])); Good(dev->EndScene());
    assert(frame.recognized == 1 && frame.duplicates == 1 && frame.skipped == 0);
    HudProbe_End(dev); assert(HudProbe_Texture() == nullptr); // disabled setting never exposes the panel
    g_cfg.hudProbe = true; assert(HudProbe_Texture()); HudProbe_Release();
    puts("production dispatch: live TEX2 shader declaration duplicated; disabled setting hides the panel");
    // Six semantic cells share one frame decision. A failure after five good cells must
    // expose neither a partial dashboard nor a partially erased ordinary HUD.
    for (auto size : {std::array<UINT,2>{1024,768}, {2560,1440}, {1832,1920}}) {
        IDirect3DSurface9* full=nullptr;
        Good(dev->CreateRenderTarget(size[0],size[1],D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&full,nullptr));
        Good(dev->SetDepthStencilSurface(nullptr)); Good(dev->SetRenderTarget(0,full));
        D3DVIEWPORT9 viewport={0,0,size[0],size[1],0,1}; Good(dev->SetViewport(&viewport));
        Good(dev->ColorFill(full,nullptr,0xff123456));
        D3DSURFACE_DESC desc={}; Good(full->GetDesc(&desc)); assert(BeginRemaining(dev,desc));
        active=treeOk=directActive=directOk=true; atlasOk=false; expectedHud=full; thread=GetCurrentThreadId();
        scaleX=1.0f/320; scaleY=1.0f/240; frame={};
        memset(componentDraws,0,sizeof(componentDraws));
        for (int e=0;e<DASH_EL_COUNT;e++) {
            Good(components[e].Begin(dev,D3DMULTISAMPLE_NONE,0)); componentSource[e]=bounds;
        }
        Good(dev->BeginScene());
        for (int e=0;e<DASH_EL_COUNT;e++) {
            Object element={}; element.component=e; current=&element;
            HudProbe_Draw(dev,D3DPT_TRIANGLEFAN,2,liveQuad,sizeof(liveQuad[0]));
        }
        current=nullptr;
        HudProbe_Draw(dev,D3DPT_TRIANGLEFAN,2,liveQuad,sizeof(liveQuad[0]));
        Good(dev->EndScene()); HudProbe_End(dev);
        assert(HudComponents_Ready() && HudComponents_Remaining());
        for (int e=0;e<DASH_EL_COUNT;e++) {
            auto* texture=HudComponents_Texture(e); assert(texture && completedDraws[e]==1);
            IDirect3DSurface9* cell=nullptr; Good(texture->GetSurfaceLevel(0,&cell));
            assert((Pixels(dev,cell)[256*512+256]&0xffffff)==0x30d060); cell->Release();
        }
        for (DWORD pixel:Pixels(dev,full)) assert(pixel==0xff123456);
        bool remainder=false; for (DWORD pixel:Pixels(dev,HudComponents_Remaining())) remainder |= (pixel&0xffffff)==0x30d060;
        assert(remainder);
        active=directActive=directOk=true; expectedHud=full;
        HudProbe_OtherDraw(dev); assert(!directOk); HudProbe_End(dev);
        assert(!HudComponents_Ready() && !HudComponents_Remaining());
        for (int e=0;e<DASH_EL_COUNT;e++) assert(!HudComponents_Texture(e));
        Good(dev->SetRenderTarget(0,original)); full->Release();
    }
    HudProbe_Release();
    puts("components: six cells and remaining HUD passed at 4:3, 16:9 and square-eye sizes; whole-frame failure fallback passed");
    Good(dev->SetVertexShader(nullptr)); Good(dev->SetPixelShader(nullptr));
    vertexShader->Release(); pixelShader->Release(); FreeLibrary(compiler);
    Good(dev->SetDepthStencilSurface(nullptr)); depth->Release(); original->Release();
    dev->Release(); api->Release(); DestroyWindow(wnd);
}
