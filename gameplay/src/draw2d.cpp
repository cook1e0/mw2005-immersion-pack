// 2D overlay drawing (draw2d.h), shared by the menu, the heat gauge and the telemetry panel.
#include "common.h"
#include "draw2d.h"
#include <math.h>

namespace draw2d {

namespace {
const Font* s_cur;
struct Vtx {
    float x, y, z, rhw;
    D3DCOLOR c;
};
std::vector<Vtx> s_v;
}  // namespace

void UseFont(const Font& f) { s_cur = &f; }

void BuildFont(Font& f, int px) {
    f.px = px;
    HDC dc = CreateCompatibleDC(nullptr);
    int W = px * 3, H = px * 2;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    HFONT font = CreateFontA(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
    if (!dc || !bmp || !font) {
        Log("ui: font creation failed");
        f.height = 0;
    } else {
        HGDIOBJ oldBmp = SelectObject(dc, bmp), oldFont = SelectObject(dc, font);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, OPAQUE);
        TEXTMETRICA tm;
        GetTextMetricsA(dc, &tm);
        f.height = std::min((int)tm.tmHeight, H);
        RECT all = {0, 0, W, H};
        for (int c = 32; c < 127; c++) {
            Glyph& g = f.glyph[c - 32];
            g.runs.clear();
            char ch = (char)c;
            FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
            TextOutA(dc, 0, 0, &ch, 1);
            SIZE sz;
            GetTextExtentPoint32A(dc, &ch, 1, &sz);
            g.advance = sz.cx;
            GdiFlush();
            for (int y = 0; y < f.height; y++) {
                for (int x = 0; x < W;) {
                    if (!(bits[y * W + x] & 0xFFFFFF)) {
                        x++;
                        continue;
                    }
                    int x0 = x;
                    while (x < W && (bits[y * W + x] & 0xFFFFFF)) x++;
                    g.runs.push_back({(short)y, (short)x0, (short)x});
                }
            }
        }
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
    }
    if (font) DeleteObject(font);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
}

// --- drawing -------------------------------------------------------------------------------

void Quad(float x0, float y0, float x1, float y1, D3DCOLOR c) {
    x0 -= 0.5f, y0 -= 0.5f, x1 -= 0.5f, y1 -= 0.5f;   // pixel centres
    Vtx a = {x0, y0, 0, 1, c}, b = {x1, y0, 0, 1, c}, d = {x0, y1, 0, 1, c}, e = {x1, y1, 0, 1, c};
    s_v.insert(s_v.end(), {a, b, d, b, e, d});
}

void Quad4(const float* xy, D3DCOLOR c) {
    Vtx v[4];
    for (int i = 0; i < 4; i++) v[i] = {xy[i * 2] - 0.5f, xy[i * 2 + 1] - 0.5f, 0, 1, c};
    s_v.insert(s_v.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

void Line(float x0, float y0, float x1, float y1, float thick, D3DCOLOR c) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), h = thick * 0.5f;
    if (l < 1e-4f) {
        Quad(x0 - h, y0 - h, x0 + h, y0 + h, c);
        return;
    }
    float nx = -dy / l * h, ny = dx / l * h;
    float xy[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
    Quad4(xy, c);
}

float TextWidth(const char* t) {
    if (!s_cur) return 0;
    float w = 0;
    for (; *t; t++)
        if (*t >= 32 && *t < 127) w += s_cur->glyph[*t - 32].advance;
    return w;
}

void Text(float x, float y, const char* t, D3DCOLOR c) {
    if (!s_cur) return;
    x = floorf(x), y = floorf(y);
    for (; *t; t++) {
        if (*t < 32 || *t >= 127) continue;
        const Glyph& g = s_cur->glyph[*t - 32];
        for (const Run& r : g.runs) Quad(x + r.x0, y + r.y, x + r.x1, y + r.y + 1, c);
        x += g.advance;
    }
}

void ShadowText(float x, float y, const char* t, D3DCOLOR c, float k) {
    float o = std::max(1.0f, floorf(k));
    Text(x + o, y + o, t, D3DCOLOR_ARGB(200, 0, 0, 0));
    Text(x, y, t, c);
}

void Flush(IDirect3DDevice9* dev, int w, int h) {
    if (s_v.empty()) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) {
        s_v.clear();
        return;
    }
    D3DVIEWPORT9 vp = {0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // Keep the HUD layer's alpha meaningful for the VR panel: alpha = src + dst * (1 - src).
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    const size_t chunk = 30000;   // vertices per call, a multiple of 3
    for (size_t i = 0; i < s_v.size(); i += chunk) {
        size_t n = std::min(chunk, s_v.size() - i);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(n / 3), &s_v[i], sizeof(Vtx));
    }
    sb->Apply();
    sb->Release();
    s_v.clear();
}

}  // namespace draw2d
