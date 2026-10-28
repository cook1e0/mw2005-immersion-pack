#pragma once
#include <d3d9.h>

// Native UI component duplication. The ordinary HUD remains complete for frame-wide fallback.
struct HudProbeRect { float left, top, right, bottom; };
struct HudProbeVertex { float x, y, z; DWORD color; float uv[4]; };

class HudProbeAtlas {
public:
    HudProbeAtlas() = default;
    ~HudProbeAtlas() { Release(); }
    HRESULT Begin(IDirect3DDevice9* dev, D3DMULTISAMPLE_TYPE samples, DWORD quality);
    // clip (layout units, optional): only what falls inside it is copied. shiftX: layout units taken
    // off x first (the widescreen fix's FixHUD moves the HUD's parts out toward the screen's edges).
    HRESULT Duplicate(IDirect3DDevice9* dev, const HudProbeVertex* quad, float scaleX, float scaleY,
                      const HudProbeRect& source, bool* outside, const HudProbeRect* clip = nullptr,
                      float shiftX = 0);
    HRESULT End(IDirect3DDevice9* dev);
    void Release();
    IDirect3DTexture9* Texture() const { return valid_ ? texture_ : nullptr; }
    static constexpr UINT Size = 512;
private:
    HudProbeAtlas(const HudProbeAtlas&) = delete;
    HudProbeAtlas& operator=(const HudProbeAtlas&) = delete;
    IDirect3DTexture9* texture_ = nullptr;
    IDirect3DSurface9 *surface_ = nullptr, *target_ = nullptr;
    IDirect3DStateBlock9* state_ = nullptr;
    D3DMULTISAMPLE_TYPE samples_ = D3DMULTISAMPLE_NONE;
    DWORD quality_ = 0;
    bool valid_ = false, failed_ = false;
};

void HudProbe_Install();
void HudProbe_Begin(IDirect3DDevice9* dev, IDirect3DSurface9* hud, int width, int height, bool direct = true);
void HudProbe_Draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride);
void HudProbe_OtherDraw(IDirect3DDevice9* dev);
void HudProbe_End(IDirect3DDevice9* dev);
void HudProbe_RequestCapture();
void HudProbe_Release();
IDirect3DTexture9* HudProbe_Texture();
// All six cells are from the same completed HUD frame, or none are exposed.
bool HudComponents_Ready();
IDirect3DTexture9* HudComponents_Texture(int element);
IDirect3DSurface9* HudComponents_Remaining();
// The part of the HUD picture (left, top, right, bottom: shares of its width and height) that
// element's cell shows, so it can be placed exactly as a crop of that picture would be.
bool HudComponents_Coverage(int element, float out[4]);
// How many layout units wider than 640 the HUD's canvas is (the widescreen fix's FixHUD; 0 without),
// and how far right of the 640 layout that element's part is drawn on it.
float HudCanvasExtra();
float HudCanvasShift(int element);
