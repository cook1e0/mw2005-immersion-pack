#include "../src/spectator.cpp"
#include "../src/spectator_motion.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>

Config g_cfg;
void Log(const char* fmt, ...) {
    va_list args; va_start(args,fmt); vprintf(fmt,args); putchar('\n'); va_end(args);
}
static constexpr int W=640,H=360;
static unsigned draws;
static void Good(HRESULT hr) { if(FAILED(hr)) { printf("D3D failure 0x%08lx\n",hr); fflush(stdout); abort(); } }
static IDirect3DTexture9* Texture(IDirect3DDevice9* dev, bool hud=false, bool grid=false) {
    IDirect3DTexture9* tex=nullptr;
    Good(dev->CreateTexture(W,H,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&tex,nullptr));
    D3DLOCKED_RECT lock; Good(tex->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD));
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        DWORD c=grid ? D3DCOLOR_ARGB(255,48+x*128/W,64+y*128/H,96) : D3DCOLOR_ARGB(255,40,100,180);
        if(grid && (x%40<2 || y%40<2)) c=D3DCOLOR_ARGB(255,220,220,220);
        if(hud) c=(x>W/3 && x<2*W/3 && y>H/3 && y<2*H/3) ? D3DCOLOR_ARGB(255,240,30,40) : 0;
        ((DWORD*)((BYTE*)lock.pBits+y*lock.Pitch))[x]=c;
    }
    Good(tex->UnlockRect(0)); return tex;
}
static std::vector<DWORD> Read(IDirect3DDevice9* dev) {
    IDirect3DSurface9 *bb=nullptr,*sys=nullptr;
    Good(dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb));
    Good(dev->CreateOffscreenPlainSurface(W,H,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&sys,nullptr));
    Good(dev->GetRenderTargetData(bb,sys));
    D3DLOCKED_RECT lock; Good(sys->LockRect(&lock,nullptr,D3DLOCK_READONLY));
    std::vector<DWORD> pixels(W*H);
    for(int y=0;y<H;y++) memcpy(pixels.data()+y*W,(BYTE*)lock.pBits+y*lock.Pitch,W*4);
    Good(sys->UnlockRect()); bb->Release(); sys->Release(); return pixels;
}
static void Bmp(const char* path,const std::vector<DWORD>& pixels) {
    FILE* f=fopen(path,"wb"); assert(f);
    BITMAPFILEHEADER file={}; BITMAPINFOHEADER info={};
    file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(info); file.bfSize=file.bfOffBits+W*H*4;
    info.biSize=sizeof(info); info.biWidth=W; info.biHeight=-H; info.biPlanes=1; info.biBitCount=32;
    fwrite(&file,sizeof(file),1,f); fwrite(&info,sizeof(info),1,f); fwrite(pixels.data(),4,W*H,f); fclose(f);
}
static void Draw(IDirect3DDevice9* dev,SpectatorFrame& f) {
    LARGE_INTEGER now,freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    s_lastTime.QuadPart=now.QuadPart-freq.QuadPart/90;
    Good(dev->SetRenderState(D3DRS_FOGENABLE,TRUE));
    Good(dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));
    D3DVIEWPORT9 beforeViewport={12,14,128,90,0.2f,0.8f};
    Good(dev->SetViewport(&beforeViewport));
    IDirect3DSurface9* previous=nullptr; Good(dev->GetRenderTarget(0,&previous));
    Spectator_Draw(dev,f); draws++;
    DWORD fog=0,blend=0; Good(dev->GetRenderState(D3DRS_FOGENABLE,&fog));
    Good(dev->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend)); assert(fog && blend);
    IDirect3DSurface9* after=nullptr; Good(dev->GetRenderTarget(0,&after)); assert(after==previous);
    after->Release(); previous->Release();
    D3DVIEWPORT9 afterViewport={}; Good(dev->GetViewport(&afterViewport));
    if(memcmp(&beforeViewport,&afterViewport,sizeof(beforeViewport)))
        printf("viewport changed: before %lu,%lu %lux%lu after %lu,%lu %lux%lu\n",beforeViewport.X,beforeViewport.Y,
            beforeViewport.Width,beforeViewport.Height,afterViewport.X,afterViewport.Y,afterViewport.Width,afterViewport.Height);
    assert(!memcmp(&beforeViewport,&afterViewport,sizeof(beforeViewport)));
}
static void Nonblack(const std::vector<DWORD>& pixels) {
    unsigned black=0; for(DWORD c:pixels) black+=(c&0xffffff)==0;
    if(black) printf("Unexpected black pixels: %u at draw %u\n",black,draws);
    assert(!black);
}
static void Settings() {
    assert(Spectator_Edit("Reset",0));
    assert(g_cfg.spectatorPreset==0 && g_cfg.spectatorFov==0 && g_cfg.spectatorAspect==0);
    assert(Spectator_Edit("SpectatorPreset",1));
    assert(g_cfg.spectatorFov==0 && g_cfg.spectatorAspect==0);
    assert(fabsf(*Spectator_Setting("SpectatorResolvedFov")-80)<0.001f);
    assert(fabsf(*Spectator_Setting("SpectatorResolvedAspect")-16.0f/9)<0.001f);
    assert(Spectator_Edit("SpectatorSmoothing",0.25f));
    assert(g_cfg.spectatorPreset==2 && g_cfg.spectatorFov==80 && fabsf(g_cfg.spectatorAspect-16.0f/9)<0.001f);
    assert(!Spectator_Edit("SpectatorResolvedFov",100));
    assert(!Spectator_Edit("SpectatorFov",NAN));
    assert(Spectator_Edit("Reset",0));
    assert(Spectator_Edit("SpectatorPreset",2));
    assert(g_cfg.spectatorFov>0 && g_cfg.spectatorAspect>0);
    assert(Spectator_Edit("SpectatorFov",0)); assert(g_cfg.spectatorFov==80);
    assert(Spectator_Edit("SpectatorAspect",0)); assert(fabsf(g_cfg.spectatorAspect-16.0f/9)<0.001f);
    assert(Spectator_Edit("Reset",0));
    assert(Spectator_Edit("SpectatorPreset",1));
    puts("settings: legacy zeros, Broadcast auto, Custom materialization, invalid edits and reset passed");
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    Settings();
    g_cfg.forceWindowed=false; g_cfg.fakeHmd=true; assert(g_cfg.UsesWindowedRendering());
    g_cfg.fakeHmd=false; assert(!g_cfg.UsesWindowedRendering());
    g_cfg.forceWindowed=true; assert(g_cfg.UsesWindowedRendering());
    WNDCLASSA wc={}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandle(nullptr); wc.lpszClassName="SpectatorRenderTest";
    assert(RegisterClassA(&wc));
    HWND wnd=CreateWindowA(wc.lpszClassName,"Spectator GPU test",WS_OVERLAPPEDWINDOW,0,0,W,H,nullptr,nullptr,wc.hInstance,nullptr); assert(wnd);
    IDirect3D9Ex* api=nullptr; Good(Direct3DCreate9Ex(D3D_SDK_VERSION,&api));
    D3DADAPTER_IDENTIFIER9 adapter={}; Good(api->GetAdapterIdentifier(0,0,&adapter)); printf("GPU: %s\n",adapter.Description);
    D3DPRESENT_PARAMETERS pp={}; pp.BackBufferWidth=W; pp.BackBufferHeight=H; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=wnd; pp.Windowed=TRUE; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9Ex* dev=nullptr;
    Good(api->CreateDeviceEx(0,D3DDEVTYPE_HAL,wnd,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,nullptr,&dev));
    auto eye=Texture(dev),grid=Texture(dev,false,true),hud=Texture(dev,true);
    SpectatorFrame frame={}; frame.eye[0]=frame.eye[1]=eye; frame.eyeRot[0]=frame.eyeRot[1]={0,0,0,1};
    frame.eyeTanX=frame.eyeTanY=frame.hmdTanX=frame.hmdTanY=1;
    frame.bbWidth=W; frame.bbHeight=H; frame.stereo=true;
    for(int i=0;i<12*90;i++) {
        frame.eyeRot[0]=frame.eyeRot[1]=Spectator_TestPose(i/90.0);
        Draw(dev,frame); assert(s_reproject);
        Nonblack(Read(dev));
    }
    printf("GPU reprojection: %u scripted frames with zero black pixels; device state restored\n",draws);
    IDirect3DSurface9 *backBuffer=nullptr,*otherTarget=nullptr;
    Good(dev->GetRenderTarget(0,&backBuffer));
    Good(dev->CreateRenderTarget(320,180,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&otherTarget,nullptr));
    Good(dev->SetRenderTarget(0,otherTarget)); Draw(dev,frame); Nonblack(Read(dev));
    Good(dev->SetRenderTarget(0,backBuffer)); otherTarget->Release(); backBuffer->Release();
    g_cfg.spectatorFov=140;
    for(float aspect : {0.5f,1.0f,16.0f/9.0f,3.5f}) {
        g_cfg.spectatorAspect=aspect;
        frame.eyeTanX=0.6f; frame.eyeTanY=0.4f;
        for(int i=0;i<30;i++) {
            frame.eyeRot[0]=frame.eyeRot[1]=Spectator_TestPose(i*0.4);
            Draw(dev,frame); Nonblack(Read(dev));
        }
    }
    puts("GPU coverage: low rendered FOV, oversized request and four output aspects passed");
    assert(Spectator_Edit("Reset",0)); assert(Spectator_Edit("SpectatorPreset",1));
    frame.eyeTanX=frame.eyeTanY=1;
    frame.eyeRot[0]=frame.eyeRot[1]={0,0,0,1}; frame.poseEpoch++;
    frame.eye[0]=frame.eye[1]=grid; Draw(dev,frame);
    Bmp("..\\dist\\validation\\spectator-grid.bmp",Read(dev));
    frame.eye[0]=frame.eye[1]=eye; frame.hud=hud; g_cfg.spectatorHud=0;
    Draw(dev,frame); auto hidden=Read(dev);
    assert((hidden[(H/2)*W+W/2]&0xffffff)==0x2864b4);
    frame.menuOpen=true; Draw(dev,frame); auto menuOpen=Read(dev);
    assert((menuOpen[(H/2)*W+W/2]&0xffffff)==0xf01e28);
    Bmp("..\\dist\\validation\\spectator-hud.bmp",menuOpen);
    frame.menuOpen=false; g_cfg.spectatorHud=1; Draw(dev,frame);
    assert((Read(dev)[(H/2)*W+W/2]&0xffffff)==0xf01e28);
    assert((Read(dev)[(H/2)*W+230]&0xffffff)==0x2864b4);
    frame.stereo=false; frame.hud=nullptr; Draw(dev,frame); auto menu=Read(dev);
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        bool black=(menu[y*W+x]&0xffffff)==0;
        assert(black==(x<80 || x>=560));
    }
    Bmp("..\\dist\\validation\\spectator-menu.bmp",menu);
    puts("GPU composition: exact centred 4:3 pillarbox, HUD toggle and menu visibility passed");
    // Mod menus use a native pixel canvas rather than the game's anamorphic one.
    frame.panelAspect=16.0f/9.0f;
    Draw(dev,frame); Nonblack(Read(dev));
    frame.stereo=true; frame.hud=hud;
    Draw(dev,frame); auto wideHud=Read(dev);
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        bool red=(wideHud[y*W+x]&0xffffff)==0xf01e28;
        assert(red==(x>W/3 && x<2*W/3 && y>H/3 && y<2*H/3));
    }
    Bmp("..\\dist\\validation\\spectator-wide-hud.bmp",wideHud);
    frame.panelAspect=0; frame.hud=nullptr;
    frame.overlay=hud; Draw(dev,frame); auto overlay=Read(dev);
    assert(overlay==wideHud);
    // An eye-shaped mod layer would pillarbox F4 while input still addresses the full client.
    frame.overlayAspect=1832.0f/1920.0f; Draw(dev,frame);
    assert((Read(dev)[(H/2)*W+230]&0xffffff)==0x2864b4);
    frame.overlayAspect=16.0f/9.0f; Draw(dev,frame);
    assert(Read(dev)==wideHud); // window-shaped canvas and mouse now use identical normalised coordinates
    puts("GPU cursor composition: window-shaped F4 canvas maps edge controls without eye-aspect pillarboxing");
    assert((overlay[(H/2)*W+230]&0xffffff)==0xf01e28);
    g_cfg.spectatorHud=0; Draw(dev,frame);
    assert((Read(dev)[(H/2)*W+W/2]&0xffffff)==0x2864b4);
    g_cfg.spectatorHud=1; frame.overlay=nullptr;
    // Headset composition targets a separate non-MSAA surface without touching the desktop.
    IDirect3DTexture9* submitted=nullptr; IDirect3DSurface9* submittedSurface=nullptr;
    Good(dev->CreateTexture(W,H,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&submitted,nullptr));
    Good(submitted->GetSurfaceLevel(0,&submittedSurface));
    Good(dev->ColorFill(submittedSurface,nullptr,0));
    auto beforeSubmit=Read(dev);
    Spectator_BlendFullscreen(dev,hud,W,H,submittedSurface);
    assert(Read(dev)==beforeSubmit);
    frame.overlay=submitted; Draw(dev,frame);
    assert(Read(dev)==wideHud);
    Good(dev->ColorFill(submittedSurface,nullptr,0));
    Spectator_BlendFullscreen(dev,hud,W,H,submittedSurface,false,16.0f/9.0f,4.0f/3.0f);
    frame.overlay=submitted; Draw(dev,frame);
    auto headsetFit=Read(dev);
    assert((headsetFit[(H/2)*W+W/2]&0xffffff)==0xf01e28);
    assert((headsetFit[(H/3+5)*W+W/2]&0xffffff)==0x2864b4);
    puts("GPU headset overlay: desktop canvas fits the HUD layer without stretching");
    D3DLOCKED_RECT overlayLock; Good(hud->LockRect(0,&overlayLock,nullptr,D3DLOCK_DISCARD));
    for(int y=0;y<H;y++) for(int x=0;x<W;x++)
        ((DWORD*)((BYTE*)overlayLock.pBits+y*overlayLock.Pitch))[x]=
            x>W/3 && x<2*W/3 && y>H/3 && y<2*H/3 ? D3DCOLOR_ARGB(128,128,0,0) : 0;
    Good(hud->UnlockRect(0));
    frame.overlay=hud; Draw(dev,frame); auto translucent=Read(dev);
    DWORD center=translucent[(H/2)*W+W/2];
    assert(abs((int)((center>>16)&255)-148)<=1 && abs((int)((center>>8)&255)-50)<=1 && abs((int)(center&255)-90)<=1);
    Good(dev->ColorFill(submittedSurface,nullptr,0));
    Spectator_BlendFullscreen(dev,hud,W,H,submittedSurface,true);
    frame.overlay=submitted; Draw(dev,frame);
    assert(Read(dev)==translucent);
    frame.overlay=nullptr; submittedSurface->Release(); submitted->Release();
    puts("GPU composition: native pixel overlays stay separate from the 4:3 HUD and reach the headset texture");
    // SBS remains two separate eye images, rather than the Broadcast framing.
    frame.stereo=true; frame.eye[0]=eye; frame.eye[1]=grid; frame.poseEpoch++;
    Spectator_CycleMode(); assert(Spectator_Mode()==SpectatorMode::SideBySide);
    Draw(dev,frame); auto sbs=Read(dev);
    assert((sbs[(H/2)*W+W/4]&0xffffff)==0x2864b4);
    assert((sbs[(H/2)*W+3*W/4]&0xffffff)!=0x2864b4);
    Spectator_CycleMode(); assert(Spectator_Mode()==SpectatorMode::Eye);
    Spectator_CycleMode(); assert(Spectator_Mode()==SpectatorMode::Smooth);
    puts("modes: SBS keeps both eyes and F6 cycle order is unchanged");
    eye->Release(); grid->Release(); hud->Release();
    if(s_state) { s_state->Release(); s_state=nullptr; }
    if(s_reproject) { s_reproject->Release(); s_reproject=nullptr; }
    dev->Release(); api->Release(); DestroyWindow(wnd);
    puts("spectator GPU tests passed");
}
