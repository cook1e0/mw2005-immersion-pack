// Heat gauge and tuning menu.
//
// Drawn with the game's own device (__g_IDirect3DDevice9, the VR mod's proxy when it is
// installed) right after the HUD, as untextured quads: text comes from a font rasterised once
// with GDI and stored as horizontal pixel runs. No textures means nothing to lose on a device
// reset and nothing that D3D9Ex (which the VR mod switches to) can't create.
//
// The menu is driven by bindings ([UI] Menu / MenuUp / ...), so it works with a keyboard, a
// wheel's d-pad or a button box, in VR as well as on a flat screen. While it is open the car's
// controls are blocked; settings are written to the ini when it closes.
#include "ui.h"
#include "common.h"
#include "game.h"
#include "input.h"
#include <d3d9.h>
#include <math.h>
#include <stdio.h>

namespace {

constexpr uintptr_t kDevice = 0x982BDC;     // IDirect3DDevice9* __g_IDirect3DDevice9

// --- font ---------------------------------------------------------------------------------
struct Run { short y, x0, x1; };
struct Glyph { int advance = 0; std::vector<Run> runs; };
struct Font {
    int px = 0;         // requested size
    int height = 0;     // line height in pixels
    Glyph glyph[96];    // ' ' .. '~'
} s_font;

void BuildFont(int px) {
    s_font.px = px;
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
        s_font.height = 0;
    } else {
        HGDIOBJ oldBmp = SelectObject(dc, bmp), oldFont = SelectObject(dc, font);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, OPAQUE);
        TEXTMETRICA tm;
        GetTextMetricsA(dc, &tm);
        s_font.height = std::min((int)tm.tmHeight, H);
        RECT all = {0, 0, W, H};
        for (int c = 32; c < 127; c++) {
            Glyph& g = s_font.glyph[c - 32];
            g.runs.clear();
            char ch = (char)c;
            FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
            TextOutA(dc, 0, 0, &ch, 1);
            SIZE sz;
            GetTextExtentPoint32A(dc, &ch, 1, &sz);
            g.advance = sz.cx;
            GdiFlush();
            for (int y = 0; y < s_font.height; y++) {
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
struct Vtx {
    float x, y, z, rhw;
    D3DCOLOR c;
};
std::vector<Vtx> s_v;

void Quad(float x0, float y0, float x1, float y1, D3DCOLOR c) {
    x0 -= 0.5f, y0 -= 0.5f, x1 -= 0.5f, y1 -= 0.5f;   // pixel centres
    Vtx a = {x0, y0, 0, 1, c}, b = {x1, y0, 0, 1, c}, d = {x0, y1, 0, 1, c}, e = {x1, y1, 0, 1, c};
    s_v.insert(s_v.end(), {a, b, d, b, e, d});
}

float TextWidth(const char* t) {
    float w = 0;
    for (; *t; t++)
        if (*t >= 32 && *t < 127) w += s_font.glyph[*t - 32].advance;
    return w;
}

void Text(float x, float y, const char* t, D3DCOLOR c) {
    x = floorf(x), y = floorf(y);
    for (; *t; t++) {
        if (*t < 32 || *t >= 127) continue;
        const Glyph& g = s_font.glyph[*t - 32];
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

bool Blink(int ms) { return (GetTickCount() / ms) % 2 == 0; }

// --- heat gauge ----------------------------------------------------------------------------
void DrawGauge(const EngineStatus& st, int W, int H, float k) {
    if (!g_cfg.gauge || !(st.wear || st.blown)) return;
    float fh = (float)s_font.height;
    float cx = g_cfg.gaugeX * W, cy = g_cfg.gaugeY * H;
    float pad = 4 * k;
    if (st.blown) {
        const char* t = "ENGINE BLOWN";
        float w = TextWidth(t);
        Quad(cx - w / 2 - pad, cy - fh / 2 - pad, cx + w / 2 + pad, cy + fh / 2 + pad, D3DCOLOR_ARGB(150, 0, 0, 0));
        ShadowText(cx - w / 2, cy - fh / 2, t, Blink(400) ? D3DCOLOR_ARGB(255, 255, 60, 40) : D3DCOLOR_ARGB(255, 150, 30, 20), k);
        return;
    }
    const char* label = "ENGINE";
    float lw = TextWidth(label), gap = 6 * k, barW = 110 * k, barH = std::max(4.0f, roundf(8 * k));
    float x0 = roundf(cx - (lw + gap + barW) / 2), x1 = x0 + lw + gap + barW;
    Quad(x0 - pad, cy - fh / 2 - pad, x1 + pad, cy + fh / 2 + pad, D3DCOLOR_ARGB(110, 0, 0, 0));
    float heat = Clamp(st.heat, 0.0f, 1.0f);
    bool losing = heat > g_cfg.powerLossStart, critical = heat > 0.85f;
    D3DCOLOR fill = critical ? D3DCOLOR_ARGB(255, 235, 45, 30)
                  : losing   ? D3DCOLOR_ARGB(255, 255, 165, 0)
                             : D3DCOLOR_ARGB(255, 90, 200, 90);
    if (critical && !Blink(150)) fill = D3DCOLOR_ARGB(255, 120, 20, 15);
    ShadowText(x0, cy - fh / 2, label, losing ? fill : D3DCOLOR_ARGB(255, 230, 230, 230), k);
    float bx = x0 + lw + gap, by = roundf(cy - barH / 2);
    Quad(bx, by, bx + barW, by + barH, D3DCOLOR_ARGB(170, 45, 45, 45));
    if (heat > 0.0f) Quad(bx, by, bx + roundf(barW * heat), by + barH, fill);
    // Where the engine starts losing power.
    float tx = roundf(bx + barW * g_cfg.powerLossStart);
    Quad(tx, by - 2 * k, tx + std::max(1.0f, floorf(k)), by + barH + 2 * k, D3DCOLOR_ARGB(200, 255, 255, 255));
}

// --- tuning menu -------------------------------------------------------------------------
enum Kind { HEADER, TOGGLE, NUMBER };
struct Item {
    Kind kind;
    const char* label;
    const char* section;
    const char* key;
    void* value;            // bool* or float*
    float lo, hi, step;
    float show;             // display multiplier
    const char* fmt;
};

const Item kItems[] = {
    {HEADER, "ENGINE", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr},
    {TOGGLE, "Engine wear", "Engine", "EngineWear", &g_cfg.engineWear, 0, 0, 0, 0, nullptr},
    {TOGGLE, "Rev limiter", "Engine", "RevLimiter", &g_cfg.revLimiter, 0, 0, 0, 0, nullptr},
    {NUMBER, "Heating at redline", "Engine", "HeatUp", &g_cfg.heatUp, 0.05f, 2.0f, 0.05f, 1, "%.2f /s"},
    {NUMBER, "Heating at max rpm", "Engine", "HeatUpMaxRPM", &g_cfg.heatUpMax, 0.05f, 5.0f, 0.05f, 1, "%.2f /s"},
    {NUMBER, "Cooling", "Engine", "CoolDown", &g_cfg.coolDown, 0.01f, 1.0f, 0.01f, 1, "%.2f /s"},
    {NUMBER, "Power loss from", "Engine", "PowerLossStart", &g_cfg.powerLossStart, 0.0f, 0.95f, 0.05f, 100, "%.0f%% heat"},
    {NUMBER, "Power lost at full heat", "Engine", "PowerLoss", &g_cfg.powerLoss, 0.0f, 1.0f, 0.05f, 100, "%.0f%%"},
    {TOGGLE, "Heat gauge", "UI", "Gauge", &g_cfg.gauge, 0, 0, 0, 0, nullptr},
    {HEADER, "SHIFTER AND CLUTCH", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr},
    {TOGGLE, "Clutch needed to shift", "Shifter", "RequireClutch", &g_cfg.requireClutch, 0, 0, 0, 0, nullptr},
    {NUMBER, "Clutch travel to shift", "Shifter", "ClutchToShift", &g_cfg.shiftPedal, 0.05f, 1.0f, 0.05f, 100, "%.0f%%"},
    {NUMBER, "Clutchless rev-match", "Shifter", "RevMatchRPM", &g_cfg.revMatchRpm, 0.0f, 3000.0f, 50.0f, 1, "%.0f rpm"},
    {NUMBER, "Clutch strength", "Clutch", "Strength", &g_cfg.strength, 0.1f, 2.0f, 0.1f, 1, "%.1f"},
    {NUMBER, "Bite point start", "Clutch", "BiteStart", &g_cfg.biteStart, 0.05f, 1.0f, 0.05f, 100, "%.0f%%"},
    {NUMBER, "Bite point end", "Clutch", "BiteEnd", &g_cfg.biteEnd, 0.0f, 0.95f, 0.05f, 100, "%.0f%%"},
};
constexpr int kNumItems = sizeof(kItems) / sizeof(kItems[0]);

struct {
    bool open = false;
    bool dirty = false;
    int sel = 1;
    bool synced = false;
    uint32_t menu = 0, nav[4] = {};
} s_menu;

std::string ValueText(const Item& it) {
    char buf[32];
    if (it.kind == TOGGLE) return *(bool*)it.value ? "On" : "Off";
    float v = *(float*)it.value;
    if (it.value == &g_cfg.revMatchRpm && v <= 0.0f) return "Off";
    snprintf(buf, sizeof(buf), it.fmt, v * it.show);
    return buf;
}

void Change(const Item& it, int dir) {
    if (it.kind == TOGGLE) {
        bool& b = *(bool*)it.value;
        b = !b;
    } else if (it.kind == NUMBER) {
        float& v = *(float*)it.value;
        v = Clamp(roundf((v + dir * it.step) / it.step) * it.step, it.lo, it.hi);
        // Keep the bite point ordered: start (released side) above end.
        if (g_cfg.biteStart < g_cfg.biteEnd + 0.05f) {
            if (it.value == &g_cfg.biteStart) g_cfg.biteEnd = std::max(0.0f, g_cfg.biteStart - 0.05f);
            else if (it.value == &g_cfg.biteEnd) g_cfg.biteStart = std::min(1.0f, g_cfg.biteEnd + 0.05f);
        }
    } else {
        return;
    }
    s_menu.dirty = true;
}

void Save() {
    for (const Item& it : kItems) {
        if (it.kind == HEADER) continue;
        char buf[32];
        if (it.kind == TOGGLE) snprintf(buf, sizeof(buf), "%d", *(bool*)it.value ? 1 : 0);
        else snprintf(buf, sizeof(buf), "%g", *(float*)it.value);
        WritePrivateProfileStringA(it.section, it.key, buf, IniPath());
    }
    Log("ui: settings saved to %s", IniPath());
}

void SetOpen(bool open) {
    if (open == s_menu.open) return;
    s_menu.open = open;
    Log("ui: menu %s", open ? "opened" : "closed");
    if (!open && s_menu.dirty) {
        Save();
        s_menu.dirty = false;
    }
    Game_SetMenuOpen(open);
}

void Step(int dir) {
    int i = s_menu.sel;
    do i = (i + dir + kNumItems) % kNumItems;
    while (kItems[i].kind == HEADER);
    s_menu.sel = i;
}

// Menu key presses only count while driving, so the pause menu and frontend keep their keys.
void UpdateMenu(const InputSnapshot& in, bool driving) {
    if (!s_menu.synced) {
        s_menu.synced = true;
        s_menu.menu = in.menu;
        memcpy(s_menu.nav, in.menuNav, sizeof(s_menu.nav));
    }
    int toggles = (int)(in.menu - s_menu.menu);
    int nav[4];
    for (int i = 0; i < 4; i++) nav[i] = (int)(in.menuNav[i] - s_menu.nav[i]);
    s_menu.menu = in.menu;
    memcpy(s_menu.nav, in.menuNav, sizeof(s_menu.nav));
    if (!driving) return;
    if (toggles & 1) SetOpen(!s_menu.open);
    if (!s_menu.open) return;
    for (int n = 0; n < nav[0]; n++) Step(-1);
    for (int n = 0; n < nav[1]; n++) Step(+1);
    for (int n = 0; n < nav[2]; n++) Change(kItems[s_menu.sel], -1);
    for (int n = 0; n < nav[3]; n++) Change(kItems[s_menu.sel], +1);
}

std::string MenuKeyName() {
    std::string m = g_cfg.menu;
    size_t comma = m.find(',');
    if (comma != std::string::npos) m.resize(comma);
    if (m.compare(0, 4, "key:") == 0) m = m.substr(4);
    return m.empty() ? "the menu key" : m;
}

void DrawMenu(int W, int H, float k) {
    float fh = (float)s_font.height, line = roundf(fh * 1.45f), pad = roundf(12 * k), gap = roundf(24 * k);
    float labelW = 0, valueW = TextWidth("< 100% heat >");
    for (const Item& it : kItems) labelW = std::max(labelW, TextWidth(it.label));
    std::string hint = "Up/Down: select    Left/Right: change    " + MenuKeyName() + ": close";
    const char* title = "DRIVETRAIN TUNING";
    float w = std::max(labelW + gap + valueW, std::max(TextWidth(hint.c_str()), TextWidth(title))) + 2 * pad;
    float h = line * (kNumItems + 3) + 2 * pad;
    float x0 = roundf((W - w) / 2), y0 = roundf((H - h) / 2);
    Quad(x0, y0, x0 + w, y0 + h, D3DCOLOR_ARGB(215, 12, 12, 14));
    Quad(x0, y0, x0 + w, y0 + roundf(2 * k), D3DCOLOR_ARGB(255, 255, 150, 0));
    float y = y0 + pad;
    Text(x0 + pad, y, title, D3DCOLOR_ARGB(255, 255, 150, 0));
    y += line * 1.5f;
    for (int i = 0; i < kNumItems; i++, y += line) {
        const Item& it = kItems[i];
        float ty = y + roundf((line - fh) / 2);
        if (it.kind == HEADER) {
            Text(x0 + pad, ty, it.label, D3DCOLOR_ARGB(255, 140, 140, 150));
            continue;
        }
        bool sel = i == s_menu.sel;
        if (sel) Quad(x0 + pad / 2, y, x0 + w - pad / 2, y + line, D3DCOLOR_ARGB(200, 190, 105, 0));
        D3DCOLOR c = sel ? D3DCOLOR_ARGB(255, 255, 255, 255) : D3DCOLOR_ARGB(255, 210, 210, 210);
        Text(x0 + pad + roundf(10 * k), ty, it.label, c);
        std::string v = ValueText(it);
        if (sel) v = "< " + v + " >";
        Text(x0 + w - pad - TextWidth(v.c_str()), ty, v.c_str(), c);
    }
    y += line * 0.5f;
    Text(x0 + pad, y, hint.c_str(), D3DCOLOR_ARGB(255, 150, 150, 160));
}

}  // namespace

void Ui_Frame() {
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)kDevice;
    if (!dev) return;
    EngineStatus st = Game_Engine();
    UpdateMenu(Input_Get(), st.driving);
    if (!st.driving) return;
    if (!(s_menu.open || (g_cfg.gauge && (st.wear || st.blown)))) return;

    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC desc;
    HRESULT hr = rt->GetDesc(&desc);
    rt->Release();
    if (FAILED(hr) || desc.Height < 100) return;

    float k = desc.Height / 768.0f * g_cfg.uiScale;
    int px = std::max(8, (int)roundf(16 * k));
    if (px != s_font.px) {
        BuildFont(px);
        Log("ui: font %d px for %ux%u", px, desc.Width, desc.Height);
    }
    if (!s_font.height) return;

    if (s_menu.open) DrawMenu(desc.Width, desc.Height, k);
    else DrawGauge(st, desc.Width, desc.Height, k);
    Flush(dev, desc.Width, desc.Height);
}
