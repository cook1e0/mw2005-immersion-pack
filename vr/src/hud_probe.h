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
    HRESULT Duplicate(IDirect3DDevice9* dev, const HudProbeVertex* quad, float scaleX, float scaleY,
                      const HudProbeRect& source, bool* outside);
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
