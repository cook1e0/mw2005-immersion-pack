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
#include "physics.h"
#include <d3d9.h>
#include <psapi.h>
#include <float.h>
#include "imgui.h"
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
// BIND items hold a binding list (std::string in g_cfg). Right adds a binding: the next key,
// button, hat direction or pedal pressed is appended. Left unbinds the action (the menu's own
// controls go back to their defaults, so they can't be lost).
// Pages: the main page lists the categories (LINK items); each PAGE item starts a page and holds
// the items after it, up to the next PAGE. A LINK opens the page whose title is in its `section`.
// RESET puts the page's settings (not its bindings) back to their defaults, after a second press:
// the mod's defaults (Config), or with BF_GAME what the unmodded game does (kGameDefaults).
// CAR switches the current car between its own physics values and the [Physics] ones; the
// per-car sliders (Physics_IsPerCar) are saved to the car's section while it has its own.
enum Kind { HEADER, TOGGLE, NUMBER, BIND, LINK, CAR, ACTION, SOUND, PAGE, RESET };
enum { BF_REQUIRED = 1, BF_CLUTCH = 2, BF_STEER = 8, BF_PEDAL = 16, BF_GAME = 32 };

// Settings of the other mods, changed live through their exports and saved to their .ini: the
// FFB mod's [FFB] values (NfsmwFfb_Setting; older FFB builds only export NfsmwFfb_Gain) and the
// VR mod's [VR] HUD panel (NfsmwVr_Setting).
enum { MOD_FFB, MOD_VR };
const struct {
    const char* exportName;     // float* __cdecl (const char* key), null for an unknown key
    const char* ini;
    const char* missing;        // shown in place of the value without the mod
} kExtMods[] = {
    {"NfsmwFfb_Setting", "nfsmw_ffb.ini", "No FFB mod"},
    {"NfsmwVr_Setting", "nfsmw_vr.ini", "No VR mod"},
};
constexpr struct {
    int mod;
    const char* key;
    float def;                  // the mod's default
} kExt[] = {
    {MOD_FFB, "Gain", 1.0f}, {MOD_FFB, "MaxForce", 0.8f}, {MOD_FFB, "Aligning", 0.8f},
    {MOD_FFB, "Centering", 0.3f}, {MOD_FFB, "Damping", 0.3f}, {MOD_FFB, "Friction", 0.05f},
    {MOD_FFB, "SoftLock", 0.8f}, {MOD_FFB, "SlewRate", 20.0f}, {MOD_FFB, "SmoothingHz", 25.0f},
    {MOD_FFB, "Reverse", 0.0f}, {MOD_FFB, "Device", 0.0f}, {MOD_FFB, "SoftLockAt", 1.0f},
    {MOD_FFB, "GameController", 1.0f}, {MOD_FFB, "MenuController", 1.0f}, {MOD_FFB, "MenuAxes", 0.0f},
    {MOD_VR, "HudWidth", 1.4f}, {MOD_VR, "HudDistance", 1.2f}, {MOD_VR, "HudHeight", -0.05f},
    {MOD_VR, "NearZ", 0.05f},
    {MOD_VR, "SteeringWheelAngle", 0.0f},   // live, not a setting: the wheel's angle for the cockpit
    {MOD_VR, "SeatRight", 0.0f}, {MOD_VR, "SeatUp", 0.0f}, {MOD_VR, "SeatForward", 0.0f},
    {MOD_VR, "SeatReset", 0.0f},            // live: the seat bindings held now (-1, 0, 1)
    {MOD_VR, "HideArms", 1.0f},
    {MOD_FFB, "CentreWeight", 0.3f}, {MOD_FFB, "EngineRumble", 0.05f}, {MOD_FFB, "Kerbs", 0.5f},
    {MOD_FFB, "SurfaceRumble", 0.3f}, {MOD_FFB, "Crashes", 0.6f}, {MOD_FFB, "Scrapes", 0.4f},
    {MOD_FFB, "Breakables", 0.6f},
    {MOD_VR, "MirrorBrightness", 1.4f}, {MOD_VR, "MirrorContrast", 1.25f},
    {MOD_VR, "WheelOverHud", 0.0f},
    {MOD_VR, "MenuOpen", 0.0f},             // live, not a setting: this menu is open
    {MOD_FFB, "OffroadVibration", 0.4f},
    {MOD_VR, "HudPerCar", 0.0f},
    {MOD_VR, "HudRight", 0.0f},
    {MOD_FFB, "NosLighter", 0.3f},
    {MOD_FFB, "Landing", 0.8f},
    {MOD_FFB, "CarHits", 0.8f},
    {MOD_VR, "Dash", 1.0f}, {MOD_VR, "DashRight", 0.0f}, {MOD_VR, "DashUp", 0.0f}, {MOD_VR, "DashZoom", 1.0f},
    {MOD_VR, "DashCover", 1.3f},
    {MOD_VR, "DashMapX", 0.0f}, {MOD_VR, "DashMapY", 0.0f}, {MOD_VR, "DashMapSize", 1.0f},
    {MOD_VR, "DashRadarX", 0.0f}, {MOD_VR, "DashRadarY", 0.0f}, {MOD_VR, "DashRadarSize", 1.0f},
    {MOD_VR, "DashSpeedoX", 0.0f}, {MOD_VR, "DashSpeedoY", 0.0f}, {MOD_VR, "DashSpeedoSize", 1.0f},
    {MOD_VR, "DashPursuitX", 0.0f}, {MOD_VR, "DashPursuitY", 0.0f}, {MOD_VR, "DashPursuitSize", 1.0f},
    {MOD_VR, "DashBoardX", 0.0f}, {MOD_VR, "DashBoardY", 0.0f}, {MOD_VR, "DashBoardSize", 1.0f},
    {MOD_VR, "DashAdjust", 0.0f}, {MOD_VR, "DashHighlight", -1.0f},   // live: the Adjust dashboard page is open
    {MOD_VR, "EditorOn", 0.0f}, {MOD_VR, "EditCommand", 0.0f},        // live: the Triangle editor page, its command
    {MOD_VR, "EditBrush", 0.0f},
    {MOD_VR, "EditPicking", 0.0f}, {MOD_VR, "EditLayer", 0.0f},   // live: picking with the menu hidden
    {MOD_VR, "EditMode", 0.0f}, {MOD_VR, "EditPicked", 0.0f},
    {MOD_VR, "DashDepth", 0.0f},
    {MOD_VR, "DashMsgX", 0.0f}, {MOD_VR, "DashMsgY", 0.12f}, {MOD_VR, "DashMsgSize", 1.0f},
    {MOD_VR, "DashMapL", 0.165f}, {MOD_VR, "DashMapT", 0.600f}, {MOD_VR, "DashMapR", 0.415f}, {MOD_VR, "DashMapB", 0.990f},
    {MOD_VR, "DashRadarL", 0.355f}, {MOD_VR, "DashRadarT", 0.000f}, {MOD_VR, "DashRadarR", 0.645f}, {MOD_VR, "DashRadarB", 0.110f},
    {MOD_VR, "DashSpeedoL", 0.585f}, {MOD_VR, "DashSpeedoT", 0.620f}, {MOD_VR, "DashSpeedoR", 0.835f}, {MOD_VR, "DashSpeedoB", 0.990f},
    {MOD_VR, "DashPursuitL", 0.400f}, {MOD_VR, "DashPursuitT", 0.660f}, {MOD_VR, "DashPursuitR", 0.600f}, {MOD_VR, "DashPursuitB", 0.990f},
    {MOD_VR, "DashBoardL", 0.645f}, {MOD_VR, "DashBoardT", 0.000f}, {MOD_VR, "DashBoardR", 0.950f}, {MOD_VR, "DashBoardB", 0.430f},
    {MOD_VR, "DashMsgL", 0.300f}, {MOD_VR, "DashMsgT", 0.210f}, {MOD_VR, "DashMsgR", 0.700f}, {MOD_VR, "DashMsgB", 0.590f},
    {MOD_VR, "MirrorOffset", 0.0f},
    {MOD_VR, "DashBox", 0.85f},
    {MOD_VR, "DashBottomBox", 1.0f}, {MOD_VR, "DashRadarBox", 1.0f}, {MOD_VR, "DashBoardBox", 1.0f}, {MOD_VR, "DashMsgBox", 0.0f},
    {MOD_VR, "DashScreenW", 0.30f}, {MOD_VR, "DashScreenH", 0.13f},
    {MOD_VR, "DashGlass", 0.35f}, {MOD_VR, "WheelPivotRight", 0.0f}, {MOD_VR, "WheelPivotUp", 0.0f},
};
enum {
    FFB_GAIN, FFB_MAXFORCE, FFB_ALIGNING, FFB_CENTERING, FFB_DAMPING, FFB_FRICTION, FFB_SOFTLOCK, FFB_SLEW,
    FFB_SMOOTHING, FFB_INVERT, FFB_DEVICE, FFB_SOFTLOCKAT, FFB_GAMECONTROLLER, FFB_MENUCONTROLLER, FFB_MENUAXES,
    VR_HUDWIDTH, VR_HUDDISTANCE, VR_HUDHEIGHT, VR_NEARZ, VR_WHEELANGLE,
    VR_SEATRIGHT, VR_SEATUP, VR_SEATFORWARD, VR_SEATRESET, VR_HIDEARMS,
    FFB_CENTREWEIGHT, FFB_ENGINE, FFB_KERBS, FFB_SURFACE, FFB_CRASHES, FFB_SCRAPES, FFB_BREAKABLES, VR_MIRRORBRIGHTNESS, VR_MIRRORCONTRAST,
    VR_WHEELOVERHUD, VR_MENUOPEN, FFB_OFFROAD, VR_HUDPERCAR, VR_HUDRIGHT, FFB_NOS, FFB_LANDING, FFB_CARHITS,
    VR_DASH, VR_DASHRIGHT, VR_DASHUP, VR_DASHZOOM, VR_DASHCOVER,
    VR_DASHEL,                  // 15: per element (minimap, cop radar, speedometer, pursuit info, leaderboard) X, Y, Size
    VR_DASHADJUST = VR_DASHEL + 15, VR_DASHHIGHLIGHT, VR_EDITORON, VR_EDITCOMMAND, VR_EDITBRUSH,
    VR_EDITPICKING, VR_EDITLAYER, VR_EDITMODE, VR_EDITPICKED, VR_DASHDEPTH,
    VR_DASHMSG,                 // 3: the messages element's X, Y, Size
    VR_DASHCROP = VR_DASHMSG + 3,   // 24: each element's crop of the HUD (left, top, right, bottom)
    VR_MIRROROFFSET = VR_DASHCROP + 24,
    VR_DASHBOX,
    VR_DASHELBOX,               // 4: the box behind the bottom HUD, cop radar, leaderboard, messages
    VR_DASHSCREEN = VR_DASHELBOX + 4,   // 2: the display's width, height
    VR_DASHGLASS = VR_DASHSCREEN + 2,
    VR_WHEELPIVOT,              // 2: the steering wheel's turning centre moved right, up
    EXT_COUNT = VR_WHEELPIVOT + 2
};
static_assert(sizeof(kExt) / sizeof(kExt[0]) == EXT_COUNT, "kExt and the EXT enum differ");
// Each block of the EXT enum names the kExt entry at its index (a row pointing at its neighbour
// made every dashboard setting move another element).
constexpr bool KeyIs(const char* a, const char* b) { return *a == *b && (!*a || KeyIs(a + 1, b + 1)); }
static_assert(KeyIs(kExt[VR_DASH].key, "Dash"), "kExt out of order at VR_DASH");
static_assert(KeyIs(kExt[VR_DASHEL].key, "DashMapX"), "kExt out of order at VR_DASHEL");
static_assert(KeyIs(kExt[VR_DASHEL + 14].key, "DashBoardSize"), "kExt out of order at VR_DASHEL + 14");
static_assert(KeyIs(kExt[VR_DASHADJUST].key, "DashAdjust"), "kExt out of order at VR_DASHADJUST");
static_assert(KeyIs(kExt[VR_EDITBRUSH].key, "EditBrush"), "kExt out of order at VR_EDITBRUSH");
static_assert(KeyIs(kExt[VR_EDITPICKED].key, "EditPicked"), "kExt out of order at VR_EDITPICKED");
static_assert(KeyIs(kExt[VR_DASHDEPTH].key, "DashDepth"), "kExt out of order at VR_DASHDEPTH");
static_assert(KeyIs(kExt[VR_DASHMSG].key, "DashMsgX"), "kExt out of order at VR_DASHMSG");
static_assert(KeyIs(kExt[VR_DASHMSG + 2].key, "DashMsgSize"), "kExt out of order at VR_DASHMSG + 2");
static_assert(KeyIs(kExt[VR_DASHCROP].key, "DashMapL"), "kExt out of order at VR_DASHCROP");
static_assert(KeyIs(kExt[VR_DASHCROP + 8].key, "DashSpeedoL"), "kExt out of order at VR_DASHCROP + 8");
static_assert(KeyIs(kExt[VR_DASHCROP + 12].key, "DashPursuitL"), "kExt out of order at VR_DASHCROP + 12");
static_assert(KeyIs(kExt[VR_DASHCROP + 23].key, "DashMsgB"), "kExt out of order at VR_DASHCROP + 23");
static_assert(KeyIs(kExt[VR_MIRROROFFSET].key, "MirrorOffset"), "kExt out of order at VR_MIRROROFFSET");
static_assert(KeyIs(kExt[VR_DASHBOX].key, "DashBox"), "kExt out of order at VR_DASHBOX");
static_assert(KeyIs(kExt[VR_DASHELBOX].key, "DashBottomBox"), "kExt out of order at VR_DASHELBOX");
static_assert(KeyIs(kExt[VR_DASHELBOX + 3].key, "DashMsgBox"), "kExt out of order at VR_DASHELBOX + 3");
static_assert(KeyIs(kExt[VR_DASHSCREEN].key, "DashScreenW"), "kExt out of order at VR_DASHSCREEN");
static_assert(KeyIs(kExt[VR_DASHGLASS].key, "DashGlass"), "kExt out of order at VR_DASHGLASS");
static_assert(KeyIs(kExt[VR_WHEELPIVOT + 1].key, "WheelPivotUp"), "kExt out of order at VR_WHEELPIVOT + 1");
float s_ext[EXT_COUNT];         // the menu's copies, read from the mods when the menu opens

// A mod is a proxy DLL or an .asi of any name: find it by its export. With two copies loaded
// (proxy and .asi), only the running one answers for its first key.
HMODULE FindModule(const char* exportName, const char* key) {
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return nullptr;
    HMODULE any = nullptr;
    for (DWORD k = 0; k < bytes / sizeof(HMODULE) && k < 512; k++) {
        FARPROC get = GetProcAddress(mods[k], exportName);
        if (!get) continue;
        if (((float* (__cdecl*)(const char*))get)(key)) return mods[k];
        if (!any) any = mods[k];
    }
    return any;
}

HMODULE ExtModule(int mod) {
    static HMODULE m[2];
    static bool looked[2];
    if (!looked[mod]) {
        looked[mod] = true;
        const char* first = nullptr;
        for (auto& e : kExt)
            if (e.mod == mod && !first) first = e.key;
        m[mod] = FindModule(kExtMods[mod].exportName, first);
        // Older FFB builds: Gain only.
        if (!m[mod] && mod == MOD_FFB) {
            HMODULE mods[512];
            DWORD bytes = 0;
            if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes))
                for (DWORD k = 0; k < bytes / sizeof(HMODULE) && k < 512 && !m[mod]; k++)
                    if (GetProcAddress(mods[k], "NfsmwFfb_Gain")) m[mod] = mods[k];
        }
    }
    return m[mod];
}

// The mod's live value for s_ext[i], or null without the mod (or a build that lacks the key).
float* ExtSetting(int i) {
    static float* p[EXT_COUNT];
    static bool looked;
    if (!looked) {
        looked = true;
        for (int k = 0; k < EXT_COUNT; k++) {
            s_ext[k] = kExt[k].def;
            HMODULE m = ExtModule(kExt[k].mod);
            FARPROC get = m ? GetProcAddress(m, kExtMods[kExt[k].mod].exportName) : nullptr;
            p[k] = get ? ((float* (__cdecl*)(const char*))get)(kExt[k].key) : nullptr;
        }
        FARPROC gain = ExtModule(MOD_FFB) ? GetProcAddress(ExtModule(MOD_FFB), "NfsmwFfb_Gain") : nullptr;
        if (!p[FFB_GAIN] && gain) p[FFB_GAIN] = ((float* (__cdecl*)())gain)();
        if (!g_cfg.gameController && !p[FFB_GAMECONTROLLER])
            Log("ui: GameController=0 needs the force feedback mod; the game still reads the controller");
        if (!p[FFB_MENUAXES])
            Log("ui: menu controller filtering needs an updated force feedback mod");
        Log("ui: force feedback mod %s", p[FFB_GAIN] ? (p[FFB_ALIGNING] ? "found" : "found (an older version)") : "not loaded");
        Log("ui: VR mod %s", p[VR_HUDWIDTH] ? "found" : "not loaded");
    }
    return p[i];
}

// Index into s_ext of a menu value, or -1.
int ExtIndex(const void* value) {
    for (int i = 0; i < EXT_COUNT; i++)
        if (value == &s_ext[i]) return i;
    return -1;
}

std::string ExtIniPath(int i) {
    char path[MAX_PATH];
    ConfigFile(kExtMods[kExt[i].mod].ini, path, false);
    return path;
}

// The VR mod's recentre (same as its key). False without a headset.
bool VrRecenter() {
    HMODULE m = ExtModule(MOD_VR);
    FARPROC f = m ? GetProcAddress(m, "NfsmwVr_Recenter") : nullptr;
    return f && ((int(__cdecl*)())f)() != 0;
}

// The FFB mod's devices in Windows' order: "n: name (axes, buttons)", and how many there are.
// Looking the names up opens every controller, far too slow to do every frame (the menu lagged):
// cached, refreshed every few seconds.
int FfbDeviceName(int index, std::string* name) {
    static std::vector<std::string> names;
    static double fetched = -1e9;
    static bool exported = false;
    if (Now() - fetched > 3.0) {
        fetched = Now();
        names.clear();
        HMODULE m = ExtModule(MOD_FFB);
        FARPROC f = m ? GetProcAddress(m, "NfsmwFfb_DeviceName") : nullptr;
        exported = f != nullptr;
        if (f) {
            char buf[160] = {};
            int count = ((int(__cdecl*)(int, char*, int))f)(0, buf, sizeof(buf));
            for (int i = 0; i < count && i < 16; i++) {
                buf[0] = 0;
                ((int(__cdecl*)(int, char*, int))f)(i, buf, sizeof(buf));
                names.push_back(buf);
            }
        }
    }
    if (name) {
        if (index >= 0 && index < (int)names.size()) *name = names[index];
        else *name = exported ? "No such device" : "Needs the updated FFB mod";
    }
    return (int)names.size();
}

// The FFB mod's status line: whether a wheel is hooked and what holds the force up.
std::string FfbStatus() {
    static std::string text;
    static double fetched = -1e9;
    if (Now() - fetched > 0.25) {
        fetched = Now();
        HMODULE m = ExtModule(MOD_FFB);
        FARPROC f = m ? GetProcAddress(m, "NfsmwFfb_Status") : nullptr;
        char buf[200] = {};
        if (f) ((int(__cdecl*)(char*, int))f)(buf, sizeof(buf));
        text = f ? buf : (m ? "Needs the updated FFB mod" : "No FFB mod");
    }
    return text;
}

bool FfbTest() {
    HMODULE m = ExtModule(MOD_FFB);
    FARPROC f = m ? GetProcAddress(m, "NfsmwFfb_Test") : nullptr;
    return f && ((int(__cdecl*)())f)() != 0;
}

bool FfbRestart() {
    HMODULE m = ExtModule(MOD_FFB);
    FARPROC f = m ? GetProcAddress(m, "NfsmwFfb_Reset") : nullptr;
    return f && ((int(__cdecl*)())f)() != 0;
}
struct Item {
    Kind kind;
    const char* label;
    const char* section;
    const char* key;
    void* value;            // bool*, float* or std::string*
    float lo, hi, step;
    float show;             // display multiplier
    const char* fmt;
    int flags;
};

#define BINDING(label, section, key, field, flags) {BIND, label, section, key, &g_cfg.field, 0, 0, 0, 0, nullptr, flags}
#define PAGE_(title) {PAGE, title, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0}
#define HEADER_(label) {HEADER, label, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0}
#define LINK_(label, page) {LINK, label, page, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0}
#define BACK_ LINK_("< Back", kMainPage)
#define RESET_ {RESET, "Reset page to mod defaults", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0}
#define RESET_GAME_ {RESET, "Reset page to game defaults", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_GAME}

constexpr const char* kMainPage = "TUNING";

const Item kItems[] = {
    PAGE_(kMainPage),
    LINK_("Controls", "CONTROLS"),
    LINK_("Force feedback", "FORCE FEEDBACK"),
    LINK_("Shifter and clutch", "SHIFTER AND CLUTCH"),
    LINK_("Gear bindings", "GEAR BINDINGS"),
    LINK_("Engine", "ENGINE"),
    LINK_("Damage and tires", "DAMAGE AND TIRES"),
    LINK_("Physics", "PHYSICS"),
    LINK_("Traffic and HUD", "TRAFFIC AND HUD"),
    LINK_("VR", "VR"),
    LINK_("Menu and testing", "MENU AND TESTING"),

    PAGE_("CONTROLS"),
    BACK_,
    HEADER_("WHEEL AND PEDALS"),
    BINDING("Steering", "Controls", "Steering", steer, BF_STEER),
    BINDING("Accelerate", "Controls", "Accelerate", throttle, BF_PEDAL),
    BINDING("Brake", "Controls", "Brake", brake, BF_PEDAL),
    {NUMBER, "Wheel rotation", "Controls", "WheelRotation", &g_cfg.wheelRotation, 180.0f, 1800.0f, 10.0f, 1, "%.0f deg", 0},
    {NUMBER, "Steering lock", "Controls", "SteeringLock", &g_cfg.steeringLock, 90.0f, 1800.0f, 10.0f, 1, "%.0f deg", 0},
    {NUMBER, "Steering deadzone", "Controls", "SteeringDeadzone", &g_cfg.steerDeadzone, 0.0f, 0.2f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Steering linearity", "Controls", "SteeringLinearity", &g_cfg.steerLinearity, 0.5f, 3.0f, 0.05f, 1, "%.2f", 0},
    {NUMBER, "Steering sensitivity", "Controls", "SteeringSensitivity", &g_cfg.steerSensitivity, 0.25f, 4.0f, 0.05f, 1, "%.2fx", 0},
    {TOGGLE, "Speed-sensitive steering", "Controls", "SpeedSensitiveSteering", &g_cfg.steerSpeedSensitive, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Speed sensitivity", "Controls", "SpeedSensitivity", &g_cfg.steerSpeedSensitivity, 0.0f, 0.95f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Pedal deadzone", "Controls", "PedalDeadzone", &g_cfg.pedalDeadzone, 0.0f, 0.3f, 0.01f, 100, "%.0f%%", 0},
    {TOGGLE, "Game's own controller input", "Controls", "GameController", &g_cfg.gameController, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Controller input in game menus", "Controls", "MenuController", &g_cfg.menuController, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Analog axes in game menus", "Controls", "MenuAxes", &g_cfg.menuAxes, 0, 0, 0, 0, nullptr, 0},
    HEADER_("BUTTONS"),
    BINDING("Handbrake", "Controls", "Handbrake", button[0], 0),
    BINDING("Nitrous", "Controls", "Nitrous", button[1], 0),
    BINDING("Speedbreaker", "Controls", "Speedbreaker", button[2], 0),
    BINDING("Reset car", "Controls", "ResetCar", button[3], 0),
    BINDING("Change camera", "Controls", "Camera", button[4], 0),
    BINDING("Look back", "Controls", "LookBack", button[5], 0),
    BINDING("Recenter VR view", "Controls", "RecenterVr", recenterVr, 0),
    HEADER_("VR SEAT (HOLD)"),
    BINDING("Seat forward", "Controls", "SeatForward", seat[0], 0),
    BINDING("Seat back", "Controls", "SeatBack", seat[1], 0),
    BINDING("Seat left", "Controls", "SeatLeft", seat[2], 0),
    BINDING("Seat right", "Controls", "SeatRight", seat[3], 0),
    BINDING("Seat up", "Controls", "SeatUp", seat[4], 0),
    BINDING("Seat down", "Controls", "SeatDown", seat[5], 0),
    BINDING("Seat back to the driver's head", "Controls", "SeatReset", seat[6], 0),
    RESET_,
    RESET_GAME_,

    PAGE_("FORCE FEEDBACK"),
    BACK_,
    {ACTION, "Status", nullptr, "FfbStatus", nullptr, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Wheel device", "FFB", "Device", &s_ext[FFB_DEVICE], 0.0f, 7.0f, 1.0f, 1, "%.0f", 0},
    {ACTION, "Test force (pushes right)", nullptr, "TestFfb", nullptr, 0, 0, 0, 0, nullptr, 0},
    {ACTION, "Restart wheel force feedback", nullptr, "RestartFfb", nullptr, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Reverse force direction", "FFB", "Reverse", &s_ext[FFB_INVERT], 0.0f, 1.0f, 1.0f, 1, "%.0f", 0},
    HEADER_("STRENGTH AND SAFETY"),
    {NUMBER, "Strength", "FFB", "Gain", &s_ext[FFB_GAIN], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Maximum force", "FFB", "MaxForce", &s_ext[FFB_MAXFORCE], 0.1f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Smoothing", "FFB", "SmoothingHz", &s_ext[FFB_SMOOTHING], 5.0f, 60.0f, 1.0f, 1, "%.0f Hz", 0},
    {NUMBER, "Force change limit", "FFB", "SlewRate", &s_ext[FFB_SLEW], 5.0f, 60.0f, 1.0f, 1, "%.0f /s", 0},
    HEADER_("STEERING FEEL"),
    {NUMBER, "Tyre feel", "FFB", "Aligning", &s_ext[FFB_ALIGNING], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Centring", "FFB", "Centering", &s_ext[FFB_CENTERING], 0.0f, 1.5f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Centre weight", "FFB", "CentreWeight", &s_ext[FFB_CENTREWEIGHT], 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Damping", "FFB", "Damping", &s_ext[FFB_DAMPING], 0.0f, 1.5f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Friction", "FFB", "Friction", &s_ext[FFB_FRICTION], 0.0f, 0.5f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Soft lock at the steering lock", "FFB", "SoftLock", &s_ext[FFB_SOFTLOCK], 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    HEADER_("EFFECTS"),
    {NUMBER, "Engine rumble", "FFB", "EngineRumble", &s_ext[FFB_ENGINE], 0.0f, 0.5f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Kerbs and bumps", "FFB", "Kerbs", &s_ext[FFB_KERBS], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Road surface rumble", "FFB", "SurfaceRumble", &s_ext[FFB_SURFACE], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Crash jolts", "FFB", "Crashes", &s_ext[FFB_CRASHES], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Wall scrapes", "FFB", "Scrapes", &s_ext[FFB_SCRAPES], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Breakable objects", "FFB", "Breakables", &s_ext[FFB_BREAKABLES], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Landing", "FFB", "Landing", &s_ext[FFB_LANDING], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Vehicle hits", "FFB", "CarHits", &s_ext[FFB_CARHITS], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Offroad vibration", "FFB", "OffroadVibration", &s_ext[FFB_OFFROAD], 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "NOS FFB", "FFB", "NosLighter", &s_ext[FFB_NOS], 0.0f, 0.9f, 0.05f, 100, "%.0f%% lighter", 0},
    RESET_,

    PAGE_("SHIFTER AND CLUTCH"),
    BACK_,
    HEADER_("SHIFTING"),
    {TOGGLE, "Shifting", "Shifter", "Sequential", &g_cfg.sequential, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Clutch needed to shift", "Shifter", "RequireClutch", &g_cfg.requireClutch, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Clutch travel to shift", "Shifter", "ClutchToShift", &g_cfg.shiftPedal, 0.05f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Clutchless rev-match", "Shifter", "RevMatchRPM", &g_cfg.revMatchRpm, 0.0f, 3000.0f, 50.0f, 1, "%.0f rpm", 0},
    {TOGGLE, "Re-select gear after a miss", "Shifter", "ReselectAfterMiss", &g_cfg.reselectAfterMiss, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Gear keys stay in gear", "Shifter", "Latching", &g_cfg.latching, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Grind on missed clutch", "Shifter", "Grind", &g_cfg.grind, 0, 0, 0, 0, nullptr, 0},
    {SOUND, "Grind sound", "Shifter", "GrindSound", &g_cfg.grindSound, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Misshift damage chance", "Shifter", "MisshiftChance", &g_cfg.misshiftChance, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Misshift damage", "Shifter", "MisshiftDamage", &g_cfg.misshiftDamage, 0.0f, 0.5f, 0.01f, 100, "%.0f%%", 0},
    HEADER_("CLUTCH"),
    {NUMBER, "Clutch strength", "Clutch", "Strength", &g_cfg.strength, 0.1f, 2.0f, 0.1f, 1, "%.1f", 0},
    {NUMBER, "Bite point start", "Clutch", "BiteStart", &g_cfg.biteStart, 0.05f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Bite point end", "Clutch", "BiteEnd", &g_cfg.biteEnd, 0.0f, 0.95f, 0.05f, 100, "%.0f%%", 0},
    RESET_,

    PAGE_("GEAR BINDINGS"),
    BACK_,
    BINDING("Clutch", "Clutch", "Clutch", clutch, BF_CLUTCH),
    BINDING("Reverse", "Shifter", "Reverse", gear[0], 0),
    BINDING("Gear 1", "Shifter", "Gear1", gear[1], 0),
    BINDING("Gear 2", "Shifter", "Gear2", gear[2], 0),
    BINDING("Gear 3", "Shifter", "Gear3", gear[3], 0),
    BINDING("Gear 4", "Shifter", "Gear4", gear[4], 0),
    BINDING("Gear 5", "Shifter", "Gear5", gear[5], 0),
    BINDING("Gear 6", "Shifter", "Gear6", gear[6], 0),
    BINDING("Gear 7", "Shifter", "Gear7", gear[7], 0),
    BINDING("Neutral (latching)", "Shifter", "Neutral", neutral, 0),
    BINDING("Shift up (sequential)", "Shifter", "ShiftUp", shiftUp, 0),
    BINDING("Shift down (sequential)", "Shifter", "ShiftDown", shiftDown, 0),
    BINDING("H-pattern / sequential", "General", "Toggle", toggle, 0),

    PAGE_("ENGINE"),
    BACK_,
    {TOGGLE, "Engine wear", "Engine", "EngineWear", &g_cfg.engineWear, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Rev limiter", "Engine", "RevLimiter", &g_cfg.revLimiter, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Heating at redline", "Engine", "HeatUp", &g_cfg.heatUp, 0.01f, 2.0f, 0.01f, 1, "%.2f /s", 0},
    {NUMBER, "Heating at max rpm", "Engine", "HeatUpMaxRPM", &g_cfg.heatUpMax, 0.01f, 5.0f, 0.01f, 1, "%.2f /s", 0},
    {NUMBER, "Cooling", "Engine", "CoolDown", &g_cfg.coolDown, 0.005f, 1.0f, 0.005f, 1, "%.3f /s", 0},
    {NUMBER, "Power loss from", "Engine", "PowerLossStart", &g_cfg.powerLossStart, 0.0f, 0.95f, 0.05f, 100, "%.0f%% heat", 0},
    {NUMBER, "Power lost at full heat", "Engine", "PowerLoss", &g_cfg.powerLoss, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Engine smoke from", "Engine", "SmokeFrom", &g_cfg.heatSmokeFrom, 0.1f, 1.0f, 0.05f, 100, "%.0f%% heat", 0},
    RESET_,
    RESET_GAME_,

    PAGE_("DAMAGE AND TIRES"),
    BACK_,
    HEADER_("CAR DAMAGE"),
    {TOGGLE, "Car damage", "Damage", "Damage", &g_cfg.damage, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Harmless impacts up to", "Damage", "MinImpact", &g_cfg.damageMinImpact, 0.0f, 30.0f, 0.5f, 1, "%.1f m/s", 0},
    {NUMBER, "Damage strength", "Damage", "Strength", &g_cfg.damageStrength, 0.1f, 5.0f, 0.1f, 1, "%.1fx", 0},
    {NUMBER, "Power loss from", "Damage", "PowerLossStart", &g_cfg.damageLossStart, 0.0f, 0.95f, 0.05f, 100, "%.0f%% damage", 0},
    {NUMBER, "Power lost at full damage", "Damage", "PowerLoss", &g_cfg.damageLoss, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {TOGGLE, "Engine smoke when damaged", "Damage", "Smoke", &g_cfg.damageSmoke, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Engine smoke from", "Damage", "SmokeFrom", &g_cfg.smokeFrom, 0.5f, 1.0f, 0.05f, 100, "%.0f%% damage", 0},
    {NUMBER, "Engine smoke trail", "Damage", "SmokeTrail", &g_cfg.smokeTrail, 0.0f, 1.0f, 0.05f, 100, "%.0f%% of car speed", 0},
    HEADER_("POPPED TIRES"),
    {TOGGLE, "Drivable popped tires", "Tires", "PoppedTires", &g_cfg.poppedTires, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Popped tire handicap", "Tires", "Handicap", &g_cfg.tireHandicap, 0.0f, 3.0f, 0.05f, 1, "%.2fx", 0},
    RESET_,
    RESET_GAME_,

    PAGE_("PHYSICS"),
    BACK_,
    {TOGGLE, "Physics sliders", "Physics", "Physics", &g_cfg.physics, 0, 0, 0, 0, nullptr, 0},
    {CAR, "Settings for this car only", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0},
    HEADER_("1 = THE GAME'S OWN"),
    {NUMBER, "Traction control", "Physics", "TractionControl", &g_cfg.tractionControl, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Stability control", "Physics", "Stability", &g_cfg.stability, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Drift assist", "Physics", "DriftAssist", &g_cfg.driftAssist, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Grip", "Physics", "Grip", &g_cfg.grip, 0.3f, 2.0f, 0.05f, 1, "%.2fx", 0},
    {NUMBER, "Front torque share", "Physics", "TorqueSplit", &g_cfg.torqueSplit, 0.0f, 3.0f, 0.05f, 1, "%.2fx", 0},
    {NUMBER, "Drivetrain multiplication", "Physics", "Drivetrain", &g_cfg.drivetrain, 0.7f, 3.0f, 0.05f, 1, "%.2fx", 0},
    {NUMBER, "Rear grip (oversteer)", "Physics", "RearGrip", &g_cfg.rearGrip, 0.5f, 1.5f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Offroad grip", "Physics", "OffroadGrip", &g_cfg.offroadGrip, 0.2f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Slide grip", "Physics", "SlideGrip", &g_cfg.slideGrip, 0.4f, 1.0f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Slide oversteer", "Physics", "SlideOversteer", &g_cfg.slideOversteer, 0.4f, 1.0f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Rain grip", "Physics", "RainGrip", &g_cfg.rainGrip, 0.5f, 1.0f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Grip loss at speed", "Physics", "SpeedGripLoss", &g_cfg.speedGripLoss, 0.0f, 0.9f, 0.01f, 100, "%.0f%%", 0},
    {NUMBER, "Gravity", "Physics", "Gravity", &g_cfg.gravity, 0.1f, 3.0f, 0.05f, 1, "%.2f g", 0},
    {TOGGLE, "Gravity for all cars", "Physics", "GravityAllCars", &g_cfg.gravityAll, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Spin limit", "Physics", "MaxSpin", &g_cfg.maxSpin, 0.0f, 30.0f, 0.5f, 1, "%.1f rad/s", 0},
    RESET_,
    RESET_GAME_,

    PAGE_("TRAFFIC AND HUD"),
    BACK_,
    HEADER_("TRAFFIC"),
    {TOGGLE, "No SUV cops", "Traffic", "NoSuvCops", &g_cfg.noSuvCops, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Drive drag races normally", "Races", "NormalDragRaces", &g_cfg.normalDragRaces, 0, 0, 0, 0, nullptr, 0},
    HEADER_("HUD"),
    {TOGGLE, "Damage / heat meter", "HUD", "Meter", &g_cfg.hudMeter, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Speedbreaker", "HUD", "Speedbreaker", &g_cfg.speedbreaker, 0, 0, 0, 0, nullptr, 0},
    RESET_,
    RESET_GAME_,

    PAGE_("VR"),
    BACK_,
    {ACTION, "Recenter view", nullptr, "RecenterVr", nullptr, 0, 0, 0, 0, nullptr, 0},
    HEADER_("HUD PANEL"),
    {NUMBER, "HUD size", "VR", "HudWidth", &s_ext[VR_HUDWIDTH], 0.5f, 3.0f, 0.05f, 100.0f / 1.4f, "%.0f%%", 0},
    {NUMBER, "HUD distance", "VR", "HudDistance", &s_ext[VR_HUDDISTANCE], 0.4f, 3.0f, 0.05f, 1, "%.2f m", 0},
    {NUMBER, "HUD height", "VR", "HudHeight", &s_ext[VR_HUDHEIGHT], -0.6f, 0.6f, 0.05f, 1, "%+.2f m", 0},
    {NUMBER, "HUD left / right", "VR", "HudRight", &s_ext[VR_HUDRIGHT], -0.8f, 0.8f, 0.05f, 1, "%+.2f m", 0},
    {NUMBER, "Each car remembers HUD positioning", "VR", "HudPerCar", &s_ext[VR_HUDPERCAR], 0.0f, 1.0f, 1.0f, 1, "%.0f", 0},
    HEADER_("IN-CAR DASHBOARD"),
    {NUMBER, "In-car dashboard", "VR", "Dash", &s_ext[VR_DASH], 0.0f, 1.0f, 1.0f, 1, "%.0f", 0},
    {NUMBER, "Whole dashboard size (this car)", "VR", "DashZoom", &s_ext[VR_DASHZOOM], 0.3f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Dashboard left / right", "VR", "DashRight", &s_ext[VR_DASHRIGHT], -0.4f, 0.4f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Dashboard up / down", "VR", "DashUp", &s_ext[VR_DASHUP], -0.4f, 0.4f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Car dashboard GUI depth", "VR", "DashDepth", &s_ext[VR_DASHDEPTH], -0.15f, 0.25f, 0.0025f, 100, "%+.2f cm", 0},
    {NUMBER, "Display width", "VR", "DashScreenW", &s_ext[VR_DASHSCREEN], 0.04f, 0.8f, 0.005f, 100, "%.1f cm", 0},
    {NUMBER, "Display height", "VR", "DashScreenH", &s_ext[VR_DASHSCREEN + 1], 0.02f, 0.5f, 0.005f, 100, "%.1f cm", 0},
    {NUMBER, "Display opacity", "VR", "DashBox", &s_ext[VR_DASHBOX], 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Display glass opacity", "VR", "DashGlass", &s_ext[VR_DASHGLASS], 0.0f, 1.0f, 0.05f, 100, "%.0f%%", 0},
    LINK_("Adjust dashboard (each element, live)", "ADJUST DASHBOARD"),
    LINK_("Triangle editor (hide, black, steering wheel)", "TRIANGLE EDITOR"),
    HEADER_("VIEW"),
    {NUMBER, "Hide the driver's arms and hands", "VR", "HideArms", &s_ext[VR_HIDEARMS], 0.0f, 1.0f, 1.0f, 1, "%.0f", 0},
    {NUMBER, "Steering wheel rendered in front of HUD", "VR", "WheelOverHud", &s_ext[VR_WHEELOVERHUD], 0.0f, 1.0f, 1.0f, 1, "%.0f", 0},
    {NUMBER, "Mirror brightness", "VR", "MirrorBrightness", &s_ext[VR_MIRRORBRIGHTNESS], 0.5f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Mirror contrast", "VR", "MirrorContrast", &s_ext[VR_MIRRORCONTRAST], 0.5f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Near clip plane", "VR", "NearZ", &s_ext[VR_NEARZ], 0.001f, 0.25f, 0.001f, 100, "%.1f cm", 0},
    RESET_,

    PAGE_("ADJUST DASHBOARD"),
    LINK_("< Back", "VR"),
    {NUMBER, "Whole dashboard size (this car)", "VR", "DashZoom", &s_ext[VR_DASHZOOM], 0.3f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    HEADER_("MINIMAP"),
    {NUMBER, "Minimap left / right", "VR", "DashMapX", &s_ext[VR_DASHEL + 0], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Minimap up / down", "VR", "DashMapY", &s_ext[VR_DASHEL + 0 + 1], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Minimap size", "VR", "DashMapSize", &s_ext[VR_DASHEL + 0 + 2], 0.0f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Minimap crop left", "VR", "DashMapL", &s_ext[VR_DASHCROP + 0], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Minimap crop top", "VR", "DashMapT", &s_ext[VR_DASHCROP + 1], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Minimap crop right", "VR", "DashMapR", &s_ext[VR_DASHCROP + 2], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Minimap crop bottom", "VR", "DashMapB", &s_ext[VR_DASHCROP + 3], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    HEADER_("COP RADAR"),
    {NUMBER, "Cop radar left / right", "VR", "DashRadarX", &s_ext[VR_DASHEL + 3], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Cop radar up / down", "VR", "DashRadarY", &s_ext[VR_DASHEL + 3 + 1], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Cop radar size", "VR", "DashRadarSize", &s_ext[VR_DASHEL + 3 + 2], 0.0f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Cop radar crop left", "VR", "DashRadarL", &s_ext[VR_DASHCROP + 4], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Cop radar crop top", "VR", "DashRadarT", &s_ext[VR_DASHCROP + 5], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Cop radar crop right", "VR", "DashRadarR", &s_ext[VR_DASHCROP + 6], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Cop radar crop bottom", "VR", "DashRadarB", &s_ext[VR_DASHCROP + 7], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    HEADER_("SPEEDOMETER"),
    {NUMBER, "Speedometer left / right", "VR", "DashSpeedoX", &s_ext[VR_DASHEL + 6], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Speedometer up / down", "VR", "DashSpeedoY", &s_ext[VR_DASHEL + 6 + 1], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Speedometer size", "VR", "DashSpeedoSize", &s_ext[VR_DASHEL + 6 + 2], 0.0f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Speedometer crop left", "VR", "DashSpeedoL", &s_ext[VR_DASHCROP + 8], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Speedometer crop top", "VR", "DashSpeedoT", &s_ext[VR_DASHCROP + 9], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Speedometer crop right", "VR", "DashSpeedoR", &s_ext[VR_DASHCROP + 10], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Speedometer crop bottom", "VR", "DashSpeedoB", &s_ext[VR_DASHCROP + 11], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    HEADER_("PURSUIT INFO"),
    {NUMBER, "Pursuit info left / right", "VR", "DashPursuitX", &s_ext[VR_DASHEL + 9], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Pursuit info up / down", "VR", "DashPursuitY", &s_ext[VR_DASHEL + 9 + 1], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Pursuit info size", "VR", "DashPursuitSize", &s_ext[VR_DASHEL + 9 + 2], 0.0f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Pursuit info crop left", "VR", "DashPursuitL", &s_ext[VR_DASHCROP + 12], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Pursuit info crop top", "VR", "DashPursuitT", &s_ext[VR_DASHCROP + 13], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Pursuit info crop right", "VR", "DashPursuitR", &s_ext[VR_DASHCROP + 14], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Pursuit info crop bottom", "VR", "DashPursuitB", &s_ext[VR_DASHCROP + 15], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    HEADER_("LEADERBOARD"),
    {NUMBER, "Leaderboard left / right", "VR", "DashBoardX", &s_ext[VR_DASHEL + 12], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Leaderboard up / down", "VR", "DashBoardY", &s_ext[VR_DASHEL + 12 + 1], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Leaderboard size", "VR", "DashBoardSize", &s_ext[VR_DASHEL + 12 + 2], 0.0f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Leaderboard crop left", "VR", "DashBoardL", &s_ext[VR_DASHCROP + 16], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Leaderboard crop top", "VR", "DashBoardT", &s_ext[VR_DASHCROP + 17], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Leaderboard crop right", "VR", "DashBoardR", &s_ext[VR_DASHCROP + 18], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Leaderboard crop bottom", "VR", "DashBoardB", &s_ext[VR_DASHCROP + 19], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},

    HEADER_("MESSAGES (BUSTED, TOTALED)"),
    {NUMBER, "Messages left / right", "VR", "DashMsgX", &s_ext[VR_DASHMSG], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Messages up / down", "VR", "DashMsgY", &s_ext[VR_DASHMSG + 1], -0.6f, 0.6f, 0.005f, 100, "%+.1f cm", 0},
    {NUMBER, "Messages size", "VR", "DashMsgSize", &s_ext[VR_DASHMSG + 2], 0.0f, 3.0f, 0.05f, 100, "%.0f%%", 0},
    {NUMBER, "Messages crop left", "VR", "DashMsgL", &s_ext[VR_DASHCROP + 20], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Messages crop top", "VR", "DashMsgT", &s_ext[VR_DASHCROP + 21], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Messages crop right", "VR", "DashMsgR", &s_ext[VR_DASHCROP + 22], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},
    {NUMBER, "Messages crop bottom", "VR", "DashMsgB", &s_ext[VR_DASHCROP + 23], 0.0f, 1.0f, 0.0025f, 100, "%.2f%%", 0},

    PAGE_("TRIANGLE EDITOR"),
    LINK_("< Back", "VR"),
    HEADER_("PICK"),
    {ACTION, "Pick triangles (the menu hides)", nullptr, "EdPick", nullptr, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Selection size", "VR", "EditMode", &s_ext[VR_EDITMODE], 0.0f, 5.0f, 1.0f, 1, "%.0f", 0},
    HEADER_("DO WITH THE SELECTION"),
    {ACTION, "Hide", nullptr, "EdHide", nullptr, 0, 0, 0, 0, nullptr, 0},
    {ACTION, "Draw black", nullptr, "EdBlack", nullptr, 0, 0, 0, 0, nullptr, 0},
    {ACTION, "Add to the steering wheel", nullptr, "EdWheel", nullptr, 0, 0, 0, 0, nullptr, 0},
    {ACTION, "Take off the steering wheel", nullptr, "EdNoWheel", nullptr, 0, 0, 0, 0, nullptr, 0},
    {ACTION, "Restore", nullptr, "EdRestore", nullptr, 0, 0, 0, 0, nullptr, 0},
    {ACTION, "Undo", nullptr, "EdUndo", nullptr, 0, 0, 0, 0, nullptr, 0},

    PAGE_("MENU AND TESTING"),
    BACK_,
    HEADER_("MENU KEYS"),
    BINDING("Menu", "UI", "Menu", menu, BF_REQUIRED),
    BINDING("Menu up", "UI", "MenuUp", menuUp, BF_REQUIRED),
    BINDING("Menu down", "UI", "MenuDown", menuDown, BF_REQUIRED),
    BINDING("Menu left", "UI", "MenuLeft", menuLeft, BF_REQUIRED),
    BINDING("Menu right", "UI", "MenuRight", menuRight, BF_REQUIRED),
    HEADER_("TESTING"),
    BINDING("Pop tires key", "Tires", "DebugPopTires", debugPopTires, 0),
    {ACTION, "Pop all four tires now", nullptr, "PopTires", nullptr, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Drop height", "Tires", "DropHeight", &g_cfg.dropHeight, 0.5f, 50.0f, 0.5f, 1, "%.1f m", 0},
    {ACTION, "Drop from air", nullptr, "DropCar", nullptr, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Rear view mirror forward / back", "VR", "MirrorOffset", &s_ext[VR_MIRROROFFSET], -0.05f, 0.05f, 0.001f, 100, "%+.1f cm", 0},
    {ACTION, "Center steering wheel animation", nullptr, "EdWheelPivot", nullptr, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Steering wheel centre left / right", "VR", "WheelPivotRight", &s_ext[VR_WHEELPIVOT], -0.08f, 0.08f, 0.001f, 100, "%+.1f cm", 0},
    {NUMBER, "Steering wheel centre up / down", "VR", "WheelPivotUp", &s_ext[VR_WHEELPIVOT + 1], -0.08f, 0.08f, 0.001f, 100, "%+.1f cm", 0},
    {ACTION, "Steering wheel centre: reset", nullptr, "EdWheelPivotReset", nullptr, 0, 0, 0, 0, nullptr, 0},
    HEADER_("LOOK"),
    {TOGGLE, "Classic menu look", "UI", "ClassicMenu", &g_cfg.classicMenu, 0, 0, 0, 0, nullptr, 0},
};
constexpr int kNumItems = sizeof(kItems) / sizeof(kItems[0]);
constexpr int kMaxRows = 18;        // rows shown at once; the list scrolls

struct {
    bool open = false;
    bool dirty = false;
    bool clutchCal = false;         // [Clutch] Released/Pressed changed by binding a pedal
    bool pedalCal = false;          // [Controls] AccelerateRest/BrakeRest changed
    int page = 0;                   // index of the current page's PAGE item in kItems
    int sel = 1;                    // selected item (index into kItems)
    int selOnPage[kNumItems] = {};  // last selection on each page (by PAGE index), 0 = none
    int first = 0;                  // first row shown
    bool synced = false;
    uint32_t menu = 0, nav[4] = {};
    int capture = -1;               // item waiting for a press
    double captureStart = 0;
    std::string note;               // short feedback under the list
    double noteTime = -1e9;
    int resetArmed = -1;            // RESET item pressed once (press again to confirm)
    double resetTime = 0;
} s_menu;

// Sound moments for a missed shift: the totaled car's engine failure, and the two the engine
// sound plays once when its engine state changes (SFXCTL_Engine::UpdateParams, states 1 and 2).
const struct {
    uint32_t moment;
    const char* name;
} kGrindSounds[] = {
    {0xBC2DFA2F, "Gear grind"},
    {0xBAE41D1B, "Engine state 2"},
    {0xC565AC30, "Totaled"},
};

std::string SoundName(uint32_t moment) {
    if (!moment) return "Off";
    for (auto& s : kGrindSounds)
        if (s.moment == moment) return s.name;
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", moment);
    return buf;
}

// The items on the current page, in order.
std::vector<int> PageRows() {
    std::vector<int> rows;
    for (int i = s_menu.page + 1; i < kNumItems && kItems[i].kind != PAGE; i++) rows.push_back(i);
    return rows;
}

bool Selectable(int i) { return kItems[i].kind != HEADER && kItems[i].kind != PAGE; }

// Opens a page by its title, on the item last selected there (else its first item).
void SetPage(const char* title) {
    s_menu.selOnPage[s_menu.page] = s_menu.sel;
    for (int i = 0; i < kNumItems; i++)
        if (kItems[i].kind == PAGE && strcmp(kItems[i].label, title) == 0) s_menu.page = i;
    s_menu.first = 0;
    s_menu.sel = s_menu.selOnPage[s_menu.page];
    if (s_menu.sel > s_menu.page && Selectable(s_menu.sel)) return;
    for (int i : PageRows())
        if (Selectable(i)) {
            s_menu.sel = i;
            break;
        }
}

// Back to the page that links to this one (a sub-page's parent, e.g. VR), on that link; for a
// category, the main page.
void GoBack() {
    if (s_menu.page == 0) return;
    const char* from = kItems[s_menu.page].label;
    const char* parent = kMainPage;
    for (int i = 1, page = 0; i < kNumItems; i++) {
        if (kItems[i].kind == PAGE) page = i;
        else if (page && kItems[i].kind == LINK && strcmp(kItems[i].section, from) == 0) parent = kItems[page].label;
    }
    SetPage(parent);
    for (int i : PageRows())
        if (kItems[i].kind == LINK && strcmp(kItems[i].section, from) == 0) s_menu.sel = i;
}

std::vector<std::string> SplitBindings(const std::string& list) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string t = list.substr(i, j - i);
        t.erase(0, t.find_first_not_of(" \t"));
        t.erase(t.find_last_not_of(" \t") + 1);
        if (!t.empty()) out.push_back(t);
        i = j + 1;
    }
    return out;
}

std::string JoinBindings(const std::vector<std::string>& v) {
    std::string s;
    for (auto& t : v) s += (s.empty() ? "" : ",") + t;
    return s;
}

bool SameBinding(std::string a, std::string b) {
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    for (auto& c : b) c = (char)tolower((unsigned char)c);
    return a == b;
}

void Note(const std::string& text) {
    s_menu.note = text;
    s_menu.noteTime = Now();
}

// One line on the main page about what a category is set to.
std::string LinkSummary(const char* page) {
    char buf[64];
    std::string p = page;
    if (p == "CONTROLS") {
        if (g_cfg.steer.empty()) return "Wheel not bound";
        snprintf(buf, sizeof(buf), "Wheel, lock %.0f of %.0f deg", g_cfg.steeringLock, g_cfg.wheelRotation);
        return buf;
    }
    if (p == "FORCE FEEDBACK") {
        if (!ExtSetting(FFB_GAIN)) return "No FFB mod";
        snprintf(buf, sizeof(buf), "%.0f%%", *ExtSetting(FFB_GAIN) * 100.0f);
        return buf;
    }
    if (p == "SHIFTER AND CLUTCH") return g_cfg.sequential ? "Sequential" : "H-pattern";
    if (p == "GEAR BINDINGS") {
        int gears = 0;
        for (int g = 1; g <= kMaxGears; g++) gears += !g_cfg.gear[g].empty();
        snprintf(buf, sizeof(buf), "%d gears%s", gears, g_cfg.clutch.empty() ? "" : ", clutch");
        return buf;
    }
    if (p == "ENGINE") return g_cfg.engineWear ? "Wear on" : "Wear off";
    if (p == "DAMAGE AND TIRES") return g_cfg.damage ? "Damage on" : "Damage off";
    if (p == "PHYSICS") return !g_cfg.physics ? "Off" : *Physics_Car() && Physics_OwnSettings() ? "This car's own" : "On";
    if (p == "TRAFFIC AND HUD") return g_cfg.noSuvCops ? "No SUV cops" : "SUV cops";
    if (p == "VR") {
        if (!ExtSetting(VR_HUDWIDTH)) return kExtMods[MOD_VR].missing;
        snprintf(buf, sizeof(buf), "HUD %.0f%%", *ExtSetting(VR_HUDWIDTH) / kExt[VR_HUDWIDTH].def * 100.0f);
        return buf;
    }
    return "";
}

// Triangle editor picking: the menu hides and the VR mod's selection follows the gaze. Menu up / down
// step through the triangles along it (nearest first), left / right change the selection's size;
// the menu key or Enter keeps the selection and brings the menu back, Escape leaves it.
const char* const kPickSizes[6] = {"1 triangle", "Within 2 cm", "Within 5 cm", "Within 10 cm", "Connected piece", "Whole draw entry"};
bool s_picking = false;

void UpdateVrPages();
void SetOpen(bool open);

void StartPicking() {
    float* picking = ExtSetting(VR_EDITPICKING);
    if (!picking) {
        Note("No VR mod");
        return;
    }
    *picking = 1;
    if (float* layer = ExtSetting(VR_EDITLAYER)) *layer = 0;
    s_picking = true;
    Log("ui: picking triangles");
    SetOpen(false);
}

void StopPicking(bool keep) {
    s_picking = false;
    if (float* picking = ExtSetting(VR_EDITPICKING)) *picking = 0;
    SetOpen(true);
    float* picked = ExtSetting(VR_EDITPICKED);
    int n = picked ? (int)lrintf(*picked) : 0;
    Note(keep ? (n ? "Picked " + std::to_string(n) + (n == 1 ? " triangle" : " triangles") : std::string("Nothing picked"))
              : std::string("Picking cancelled"));
    Log("ui: picking %s (%d triangles)", keep ? "done" : "cancelled", n);
}

// While picking, in place of the menu's own keys.
void UpdatePicking(int toggles, const int nav[4]) {
    static bool enterWas = true, escWas = true;
    bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0, esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    bool enterHit = enter && !enterWas, escHit = esc && !escWas;
    enterWas = enter;
    escWas = esc;
    if (escHit) {
        if (float* cmd = ExtSetting(VR_EDITCOMMAND)) *cmd = 9;   // the selection dropped
        StopPicking(false);
        return;
    }
    if ((toggles & 1) || enterHit) {
        StopPicking(true);
        return;
    }
    if (float* layer = ExtSetting(VR_EDITLAYER)) *layer = std::max(0.0f, *layer - nav[0] + nav[1]);
    if (float* mode = ExtSetting(VR_EDITMODE)) {
        *mode = Clamp(*mode - nav[2] + nav[3], 0.0f, 5.0f);
        s_ext[VR_EDITMODE] = *mode;
    }
}

std::string ValueText(const Item& it, int index) {
    char buf[64];
    if (it.value == &g_cfg.sequential) return g_cfg.sequential ? "Sequential" : "H-pattern";
    if (it.kind == TOGGLE) return *(bool*)it.value ? "On" : "Off";
    if (it.kind == CAR) return *Physics_Car() ? (Physics_OwnSettings() ? "On" : "Off") : "No car";
    if (it.kind == LINK) return it.section == kMainPage ? "" : LinkSummary(it.section) + "  >";
    if (it.kind == ACTION && strcmp(it.key, "FfbStatus") == 0) return FfbStatus();
    if (it.kind == ACTION) return "Go";
    if (it.kind == RESET) return s_menu.resetArmed >= 0 && Now() - s_menu.resetTime < 3.0 ? "Sure?" : "Reset";
    if (it.kind == SOUND) return SoundName(*(uint32_t*)it.value);
    if (it.kind == BIND) {
        if (index == s_menu.capture) return it.flags & BF_STEER ? "Turn the wheel..." : "Press a key, button or pedal...";
        std::string s;
        for (auto& t : SplitBindings(*(std::string*)it.value))
            s += (s.empty() ? "" : ", ") + (t.compare(0, 4, "key:") == 0 ? t.substr(4) : t);
        if (s.empty()) return "-";
        if (s.size() > 28) s = s.substr(0, 25) + "...";
        return s;
    }
    int ext = ExtIndex(it.value);
    if (ext >= 0 && !ExtSetting(ext)) return kExtMods[kExt[ext].mod].missing;
    float v = *(float*)it.value;
    if (it.value == &g_cfg.revMatchRpm && v <= 0.0f) return "Off";
    if (it.value >= (void*)&s_ext[VR_DASHELBOX] && it.value < (void*)&s_ext[VR_DASHELBOX + 4]) return v > 0.5f ? "On" : "Off";
    for (int el = 0; el < 5; el++)
        if (it.value == &s_ext[VR_DASHEL + el * 3 + 2] && v <= 0.001f) return "Hidden";
    if (it.value == &s_ext[VR_DASHMSG + 2] && v <= 0.001f) return "Hidden";
    if (it.value == &s_ext[VR_EDITBRUSH] && v <= 0.0f) return "1 triangle";
    if (it.value == &s_ext[VR_EDITMODE]) return kPickSizes[std::max(0, std::min(5, (int)lrintf(v)))];
    if (it.value == &s_ext[FFB_INVERT] || it.value == &s_ext[VR_HIDEARMS] || it.value == &s_ext[VR_WHEELOVERHUD] ||
        it.value == &s_ext[VR_HUDPERCAR] || it.value == &s_ext[VR_DASH])
        return v > 0.5f ? "On" : "Off";
    if (it.value == &s_ext[FFB_DEVICE]) {
        std::string name;
        FfbDeviceName((int)lrintf(v), &name);
        return name;
    }
    snprintf(buf, sizeof(buf), it.fmt, v * it.show);
    return buf;
}

void StartCapture(int index) {
    s_menu.capture = index;
    s_menu.captureStart = Now();
    Input_BeginCapture();
    Log("ui: binding %s", kItems[index].label);
}

void FinishCapture(const std::string& binding, int axisRest) {
    const Item& it = kItems[s_menu.capture];
    s_menu.capture = -1;
    // The menu key closes capture instead of being bound (except to the menu item itself).
    if (it.value != &g_cfg.menu)
        for (auto& t : SplitBindings(g_cfg.menu))
            if (SameBinding(t, binding)) {
                Note("Cancelled");
                return;
            }
    std::string& field = *(std::string*)it.value;
    std::string b = binding;
    if (it.flags & BF_STEER) {
        // An axis; which way it was turned doesn't matter.
        if (axisRest < 0) {
            Note("Steering needs an axis: turn the wheel");
            return;
        }
        size_t minus = b.find(":-");
        if (minus != std::string::npos) b.erase(minus + 1, 1);
    }
    std::vector<std::string> list = SplitBindings(field);
    for (auto& t : list)
        if (SameBinding(t, b)) {
            Note(std::string(it.label) + ": " + b + " is already bound");
            return;
        }
    if (it.flags & BF_STEER) list.clear();      // one steering axis
    list.push_back(b);
    if ((it.flags & BF_CLUTCH) && axisRest >= 0) {
        // Pedal: remember where it rests; floored is taken to be the other end of its travel.
        g_cfg.clutchReleased = axisRest;
        g_cfg.clutchPressed = -1;
        s_menu.clutchCal = true;
    }
    if ((it.flags & BF_PEDAL) && axisRest >= 0) {
        // Pedal: remember where it rests (pedals sharing one axis rest in the middle).
        (it.value == &g_cfg.throttle ? g_cfg.throttleRest : g_cfg.brakeRest) = axisRest;
        s_menu.pedalCal = true;
    }
    Input_SetBinding(field, JoinBindings(list));
    s_menu.dirty = true;
    Note(std::string(it.label) + ": added " + b);
    Log("ui: %s = %s", it.key, field.c_str());
}

// The same field in a default Config.
const std::string& DefaultBinding(const Item& it) {
    static const Config def;
    return *(const std::string*)((const char*)&def + ((const char*)it.value - (const char*)&g_cfg));
}

void Unbind(const Item& it) {
    std::string& field = *(std::string*)it.value;
    std::string value = (it.flags & BF_REQUIRED) ? DefaultBinding(it) : std::string();
    if (SplitBindings(field) == SplitBindings(value)) {
        Note(std::string(it.label) + (value.empty() ? ": nothing to unbind" : ": already the default"));
        return;
    }
    if (it.flags & BF_PEDAL) {
        (it.value == &g_cfg.throttle ? g_cfg.throttleRest : g_cfg.brakeRest) = -1;
        s_menu.pedalCal = true;
    }
    Input_SetBinding(field, value);
    s_menu.dirty = true;
    Note(std::string(it.label) + (value.empty() ? ": unbound" : ": back to default"));
    Log("ui: %s = %s", it.key, field.c_str());
}

// The default of a setting, from a default Config (or the FFB mod's defaults). False for items
// without one (bindings, actions, per-mod values outside g_cfg).
bool DefaultOf(const Item& it, float* f, bool* b, uint32_t* u) {
    static const Config def;
    const char* base = (const char*)&g_cfg;
    const char* p = (const char*)it.value;
    if (it.kind == NUMBER) {
        int ext = ExtIndex(it.value);
        if (ext >= 0) {
            *f = kExt[ext].def;
            return true;
        }
    }
    if (!p || p < base || p >= base + sizeof(Config)) return false;
    const char* d = (const char*)&def + (p - base);
    if (it.kind == NUMBER) *f = *(const float*)d;
    else if (it.kind == TOGGLE) *b = *(const bool*)d;
    else if (it.kind == SOUND) *u = *(const uint32_t*)d;
    else return false;
    return true;
}

// What the unmodded game does, for settings where that means something (toggles as 0 / 1).
const struct {
    const void* value;
    float game;
} kGameDefaults[] = {
    {&g_cfg.gameController, 1}, {&g_cfg.steerLinearity, 1}, {&g_cfg.steerSensitivity, 1},
    {&g_cfg.menuController, 1}, {&g_cfg.menuAxes, 1},
    {&g_cfg.engineWear, 0}, {&g_cfg.revLimiter, 1},
    {&g_cfg.damage, 0}, {&g_cfg.damageSmoke, 0}, {&g_cfg.poppedTires, 0},
    {&g_cfg.physics, 0}, {&g_cfg.tractionControl, 1}, {&g_cfg.stability, 1}, {&g_cfg.driftAssist, 1},
    {&g_cfg.grip, 1}, {&g_cfg.torqueSplit, 1}, {&g_cfg.drivetrain, 1}, {&g_cfg.rearGrip, 1}, {&g_cfg.offroadGrip, 1},
    {&g_cfg.slideGrip, 1}, {&g_cfg.slideOversteer, 1}, {&g_cfg.rainGrip, 1}, {&g_cfg.speedGripLoss, 1}, {&g_cfg.gravity, 1}, {&g_cfg.gravityAll, 0},
    {&g_cfg.maxSpin, 0},
    {&g_cfg.noSuvCops, 0}, {&g_cfg.normalDragRaces, 0},
    {&g_cfg.hudMeter, 0}, {&g_cfg.speedbreaker, 1},
};

bool GameDefaultOf(const Item& it, float* f) {
    if (it.kind != NUMBER && it.kind != TOGGLE) return false;
    for (auto& g : kGameDefaults)
        if (g.value == it.value) {
            *f = g.game;
            return true;
        }
    return false;
}

// The setting differs from its mod default (game = false) or its game default (game = true).
bool Differs(const Item& it, bool game) {
    float f;
    bool b = false;
    uint32_t u;
    if (game) {
        if (!GameDefaultOf(it, &f)) return false;
        b = f != 0.0f;
    } else if (!DefaultOf(it, &f, &b, &u)) {
        return false;
    }
    if (it.kind == NUMBER) return fabsf(*(float*)it.value - f) > it.step * 0.25f;
    if (it.kind == TOGGLE) return *(bool*)it.value != b;
    return *(uint32_t*)it.value != u;
}
bool Changed(const Item& it) { return Differs(it, false); }

void AfterNumberChange(const Item& it);

// Puts one setting back to its mod or game default. True if it changed.
bool ResetItem(const Item& it, bool game = false) {
    float f = 0.0f;
    bool b = false;
    uint32_t u = 0;
    if (!Differs(it, game)) return false;
    if (game) {
        if (!GameDefaultOf(it, &f)) return false;
        b = f != 0.0f;
    } else if (!DefaultOf(it, &f, &b, &u)) {
        return false;
    }
    if (it.kind == NUMBER) {
        int ext = ExtIndex(it.value);
        if (ext >= 0 && !ExtSetting(ext)) return false;
        *(float*)it.value = f;
        AfterNumberChange(it);
    } else if (it.kind == TOGGLE) {
        *(bool*)it.value = b;
    } else {
        *(uint32_t*)it.value = u;
    }
    s_menu.dirty = true;
    return true;
}

// Every setting on the page holding item `index`.
int ResetPage(int index, bool game) {
    int page = index;
    while (page > 0 && kItems[page].kind != PAGE) page--;
    int n = 0;
    for (int i = page + 1; i < kNumItems && kItems[i].kind != PAGE; i++) n += ResetItem(kItems[i], game);
    return n;
}

// After a number changed (bindings or mouse): pass the other mods' values on, keep pairs consistent.
void AfterNumberChange(const Item& it) {
    if (it.value == &s_ext[FFB_DEVICE]) {
        int count = FfbDeviceName(0, nullptr);
        s_ext[FFB_DEVICE] = Clamp(s_ext[FFB_DEVICE], 0.0f, (float)std::max(count - 1, 0));
    }
    int ext = ExtIndex(it.value);
    if (ext >= 0 && ExtSetting(ext)) *ExtSetting(ext) = *(float*)it.value;
    // The steering lock is part of the wheel's rotation.
    if (g_cfg.steeringLock > g_cfg.wheelRotation) {
        if (it.value == &g_cfg.steeringLock) g_cfg.steeringLock = g_cfg.wheelRotation;
        else g_cfg.wheelRotation = g_cfg.steeringLock;
    }
    // Keep the bite point ordered: start (released side) above end.
    if (g_cfg.biteStart < g_cfg.biteEnd + 0.05f) {
        if (it.value == &g_cfg.biteStart) g_cfg.biteEnd = std::max(0.0f, g_cfg.biteStart - 0.05f);
        else if (it.value == &g_cfg.biteEnd) g_cfg.biteStart = std::min(1.0f, g_cfg.biteEnd + 0.05f);
    }
    s_menu.dirty = true;
}

void Change(int index, int dir) {
    const Item& it = kItems[index];
    if (it.kind == TOGGLE) {
        bool& b = *(bool*)it.value;
        b = !b;
    } else if (it.kind == NUMBER) {
        int ext = ExtIndex(it.value);
        if (ext >= 0 && !ExtSetting(ext)) return;
        float& v = *(float*)it.value;
        v = Clamp(roundf((v + dir * it.step) / it.step) * it.step, it.lo, it.hi);
        AfterNumberChange(it);
    } else if (it.kind == LINK) {
        if (it.section == kMainPage || dir < 0) GoBack();
        else SetPage(it.section);
        return;
    } else if (it.kind == SOUND) {
        // Cycle through the candidates, playing each one.
        uint32_t& v = *(uint32_t*)it.value;
        int n = (int)(sizeof(kGrindSounds) / sizeof(kGrindSounds[0])), cur = n - 1;
        for (int i = 0; i < n; i++)
            if (kGrindSounds[i].moment == v) cur = i;
        v = kGrindSounds[(cur + dir + n) % n].moment;
        Game_PlayMoment(v);
    } else if (it.kind == ACTION) {
        if (dir < 0) {
            GoBack();
            return;
        }
        if (strncmp(it.key, "Ed", 2) == 0) {
            static const struct { const char* key; int cmd; const char* note; } kEd[] = {
                {"EdHide", 1, "Hidden"}, {"EdBlack", 2, "Drawn black"}, {"EdWheel", 3, "On the steering wheel"},
                {"EdNoWheel", 4, "Off the steering wheel"}, {"EdRestore", 5, "Restored"}, {"EdUndo", 6, "Undone"},
                {"EdDash", 7, "Added to the dashboard blackout"}, {"EdNoDash", 8, "Taken off the dashboard blackout"},
                {"EdWheelPivot", 10, "Steering wheel animation centred"}, {"EdWheelPivotReset", 11, "Steering wheel centre reset"}};
            if (strcmp(it.key, "EdPick") == 0) {
                StartPicking();
                return;
            }
            float* cmd = ExtSetting(VR_EDITCOMMAND);
            for (const auto& e : kEd)
                if (strcmp(it.key, e.key) == 0) {
                    if (cmd) *cmd = (float)e.cmd;
                    Note(cmd ? e.note : "No VR mod");
                }
            return;
        }
        if (strcmp(it.key, "RecenterVr") == 0) {
            Note(VrRecenter() ? "View recentred" : !ExtSetting(VR_HUDWIDTH) ? "No VR mod" : "No headset");
            return;
        }
        if (strcmp(it.key, "FfbStatus") == 0) {
            Note(FfbStatus());
            return;
        }
        if (strcmp(it.key, "TestFfb") == 0) {
            Note(FfbTest() ? "Pushing right: if it pushed left, turn on Reverse force direction" : "Needs updated FFB mod with NativeFFB=0");
            return;
        }
        if (strcmp(it.key, "RestartFfb") == 0) {
            Note(FfbRestart() ? "FFB restart requested" : "Needs updated FFB mod with NativeFFB=0");
            return;
        }
        if (strcmp(it.key, "DropCar") == 0) {
            Physics_DropCar();
            Note("Dropping the car");
            return;
        }
        Game_PopTires();
        Note("Tires popped (repair: safehouse, race restart or DebugRepair)");
        return;
    } else if (it.kind == RESET) {
        if (dir < 0) {
            s_menu.resetArmed = -1;
            GoBack();
            return;
        }
        if (s_menu.resetArmed != index || Now() - s_menu.resetTime > 3.0) {
            s_menu.resetArmed = index;
            s_menu.resetTime = Now();
            Note(std::string("Press again to reset this page to the ") + (it.flags & BF_GAME ? "game's" : "mod's") +
                 " defaults (bindings are kept)");
            return;
        }
        s_menu.resetArmed = -1;
        bool game = (it.flags & BF_GAME) != 0;
        int n = ResetPage(index, game);
        Note(n ? std::to_string(n) + (n == 1 ? " setting" : " settings") + (game ? " back to the game's default" : " back to the mod's default")
               : "Already at the defaults");
        Log("ui: page reset to %s defaults, %d settings", game ? "game" : "mod", n);
        return;
    } else if (it.kind == CAR) {
        if (!*Physics_Car()) return;
        Physics_SetOwnSettings(!Physics_OwnSettings());
        Note(std::string(Physics_Car()) + (Physics_OwnSettings() ? ": own physics settings" : ": back to the shared physics settings"));
    } else if (it.kind == BIND) {
        if (dir > 0) StartCapture(index);
        else Unbind(it);
        return;
    } else {
        return;
    }
    s_menu.dirty = true;
}

void Save() {
    for (const Item& it : kItems) {
        if (it.kind == HEADER || it.kind == LINK || it.kind == CAR || it.kind == ACTION || it.kind == PAGE || it.kind == RESET) continue;
        char buf[32];
        std::string text;
        if (it.kind == TOGGLE) snprintf(buf, sizeof(buf), "%d", *(bool*)it.value ? 1 : 0), text = buf;
        else if (it.kind == NUMBER) snprintf(buf, sizeof(buf), "%g", *(float*)it.value), text = buf;
        else if (it.kind == SOUND) snprintf(buf, sizeof(buf), "0x%08X", *(uint32_t*)it.value), text = buf;
        else text = *(std::string*)it.value;
        int ext = ExtIndex(it.value);
        if (ext >= 0) {
            if (ExtSetting(ext)) WritePrivateProfileStringA(it.section, it.key, text.c_str(), ExtIniPath(ext).c_str());
            continue;
        }
        std::string section = Physics_IsPerCar(it.value) ? Physics_Section() : it.section;
        WritePrivateProfileStringA(section.c_str(), it.key, text.c_str(), IniPath());
    }
    if (s_menu.clutchCal) {
        WritePrivateProfileStringA("Clutch", "Released", std::to_string(g_cfg.clutchReleased).c_str(), IniPath());
        WritePrivateProfileStringA("Clutch", "Pressed", std::to_string(g_cfg.clutchPressed).c_str(), IniPath());
        s_menu.clutchCal = false;
    }
    if (s_menu.pedalCal) {
        WritePrivateProfileStringA("Controls", "AccelerateRest", std::to_string(g_cfg.throttleRest).c_str(), IniPath());
        WritePrivateProfileStringA("Controls", "BrakeRest", std::to_string(g_cfg.brakeRest).c_str(), IniPath());
        s_menu.pedalCal = false;
    }
    Log("ui: settings saved to %s", IniPath());
}

void CancelCapture() {
    if (s_menu.capture < 0) return;
    Input_CancelCapture();
    s_menu.capture = -1;
}

// The VR mod's live flags for the Adjust dashboard page (and the element of the selected row) and
// the Triangle editor page; all off with the menu closed.
void UpdateVrPages() {
    const char* page = s_menu.open ? kItems[s_menu.page].label : "";
    float* adjust = ExtSetting(VR_DASHADJUST);
    float* highlight = ExtSetting(VR_DASHHIGHLIGHT);
    float* editor = ExtSetting(VR_EDITORON);
    if (adjust) *adjust = strcmp(page, "ADJUST DASHBOARD") == 0 ? 1.0f : 0.0f;
    if (editor) *editor = strcmp(page, "TRIANGLE EDITOR") == 0 || s_picking ? 1.0f : 0.0f;
    for (int k = 0; k < 2 && s_menu.open; k++)
        if (float* v = ExtSetting(VR_WHEELPIVOT + k)) s_ext[VR_WHEELPIVOT + k] = *v;
    if (highlight) {
        *highlight = -1;
        const void* v = s_menu.open && s_menu.sel >= 0 && s_menu.sel < kNumItems ? kItems[s_menu.sel].value : nullptr;
        for (int k = 0; k < 15 && v; k++)
            if (v == &s_ext[VR_DASHEL + k]) *highlight = (float)(k / 3);
        for (int k = 0; k < 3 && v; k++)
            if (v == &s_ext[VR_DASHMSG + k]) *highlight = 5;
        for (int k = 0; k < 24 && v; k++)
            if (v == &s_ext[VR_DASHCROP + k]) *highlight = (float)(k / 4);
        const int boxElement[4] = {0, 1, 4, 5};
        for (int k = 0; k < 4 && v; k++)
            if (v == &s_ext[VR_DASHELBOX + k]) *highlight = (float)boxElement[k];
    }
}

void SetOpen(bool open) {
    if (open == s_menu.open) return;
    s_menu.open = open;
    Log("ui: menu %s", open ? "opened" : "closed");
    if (open)
        for (int i = 0; i < EXT_COUNT; i++)
            if (ExtSetting(i)) s_ext[i] = *ExtSetting(i);
    if (!open) {
        CancelCapture();
        UpdateVrPages();
        if (s_menu.dirty) {
            Save();
            s_menu.dirty = false;
        }
    }
    Game_SetMenuOpen(open);
    HMODULE ffb = ExtModule(MOD_FFB);
    FARPROC menuOpen = ffb ? GetProcAddress(ffb, "NfsmwFfb_MenuOpen") : nullptr;
    if (menuOpen) ((void(__cdecl*)(int))menuOpen)(open ? 1 : 0);
}

void Step(int dir) {
    std::vector<int> rows = PageRows();
    int n = (int)rows.size();
    int pos = (int)(std::find(rows.begin(), rows.end(), s_menu.sel) - rows.begin());
    if (pos >= n) pos = 0;
    do pos = (pos + dir + n) % n;
    while (!Selectable(rows[pos]));
    s_menu.sel = rows[pos];
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
    if (!driving) {
        CancelCapture();
        return;
    }
    if (s_menu.capture >= 0) {
        // Waiting for a press: the menu key cancels, everything else is the new binding.
        std::string binding;
        int rest;
        if (Input_Captured(&binding, &rest)) {
            FinishCapture(binding, rest);
        } else if (toggles & 1) {
            CancelCapture();
            Note("Cancelled");
        } else if (Now() - s_menu.captureStart > 8.0) {
            CancelCapture();
            Note("Nothing pressed, cancelled");
        }
        return;
    }
    if (s_picking) {
        UpdatePicking(toggles, nav);
        return;
    }
    if (toggles & 1) SetOpen(!s_menu.open);
    if (!s_menu.open) return;
    for (int n = 0; n < nav[0]; n++) Step(-1);
    for (int n = 0; n < nav[1]; n++) Step(+1);
    for (int n = 0; n < nav[2] && s_menu.capture < 0; n++) Change(s_menu.sel, -1);
    for (int n = 0; n < nav[3] && s_menu.capture < 0; n++) Change(s_menu.sel, +1);
}

std::string MenuKeyName() {
    std::vector<std::string> list = SplitBindings(g_cfg.menu);
    if (list.empty()) return "the menu key";
    std::string m = list[0];
    return m.compare(0, 4, "key:") == 0 ? m.substr(4) : m;
}

void DrawMenu(int W, int H, float k) {
    float fh = (float)s_font.height, line = roundf(fh * 1.45f), pad = roundf(12 * k), gap = roundf(24 * k);
    float labelW = 0, valueW = std::max(TextWidth("< Press a key, button or pedal... >"), TextWidth("< 100% heat >"));
    for (const Item& it : kItems) labelW = std::max(labelW, TextWidth(it.label));
    const Item& cur = kItems[s_menu.sel];
    std::string hint;
    if (s_menu.capture >= 0) hint = MenuKeyName() + ": cancel";
    else if (cur.flags & BF_STEER) hint = "Right: bind (turn the wheel)    Left: unbind    " + MenuKeyName() + ": close";
    else if (cur.kind == BIND)
        hint = std::string("Right: add binding    Left: ") + (cur.flags & BF_REQUIRED ? "reset" : "unbind") + "    " +
               MenuKeyName() + ": close";
    else if (cur.kind == ACTION) hint = "Right: do it    " + MenuKeyName() + ": close";
    else if (cur.kind == LINK && cur.section != kMainPage) hint = "Right: open    " + MenuKeyName() + ": close";
    else if (cur.kind == LINK) hint = "Right/Left: back    " + MenuKeyName() + ": close";
    else hint = "Up/Down: select    Left/Right: change    " + MenuKeyName() + ": close";
    bool noted = Now() - s_menu.noteTime < 3.0 && s_menu.capture < 0;
    std::string titleText = s_menu.page == 0 ? std::string(kMainPage) : std::string(kMainPage) + "  >  " + kItems[s_menu.page].label;
    if (strcmp(kItems[s_menu.page].label, "PHYSICS") == 0 && *Physics_Car()) {
        std::string car = Physics_Car();
        for (auto& c : car) c = (char)toupper((unsigned char)c);
        titleText += "  -  " + car;
    }
    const char* title = titleText.c_str();
    float w = std::max(labelW + gap + valueW, std::max(TextWidth(hint.c_str()), TextWidth(title))) + 2 * pad;
    if (noted) w = std::max(w, TextWidth(s_menu.note.c_str()) + 2 * pad);
    std::vector<int> page = PageRows();
    int count = (int)page.size();
    int rows = std::min(count, kMaxRows);
    int pos = (int)(std::find(page.begin(), page.end(), s_menu.sel) - page.begin());
    if (pos >= count) {
        for (pos = 0; pos < count - 1 && !Selectable(page[pos]); pos++) {}
        s_menu.sel = page[pos];
    }
    // Keep the selection in view, with its section header when it fits.
    int want = pos > 0 && kItems[page[pos - 1]].kind == HEADER ? pos - 1 : pos;
    if (want < s_menu.first) s_menu.first = want;
    if (pos >= s_menu.first + rows) s_menu.first = pos - rows + 1;
    s_menu.first = Clamp(s_menu.first, 0, count - rows);
    float h = line * (rows + 3) + 2 * pad;
    float x0 = roundf((W - w) / 2), y0 = roundf((H - h) / 2);
    Quad(x0, y0, x0 + w, y0 + h, D3DCOLOR_ARGB(215, 12, 12, 14));
    Quad(x0, y0, x0 + w, y0 + roundf(2 * k), D3DCOLOR_ARGB(255, 255, 150, 0));
    float y = y0 + pad;
    Text(x0 + pad, y, title, D3DCOLOR_ARGB(255, 255, 150, 0));
    y += line * 1.5f;
    float listTop = y;
    for (int r = s_menu.first; r < s_menu.first + rows; r++, y += line) {
        int i = page[r];
        const Item& it = kItems[i];
        float ty = y + roundf((line - fh) / 2);
        if (it.kind == HEADER) {
            Text(x0 + pad, ty, it.label, D3DCOLOR_ARGB(255, 140, 140, 150));
            continue;
        }
        bool sel = i == s_menu.sel;
        bool capturing = i == s_menu.capture;
        if (sel) Quad(x0 + pad / 2, y, x0 + w - pad / 2, y + line, D3DCOLOR_ARGB(200, 190, 105, 0));
        D3DCOLOR c = sel ? D3DCOLOR_ARGB(255, 255, 255, 255) : D3DCOLOR_ARGB(255, 210, 210, 210);
        Text(x0 + pad + roundf(10 * k), ty, it.label, c);
        std::string v = ValueText(it, i);
        if (sel && !capturing && it.kind != LINK && it.kind != ACTION) v = "< " + v + " >";
        D3DCOLOR vc = capturing && Blink(300) ? D3DCOLOR_ARGB(255, 255, 210, 120) : c;
        Text(x0 + w - pad - TextWidth(v.c_str()), ty, v.c_str(), vc);
    }
    if (rows < count) {
        // Scroll bar along the right edge of the list.
        float trackH = line * rows, barH = std::max(4.0f * k, trackH * rows / count);
        float barY = listTop + (trackH - barH) * s_menu.first / (count - rows);
        float bx = x0 + w - roundf(4 * k);
        Quad(bx, listTop, bx + roundf(2 * k), listTop + trackH, D3DCOLOR_ARGB(120, 90, 90, 95));
        Quad(bx, barY, bx + roundf(2 * k), barY + barH, D3DCOLOR_ARGB(255, 255, 150, 0));
    }
    y += line * 0.5f;
    if (noted) Text(x0 + pad, y, s_menu.note.c_str(), D3DCOLOR_ARGB(255, 255, 200, 120));
    else Text(x0 + pad, y, hint.c_str(), D3DCOLOR_ARGB(255, 150, 150, 160));
}

// --- ImGui menu ------------------------------------------------------------------------------
// The same items, pages and navigation as the flat menu (bindings drive s_menu.sel), drawn with
// Dear ImGui: a category sidebar, the page beside it, and a footer with the selected setting's
// description. On a flat screen the mouse works too.
//
// Rendering is ours, not imgui_impl_dx9: everything is drawn with DrawIndexedPrimitiveUP, and the
// font atlas is a MANAGED texture (a DYNAMIC one in DEFAULT on D3D9Ex, where MANAGED doesn't exist
// and nothing is lost), so a device reset never needs our cooperation. Alpha is blended for the
// VR mod's HUD panel the same way as the flat menu.

// One-line descriptions, shown under the menu for the selected setting.
const struct {
    const void* value;
    const char* text;
} kHelp[] = {
    {&g_cfg.steer, "The wheel axis that steers. Right, then turn the wheel."},
    {&g_cfg.throttle, "Accelerator pedal, button or key. Pedals: press it fully when binding."},
    {&g_cfg.brake, "Brake pedal, button or key. Pedals: press it fully when binding."},
    {&g_cfg.wheelRotation, "Set this to the wheel's rotation in its driver (G HUB, Fanatec, ...), lock to lock."},
    {&g_cfg.steeringLock, "How far you turn the wheel, lock to lock, for full steering in game. A soft lock (FFB) stops it there."},
    {&g_cfg.steerDeadzone, "Ignored wheel travel around the centre."},
    {&g_cfg.steerLinearity, "1 = linear. Higher: finer steering around the centre, quicker towards full lock."},
    {&g_cfg.steerSensitivity, "How much the car steers for a given wheel angle. Higher: quicker steering (full lock sooner)."},
    {&g_cfg.steerSpeedSensitive, "Off: the front wheels follow your wheel directly, full lock (45 deg) at the steering lock, at any speed. On: less lock the faster you go, smoothly, still in proportion to your wheel; in a slide the lock widens with it so you can catch it."},
    {&g_cfg.steerSpeedSensitivity, "With speed-sensitive steering: how much lock is taken away at speed. 85% is close to the game's own (19 deg of front wheel lock at 64 km/h, 12 at 130, 9 at 250). Lower: quicker steering at speed."},
    {&g_cfg.pedalDeadzone, "Ignored pedal travel at each end."},
    {&g_cfg.gameController, "Off: the game ignores controllers (also in its menus); bind them here instead."},
    {&g_cfg.menuController, "Off: disable all controller input in the game's menus. Driving and this F4 menu keep their bindings. Needs the updated force feedback mod."},
    {&g_cfg.menuAxes, "Off: pedals and steering cannot scroll the game's menus; buttons and the d-pad still work. Driving is unaffected. Needs the updated force feedback mod."},
    {&s_ext[FFB_GAIN], "Overall force feedback strength."},
    {&s_ext[FFB_MAXFORCE], "Ceiling of the force sent to the wheel. Peaks are compressed below it, never cut flat. Lower it on a strong direct drive base."},
    {&s_ext[FFB_SMOOTHING], "Smooths the game's physics steps (about 60 a second). Lower: smoother but slower; higher: sharper."},
    {&s_ext[FFB_SLEW], "How fast the force may change, full scale per second. Lower is gentler on the hands."},
    {&s_ext[FFB_ALIGNING], "Weight from the front tyres: heavy while they grip, light past the limit, pulling into the countersteer in a slide."},
    {&s_ext[FFB_CENTERING], "Spring back to centre, growing with speed."},
    {&s_ext[FFB_CENTREWEIGHT], "Firm hold around straight ahead once moving: a few degrees off centre already pull back, so you hold the wheel against it instead of it staying wherever you leave it."},
    {&s_ext[FFB_ENGINE], "Engine vibration, at the engine's revs. Mostly felt standing still."},
    {&s_ext[FFB_KERBS], "A kick toward the side of a front wheel that hits a kerb, bump or pothole, and a short shake."},
    {&s_ext[FFB_SURFACE], "The game's own rumble for the surface under the front tyres: grass, gravel, dirt, rumble strips."},
    {&s_ext[FFB_CRASHES], "A jolt when you hit something: cars, walls, and props such as benches, sign posts and fences."},
    {&s_ext[FFB_SCRAPES], "Grinding while the car slides along a wall, and a pull toward it."},
    {&s_ext[FFB_BREAKABLES], "A sharp knock when you smash a breakable object (bench, sign post, fence, bin): the game's own breakable props, recognised as such, not any light contact."},
    {&s_ext[FFB_DAMPING], "Weight against fast wheel movement. Keeps a direct drive wheel calm."},
    {&s_ext[FFB_FRICTION], "Constant resistance, like a real steering rack."},
    {&s_ext[FFB_SOFTLOCK], "Push-back when the wheel goes past the steering lock."},
    {&s_ext[FFB_INVERT], "On if every force pushes the wrong way (the wheel runs away from centre, or Test pushes left)."},
    {&s_ext[FFB_DEVICE], "The force feedback device that drives your wheel, in Windows' order (as joy.cpl lists them). Use Test to check."},
    {&s_ext[VR_HUDWIDTH], "Size of the HUD panel in the headset (this menu is on it too)."},
    {&s_ext[VR_HUDDISTANCE], "How far in front of you the HUD panel floats."},
    {&s_ext[VR_HUDHEIGHT], "Moves the HUD panel up or down."},
    {&s_ext[VR_HUDRIGHT], "Moves the HUD panel to the left (-) or right (+)."},
    {&s_ext[VR_HIDEARMS], "Leaves out the driver model's arms and hands in the cockpit: they don't follow your hands or the wheel."},
    {&s_ext[VR_WHEELOVERHUD], "On: the car's steering wheel is drawn in front of the HUD while driving. In the pause menu, other menus and this menu the HUD is on top again."},
    {&s_ext[FFB_OFFROAD], "The wheel vibrates while the front wheels are off the road: grass, dirt, mud, gravel, sand. Stronger with both wheels off and with speed."},
    {&s_ext[FFB_CARHITS], "A hard kick and a short shake when you hit another vehicle (traffic, cops, racers), toward the side you were pushed, full strength from 150 km/h. Only vehicles: the game tells them apart by their own object type."},
    {&s_ext[FFB_LANDING], "A hard kick and a short shake in the wheel when the car lands after a jump, stronger the longer it was in the air."},
    {&s_ext[FFB_NOS], "How much lighter the steering gets while the nitrous is on (0 = no change)."},
    {&s_ext[VR_HUDPERCAR], "On: each car remembers its own HUD size, distance, height and left / right position, and its dashboard zoom and position, like the seat. A car you haven't set yet starts with the last ones you used."},
    {&s_ext[VR_DASH], "On: from the driver's seat the HUD's minimap, speedometer, cop radar, pursuit info and leaderboard move onto the car's gauge cluster (place each in Adjust dashboard; black out the car's own gauges with the Triangle editor). The floating HUD is hidden meanwhile, except messages (busted, totaled, locations), which stay in front of your eyes. Back to normal in the pause menu and this menu."},
    {&s_ext[VR_DASHZOOM], "Size of the whole in-car dashboard: the display and everything on it. Each car remembers its own."},
    {&s_ext[VR_DASHRIGHT], "Moves the in-car dashboard panel to the left (-) or right (+) of the gauge cluster."},
    {&s_ext[VR_DASHUP], "Moves the in-car dashboard panel down (-) or up (+)."},
    {&s_ext[VR_MIRROROFFSET], "Moves the VR rear view mirror's picture toward you (+) or back into the mirror (-), for cars where it floats in front of the mirror. Each car remembers it."},
    {&s_ext[VR_DASHBOX], "The in-car display: a white screen with rounded corners and an orange outline the HUD elements sit on (0 = none)."},
    {&s_ext[VR_DASHGLASS], "A glass sheen over the in-car display and everything on it (0 = none)."},
    {&s_ext[VR_WHEELPIVOT], "Moves the point the steering wheel turns about to the right (+) or left (-). Each car remembers it."},
    {&s_ext[VR_WHEELPIVOT + 1], "Moves the point the steering wheel turns about up (+) or down (-). Each car remembers it."},
    {&s_ext[VR_DASHSCREEN], "Width of the in-car display (scaled by Dashboard zoom). Each car remembers it with HUD positioning."},
    {&s_ext[VR_DASHSCREEN + 1], "Height of the in-car display (scaled by Dashboard zoom). Each car remembers it with HUD positioning."},
    {&s_ext[VR_DASHDEPTH], "Moves the in-car dashboard's HUD toward you (+) or into the dashboard (-), from 3 cm off the gauges. Each car remembers it with HUD positioning."},
    {&s_ext[VR_EDITBRUSH], "Triangles within this distance of the point you look at are picked too (0 = only the one looked at)."},
    {&s_ext[VR_EDITMODE], "How much one pick selects: one triangle, all within 2 / 5 / 10 cm, the connected piece (a gauge, a wheel spoke) or the whole draw entry. Also Left / Right while picking."},
    {&s_ext[VR_MIRRORBRIGHTNESS], "Brightness of the rear view in the side mirrors and the rear view mirror."},
    {&s_ext[VR_MIRRORCONTRAST], "Contrast of the rear view in the side mirrors and the rear view mirror."},
    {&s_ext[VR_NEARZ], "Closest distance drawn. Lower it if the dashboard or wheel near your eyes is cut off; very low values can make distant surfaces flicker."},
    {&g_cfg.recenterVr, "Recentres the VR view, like the VR mod's F8. Works while driving and in menus."},
    {&g_cfg.seat[0], "Moves the VR seat (cockpit camera) forward while held. Saved per car."},
    {&g_cfg.seat[1], "Moves the VR seat back while held. Saved per car."},
    {&g_cfg.seat[2], "Moves the VR seat left while held. Saved per car."},
    {&g_cfg.seat[3], "Moves the VR seat right while held. Saved per car."},
    {&g_cfg.seat[4], "Moves the VR seat up while held. Saved per car."},
    {&g_cfg.seat[5], "Moves the VR seat down while held. Saved per car."},
    {&g_cfg.seat[6], "Puts the VR seat back at the driver model's head."},
    {&g_cfg.misshiftChance, "Chance that a shift without the clutch (a misshift) damages the car. Needs car damage on."},
    {&g_cfg.misshiftDamage, "Car damage done by a damaging misshift."},
    {&g_cfg.sequential, "H-pattern shifter, or sequential shift up / shift down."},
    {&g_cfg.requireClutch, "A gear only goes in with the clutch pressed."},
    {&g_cfg.shiftPedal, "How far the clutch must be pressed to count for a shift."},
    {&g_cfg.revMatchRpm, "Allow a clutchless shift when the revs match the new gear this closely."},
    {&g_cfg.reselectAfterMiss, "After a missed shift, take the stick out of the gate and put it back in."},
    {&g_cfg.latching, "Gear stays selected after its button is released (keyboard, button boxes)."},
    {&g_cfg.grind, "A missed shift crunches."},
    {&g_cfg.grindSound, "The crunch sound. Changing it plays it."},
    {&g_cfg.strength, "Clutch friction compared with the game's own. Lower slips longer."},
    {&g_cfg.biteStart, "Pedal travel where the clutch starts to grip as you let it out."},
    {&g_cfg.biteEnd, "Pedal travel where the clutch grips fully."},
    {&g_cfg.engineWear, "Over-revving heats the engine; it loses power and blows at full heat."},
    {&g_cfg.revLimiter, "Keep the rev limiter. Sitting on it still heats the engine."},
    {&g_cfg.heatUp, "Heat per second at the redline. Full heat blows the engine."},
    {&g_cfg.heatUpMax, "Heat per second at max rpm."},
    {&g_cfg.coolDown, "Heat lost per second below the redline."},
    {&g_cfg.powerLossStart, "Heat where the engine starts losing power."},
    {&g_cfg.powerLoss, "Torque lost at full heat."},
    {&g_cfg.heatSmokeFrom, "The engine smokes from this much heat."},
    {&g_cfg.damage, "Crashes damage the car; at 100% it is totaled."},
    {&g_cfg.damageMinImpact, "Impacts below this speed change do no damage."},
    {&g_cfg.damageStrength, "Damage per hit."},
    {&g_cfg.damageLossStart, "Damage where the engine starts losing power."},
    {&g_cfg.damageLoss, "Torque lost at 100% damage."},
    {&g_cfg.damageSmoke, "A damaged engine smokes."},
    {&g_cfg.smokeFrom, "The engine smokes from this much damage."},
    {&g_cfg.smokeTrail, "How much of the car's speed the smoke keeps, so it trails behind."},
    {&g_cfg.poppedTires, "Spike strips leave the car drivable, as in NFS Carbon."},
    {&g_cfg.tireHandicap, "Grip loss and drag of a popped tire."},
    {&g_cfg.physics, "Off: the game's own physics everywhere."},
    {&g_cfg.tractionControl, "Cuts power to a spinning wheel. 100% = the game's own."},
    {&g_cfg.stability, "Rear grip added while the car rotates, and the drift stabiliser."},
    {&g_cfg.driftAssist, "Rear grip taken away once a slide starts."},
    {&g_cfg.grip, "Tire grip multiplier."},
    {&g_cfg.torqueSplit, "Multiplies the share of torque sent to the front wheels."},
    {&g_cfg.rearGrip, "Rear tyre grip compared with the front. Below 100% the rear lets go first: the car oversteers: with the throttle in rear and all-wheel drive cars, lifting off or braking into a corner in front drive cars. Above 100% it understeers."},
    {&g_cfg.slideGrip, "Grip of a tyre that has broken loose, compared with its peak. Below 100% a tyre that lets go slides on with less grip, so the car slides and you catch it, instead of snapping into the turn. Use it with Rear grip for oversteer."},
    {&g_cfg.slideOversteer, "Side grip of the rear tyres under full throttle, the way grass takes it: they keep driving but hold less sideways, so the rear slides out under power. It follows the throttle (lifting off in front drive cars) and the grip comes back as the slide gets past 20-45 degrees, so lifting or countersteering catches it. 100% = off."},
    {&g_cfg.rainGrip, "Tyre grip in the rain (90% = 10% less), from a light rain on."},
    {&g_cfg.speedGripLoss, "Cornering grip lost at speed: this much of the tyres' side grip is gone at 300 km/h, a quarter of it at 150 km/h, nothing from a standstill (it grows with the square of the speed). Launching and braking keep their grip. Works per car with \"Settings for this car only\"."},
    {&g_cfg.offroadGrip, "Tyre grip on grass, dirt, mud, gravel and sand (each tyre by the surface under it). 100% = as on the road."},
    {&g_cfg.drivetrain, "Exaggerates how the driven wheels lose grip under power: 2x doubles a rear drive car's power oversteer, a front drive car's understeer, and all-wheel drive by its torque split. Below 1 softens it."},
    {&g_cfg.gravity, "Gravity in g. Grip scales with it."},
    {&g_cfg.gravityAll, "Gravity applies to AI and traffic too."},
    {&g_cfg.maxSpin, "The fastest your car may rotate (8 rad/s is over one turn a second). The game allows 30, and a car spinning that fast gets caught in a collision loop and is thrown around the map. 0 = the game's."},
    {&g_cfg.noSuvCops, "No SUV police: no heavy support, other cars instead of SUV patrols."},
    {&g_cfg.normalDragRaces, "Drag races without the drag mechanics for your car: you steer (no automatic lane changes), the normal engine, shifting and damage, so the wheel, shifter, engine wear and damage mods work. Opponents and the race itself are unchanged. Applies from the next race."},
    {&g_cfg.hudMeter, "The speedbreaker meter shows car damage, with engine heat in red."},
    {&g_cfg.speedbreaker, "Keep the game's speedbreaker."},
    {&g_cfg.clutch, "Clutch pedal, button or key."},
    {&g_cfg.toggle, "Switches between H-pattern and sequential while driving."},
    {&g_cfg.menu, "Opens and closes this menu."},
    {&g_cfg.classicMenu, "The old flat menu (no textures), in case this one gives trouble."},
};

const char* HelpText(const Item& it) {
    if (it.kind == CAR) return "This car keeps its own physics values, saved in its [Car.<name>] section.";
    if (it.kind == ACTION && strcmp(it.key, "RecenterVr") == 0)
        return "Makes the way you are facing now straight ahead, at the driver's seat (same as the VR mod's key, F8).";
    if (it.kind == ACTION && strcmp(it.key, "FfbStatus") == 0)
        return "Whether a wheel is hooked up for force feedback, and if not, what is holding it up. Changes are written to nfsmw_ffb.log too.";
    if (it.kind == ACTION && strcmp(it.key, "TestFfb") == 0)
        return "Pushes the wheel to the right for half a second. Nothing: pick another Wheel device. To the left: turn on Reverse force direction.";
    if (it.kind == ACTION && strcmp(it.key, "RestartFfb") == 0)
        return "Reacquires the wheel and rebuilds this mod's effects after a driver or focus issue. Also available with F10.";
    if (it.kind == ACTION && strcmp(it.key, "DropCar") == 0)
        return "Lifts the car straight up by Drop height and lets it fall, keeping its forward speed: for testing landings anywhere.";
    if (it.value == &g_cfg.dropHeight) return "How high Drop from air lifts the car.";
    if (it.kind == ACTION) return "Pops all four tires, as a spike strip does.";
    if (it.kind == RESET && (it.flags & BF_GAME)) return "Settings on this page as the unmodded game has them. Bindings are kept.";
    if (it.kind == RESET) return "Every setting on this page back to the mod's default. Bindings are kept.";
    if (it.kind == LINK) return it.section == kMainPage ? "Back to the categories." : "";
    for (auto& h : kHelp)
        if (h.value == it.value) return h.text;
    if (it.kind == BIND) return "Right: add a key, button or pedal. Left: unbind.";
    return "";
}

// "Default: ..." for settings with a default in Config (or the FFB mod's defaults).
std::string DefaultText(const Item& it) {
    float f;
    bool b;
    uint32_t u;
    if (!DefaultOf(it, &f, &b, &u)) return "";
    char buf[64];
    if (it.kind == TOGGLE) {
        if (it.value == &g_cfg.sequential) return b ? "Default: Sequential" : "Default: H-pattern";
        return b ? "Default: On" : "Default: Off";
    }
    if (it.kind == SOUND) return "Default: " + SoundName(u);
    if (it.value == &g_cfg.revMatchRpm && f <= 0.0f) return "Default: Off";
    snprintf(buf, sizeof(buf), it.fmt, f * it.show);
    return std::string("Default: ") + buf;
}

// "Game: ..." for settings with a game default.
std::string GameDefaultText(const Item& it) {
    float f;
    if (!GameDefaultOf(it, &f)) return "";
    if (it.kind == TOGGLE) return f != 0.0f ? "Game: On" : "Game: Off";
    char buf[64];
    snprintf(buf, sizeof(buf), it.fmt, f * it.show);
    return std::string("Game: ") + buf;
}


struct ImState {
    bool init = false;
    int fontPx = 0;
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    IDirect3DTexture9* fontTex = nullptr;
    IDirect3DDevice9* dev = nullptr;    // the device fontTex belongs to
    double last = 0;
    int lastSel = -1, lastPage = -1;
    POINT mouseAt = {};
    bool mouseUsed = false;             // the mouse moved while the menu was open: show its cursor
    bool wasOpen = false;
    HWND hookedWnd = nullptr;           // game window whose WndProc we chained (mouse wheel)
    WNDPROC origProc = nullptr;
    volatile LONG wheel = 0;            // WM_MOUSEWHEEL delta since the last frame
} s_im;

// The game reads the mouse through DirectInput, so the wheel only shows up as window messages:
// count them while the menu is open, and pass every message on.
LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEWHEEL && s_menu.open) InterlockedExchangeAdd(&s_im.wheel, GET_WHEEL_DELTA_WPARAM(wp));
    return CallWindowProcA(s_im.origProc, hwnd, msg, wp, lp);
}

const ImU32 kAccent = IM_COL32(255, 150, 0, 255);

// Font atlas texture: MANAGED survives resets; D3D9Ex has no MANAGED pool but loses nothing.
bool CreateFontTexture(IDirect3DDevice9* dev) {
    if (s_im.fontTex) s_im.fontTex->Release();
    s_im.fontTex = nullptr;
    unsigned char* alpha;
    int w, h;
    ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&alpha, &w, &h);
    IDirect3DTexture9* tex = nullptr;
    DWORD lockFlags = 0;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr))) {
            Log("ui: font texture %dx%d could not be created", w, h);
            return false;
        }
        lockFlags = D3DLOCK_DISCARD;
    }
    Log("ui: font texture %dx%d %s", w, h, lockFlags ? "DEFAULT (D3D9Ex)" : "MANAGED");
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, lockFlags))) {
        tex->Release();
        return false;
    }
    for (int y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) row[x] = ((uint32_t)alpha[y * w + x] << 24) | 0x00FFFFFF;
    }
    tex->UnlockRect(0);
    s_im.fontTex = tex;
    s_im.dev = dev;
    ImGui::GetIO().Fonts->SetTexID((ImTextureID)tex);
    return true;
}

void BuildImFonts(IDirect3DDevice9* dev, int px) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::string dir;
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    dir = std::string(win) + "\\Fonts\\";
    const char* faces[] = {"bahnschrift.ttf", "segoeui.ttf", "arial.ttf"};
    std::string face;
    for (const char* f : faces)
        if (GetFileAttributesA((dir + f).c_str()) != INVALID_FILE_ATTRIBUTES) {
            face = dir + f;
            break;
        }
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    auto add = [&](float size) {
        ImFont* f = face.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(face.c_str(), size, &cfg, io.Fonts->GetGlyphRangesDefault());
        if (!f) {
            ImFontConfig d;
            d.SizePixels = size;
            f = io.Fonts->AddFontDefault(&d);
        }
        return f;
    };
    s_im.body = add((float)px);
    s_im.small = add(roundf(px * 0.8f));
    s_im.title = add(roundf(px * 1.45f));
    io.Fonts->Build();
    CreateFontTexture(dev);
    s_im.fontPx = px;
    Log("ui: ImGui font %s, %d px", face.empty() ? "(built-in)" : face.c_str(), px);
}

void StyleImGui(float k) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6;
    st.ChildRounding = 4;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.PopupRounding = 4;
    st.ScrollbarRounding = 4;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 4);
    st.WindowPadding = ImVec2(16, 14);
    st.ScrollbarSize = 10;
    st.GrabMinSize = 10;
    st.ScaleAllSizes(k);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.06f, 0.90f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.09f, 0.97f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.52f, 0.56f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.29f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.59f, 0.0f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.70f, 0.25f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.29f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.59f, 0.0f, 0.8f);
    c[ImGuiCol_Header] = ImVec4(1.0f, 0.59f, 0.0f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.59f, 0.0f, 0.18f);
    c[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.59f, 0.0f, 0.45f);
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.28f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.33f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 0.59f, 0.0f, 0.7f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 0.59f, 0.0f, 1.0f);
}

void RenderImGui(IDirect3DDevice9* dev, ImDrawData* dd, int W, int H) {
    if (!dd || dd->TotalVtxCount == 0) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    D3DVIEWPORT9 vp = {0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // Keep the HUD layer's alpha meaningful for the VR panel: alpha = src + dst * (1 - src).
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    struct V {
        float x, y, z, rhw;
        D3DCOLOR c;     // ImGui colours are packed BGRA (IMGUI_USE_BGRA_PACKED_COLOR)
        float u, v;
    };
    static std::vector<V> verts;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        verts.resize(cl->VtxBuffer.Size);
        for (int i = 0; i < cl->VtxBuffer.Size; i++) {
            const ImDrawVert& s = cl->VtxBuffer[i];
            verts[i] = {s.pos.x - 0.5f, s.pos.y - 0.5f, 0.0f, 1.0f, s.col, s.uv.x, s.uv.y};
        }
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            RECT r = {(LONG)cmd.ClipRect.x, (LONG)cmd.ClipRect.y, (LONG)cmd.ClipRect.z, (LONG)cmd.ClipRect.w};
            if (r.right <= r.left || r.bottom <= r.top) continue;
            dev->SetScissorRect(&r);
            dev->SetTexture(0, (IDirect3DTexture9*)cmd.GetTexID());
            dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(), cmd.ElemCount / 3,
                                        cl->IdxBuffer.Data + cmd.IdxOffset, D3DFMT_INDEX16,
                                        verts.data() + cmd.VtxOffset, sizeof(V));
        }
    }
    sb->Apply();
    sb->Release();
}

// Mouse, on a flat screen: the cursor over the game window, scaled to the render target.
void FeedMouse(IDirect3DDevice9* dev, int W, int H) {
    ImGuiIO& io = ImGui::GetIO();
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    dev->GetCreationParameters(&cp);
    HWND hwnd = cp.hFocusWindow;
    POINT pt;
    RECT rc;
    bool focus = hwnd && GetForegroundWindow() == hwnd;
    if (!focus || !GetCursorPos(&pt) || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        return;
    }
    if (hwnd != s_im.hookedWnd && IsWindow(hwnd)) {
        s_im.origProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)&MenuWndProc);
        s_im.hookedWnd = hwnd;
    }
    LONG wheel = InterlockedExchange(&s_im.wheel, 0);
    if (wheel) io.AddMouseWheelEvent(0.0f, wheel / (float)WHEEL_DELTA);
    if (s_im.wasOpen && (pt.x != s_im.mouseAt.x || pt.y != s_im.mouseAt.y)) s_im.mouseUsed = true;
    s_im.mouseAt = pt;
    ScreenToClient(hwnd, &pt);
    io.AddMousePosEvent(pt.x * (float)W / rc.right, pt.y * (float)H / rc.bottom);
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.MouseDrawCursor = s_im.mouseUsed;
}

// An on/off switch. Returns true when clicked.
bool Switch(const char* id, bool on, float k) {
    float h = ImGui::GetFrameHeight() * 0.8f, w = h * 1.9f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetFrameHeight() - h) / 2;
    ImGui::SetCursorScreenPos(p);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? kAccent : IM_COL32(70, 70, 76, 255), h / 2);
    float r = h / 2 - 2.5f * k;
    ImVec2 c(on ? p.x + w - h / 2 : p.x + h / 2, p.y + h / 2);
    dl->AddCircleFilled(c, r, IM_COL32(245, 245, 245, 255));
    return clicked;
}

// The value side of a setting row, `w` wide. Changes made with the mouse go through the same
// code as the bindings' Left/Right.
void ValueWidget(int i, float w, float k) {
    const Item& it = kItems[i];
    ImGui::PushID(i);
    float x0 = ImGui::GetCursorPosX();
    std::string v = ValueText(it, i);
    switch (it.kind) {
        case TOGGLE:
        case CAR: {
            bool on = it.kind == TOGGLE ? *(bool*)it.value : (*Physics_Car() && Physics_OwnSettings());
            if (Switch("sw", on, k)) {
                s_menu.sel = i;
                Change(i, +1);
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", v.c_str());
            break;
        }
        case NUMBER: {
            int ext = ExtIndex(it.value);
            if (ext >= 0 && !ExtSetting(ext)) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", kExtMods[kExt[ext].mod].missing);
                break;
            }
            float& val = *(float*)it.value;
            float shown = val * it.show;
            ImGui::SetNextItemWidth(w);
            std::string fmt = v == "Off" ? std::string("Off") : std::string(it.fmt);
            if (it.value == &s_ext[FFB_INVERT] || it.value == &s_ext[FFB_DEVICE] || it.value == &s_ext[VR_HIDEARMS] ||
                it.value == &s_ext[VR_WHEELOVERHUD] || it.value == &s_ext[VR_HUDPERCAR] || it.value == &s_ext[VR_DASH] ||
                (it.value >= (void*)&s_ext[VR_DASHELBOX] && it.value < (void*)&s_ext[VR_DASHELBOX + 4])) {
                fmt.clear();   // the text from ValueText, as a format without conversions
                for (char ch : v) fmt += ch == '%' ? std::string("%%") : std::string(1, ch);
            }
            if (ImGui::SliderFloat("##v", &shown, it.lo * it.show, it.hi * it.show, fmt.c_str(), ImGuiSliderFlags_NoInput)) {
                val = Clamp(roundf(shown / it.show / it.step) * it.step, it.lo, it.hi);
                s_menu.sel = i;
                AfterNumberChange(it);
            }
            break;
        }
        case BIND: {
            bool capturing = i == s_menu.capture;
            float clearW = ImGui::GetFrameHeight();
            if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, Blink(300) ? ImVec4(1.0f, 0.59f, 0.0f, 0.55f) : ImVec4(0.3f, 0.2f, 0.05f, 1.0f));
            if (ImGui::Button(v.c_str(), ImVec2(w - clearW - ImGui::GetStyle().ItemSpacing.x, 0)) && !capturing && s_menu.capture < 0) {
                s_menu.sel = i;
                Change(i, +1);
            }
            if (capturing) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(clearW, 0)) && s_menu.capture < 0) {
                s_menu.sel = i;
                Change(i, -1);
            }
            break;
        }
        case SOUND: {
            uint32_t cur = *(uint32_t*)it.value;
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginCombo("##s", v.c_str())) {
                for (auto& snd : kGrindSounds)
                    if (ImGui::Selectable(snd.name, snd.moment == cur)) {
                        *(uint32_t*)it.value = snd.moment;
                        Game_PlayMoment(snd.moment);
                        s_menu.sel = i;
                        s_menu.dirty = true;
                    }
                ImGui::EndCombo();
            }
            break;
        }
        case RESET: {
            bool armed = s_menu.resetArmed == i && Now() - s_menu.resetTime < 3.0;
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.1f, 1.0f));
            if (ImGui::Button(armed ? "Click again to reset" : "Reset", ImVec2(w * (armed ? 0.75f : 0.4f), 0))) {
                s_menu.sel = i;
                Change(i, +1);
            }
            if (armed) ImGui::PopStyleColor();
            break;
        }
        case ACTION:
            if (strcmp(it.key, "FfbStatus") == 0) {
                // The FFB mod's status: green while the force runs, orange while something holds it up.
                std::string status = FfbStatus();
                bool running = status.compare(0, 7, "Running") == 0;
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, running ? ImVec4(0.45f, 0.9f, 0.45f, 1.0f) : ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
                ImGui::TextUnformatted(status.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                break;
            }
            if (ImGui::Button("Go", ImVec2(w * 0.4f, 0))) {
                s_menu.sel = i;
                Change(i, +1);
            }
            break;
        case LINK:
            // A link on a page (to a sub-page, or back): opened like a category.
            if (ImGui::Button(it.section == kMainPage || strncmp(it.label, "<", 1) == 0 ? "< Back" : "Open  >",
                              ImVec2(w * 0.4f, 0))) {
                s_menu.sel = i;
                Change(i, +1);
            }
            break;
        default:
            break;
    }
    (void)x0;
    ImGui::PopID();
}

// The settings of one page (`page` = its PAGE item). `focused`: the page has the selection.
void DrawPage(int page, bool focused, float k) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rowH = roundf(ImGui::GetFrameHeight() + 8 * k);
    float fullW = ImGui::GetContentRegionAvail().x;
    float valueW = roundf(fullW * 0.46f);
    for (int i = page + 1; i < kNumItems && kItems[i].kind != PAGE; i++) {
        const Item& it = kItems[i];
        ImGui::PushID(i);
        if (it.kind == HEADER) {
            ImGui::Dummy(ImVec2(0, 6 * k));
            ImGui::PushFont(s_im.small);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 150, 0, 230));
            ImGui::TextUnformatted(it.label);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImVec2 a = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(a.x, a.y - 1), ImVec2(a.x + fullW, a.y - 1), IM_COL32(255, 150, 0, 70), 1.0f);
            ImGui::Dummy(ImVec2(0, 2 * k));
            ImGui::PopID();
            continue;
        }
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool sel = focused && i == s_menu.sel;
        // Clicking anywhere on the row selects it; the value widget sits on top.
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("row", ImVec2(fullW, rowH))) s_menu.sel = i;
        bool hovered = ImGui::IsItemHovered();
        if (sel) {
            dl->AddRectFilled(p0, ImVec2(p0.x + fullW, p0.y + rowH), IM_COL32(255, 150, 0, 55), 4 * k);
            dl->AddRectFilled(p0, ImVec2(p0.x + 3 * k, p0.y + rowH), kAccent, 2 * k);
            if (s_im.lastSel != s_menu.sel) ImGui::SetScrollHereY(0.5f);
        } else if (hovered) {
            dl->AddRectFilled(p0, ImVec2(p0.x + fullW, p0.y + rowH), IM_COL32(255, 255, 255, 12), 4 * k);
        }
        float ty = p0.y + (rowH - ImGui::GetTextLineHeight()) / 2;
        // The label keeps clear of the reset icon and the value.
        float labelEnd = p0.x + fullW - valueW - 18 * k - ImGui::GetFrameHeight();
        dl->PushClipRect(p0, ImVec2(labelEnd, p0.y + rowH), true);
        dl->AddText(ImVec2(p0.x + 12 * k, ty), sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 215, 215, 255), it.label);
        dl->PopClipRect();
        bool modDiff = Changed(it), gameDiff = Differs(it, true);
        if (modDiff || gameDiff) {
            // Reset to default: a circular arrow just left of the value.
            float s = ImGui::GetFrameHeight();
            ImVec2 b0(p0.x + fullW - valueW - 12 * k - s, p0.y + (rowH - s) / 2);
            ImGui::SetCursorScreenPos(b0);
            int click = ImGui::InvisibleButton("reset", ImVec2(s, s), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight)
                            ? (ImGui::IsMouseReleased(ImGuiMouseButton_Right) ? 2 : 1) : 0;
            if (click) {
                s_menu.sel = i;
                bool game = click == 2;
                if (ResetItem(it, game)) Note(std::string(it.label) + (game ? ": the game's default" : ": back to the mod's default"));
            }
            bool hot = ImGui::IsItemHovered();
            if (hot) {
                std::string tip = "Left click: mod default (" + DefaultText(it).substr(9) + ")";
                std::string g = GameDefaultText(it);
                if (!g.empty()) tip += "\nRight click: game default (" + g.substr(6) + ")";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            // Dimmer when only the game default differs.
            ImU32 col = hot ? kAccent : modDiff ? IM_COL32(170, 170, 180, 255) : IM_COL32(95, 95, 105, 255);
            ImVec2 c(b0.x + s / 2, b0.y + s / 2);
            float r = s * 0.28f, th = std::max(1.5f, 1.6f * k);
            dl->PathArcTo(c, r, 3.14159f * 0.15f, 3.14159f * 1.75f, 16);
            dl->PathStroke(col, 0, th);
            ImVec2 tip(c.x + r * cosf(3.14159f * 0.15f), c.y + r * sinf(3.14159f * 0.15f));
            float a = s * 0.16f;
            dl->AddTriangleFilled(ImVec2(tip.x - a, tip.y - a * 0.2f), ImVec2(tip.x + a, tip.y - a * 0.2f), ImVec2(tip.x, tip.y + a), col);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + fullW - valueW - 6 * k, p0.y + (rowH - ImGui::GetFrameHeight()) / 2));
        ValueWidget(i, valueW, k);
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
}

void DrawMenuImGui(IDirect3DDevice9* dev, int W, int H, float k) {
    if (!s_im.init) {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        s_im.init = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    int px = std::max(10, (int)roundf(19 * k));
    if (px != s_im.fontPx || dev != s_im.dev || !s_im.fontTex) {
        BuildImFonts(dev, px);
        StyleImGui(k);
    }
    if (!s_im.fontTex) return;
    double now = Now();
    io.DeltaTime = (float)Clamp(now - s_im.last, 0.001, 0.1);
    s_im.last = now;
    io.DisplaySize = ImVec2((float)W, (float)H);
    if (!s_im.wasOpen) s_im.mouseUsed = false;
    FeedMouse(dev, W, H);
    s_im.wasOpen = true;

    ImGui::NewFrame();
    float winW = std::min(W - 40.0f * k, roundf(900 * k)), winH = std::min(H - 24.0f * k, roundf(H * 0.86f));
    ImGui::SetNextWindowPos(ImVec2(roundf((W - winW) / 2), roundf((H - winH) / 2)));
    ImGui::SetNextWindowSize(ImVec2(winW, winH));
    ImGui::Begin("##tuning", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRectFilled(wp, ImVec2(wp.x + winW, wp.y + 3 * k), kAccent, 6 * k, ImDrawFlags_RoundCornersTop);

    // Title: TUNING > PAGE (- CAR)
    bool onMain = s_menu.page == 0;
    int shownPage = s_menu.page;
    if (onMain && kItems[s_menu.sel].kind == LINK)
        for (int i = 0; i < kNumItems; i++)
            if (kItems[i].kind == PAGE && strcmp(kItems[i].label, kItems[s_menu.sel].section) == 0) shownPage = i;
    ImGui::PushFont(s_im.title);
    ImGui::TextColored(ImVec4(1.0f, 0.59f, 0.0f, 1.0f), "TUNING");
    ImGui::PopFont();
    ImGui::SameLine();
    {
        std::string crumb = shownPage ? std::string(kItems[shownPage].label) : std::string();
        if (shownPage && strcmp(kItems[shownPage].label, "PHYSICS") == 0 && *Physics_Car()) {
            std::string car = Physics_Car();
            for (auto& c : car) c = (char)toupper((unsigned char)c);
            crumb += "  -  " + car;
        }
        float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (ImGui::GetTextLineHeightWithSpacing() * 1.45f - ImGui::GetTextLineHeightWithSpacing()) * 0.6f);
        ImGui::TextDisabled("%s", crumb.c_str());
    }
    ImGui::Dummy(ImVec2(0, 4 * k));

    float footerH = roundf(ImGui::GetTextLineHeightWithSpacing() * 2 + 14 * k);
    float bodyH = ImGui::GetContentRegionAvail().y - footerH;
    float sideW = roundf(winW * 0.30f);

    // Sidebar: the categories.
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    {
        ImDrawList* sd = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;
        for (int i = 1; i < kNumItems && kItems[i].kind != PAGE; i++) {
            const Item& it = kItems[i];
            if (it.kind != LINK) continue;
            ImGui::PushID(i);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float h = roundf(ImGui::GetTextLineHeight() + s_im.small->FontSize + 14 * k);
            bool current = onMain ? i == s_menu.sel : strcmp(it.section, kItems[s_menu.page].label) == 0;
            if (ImGui::InvisibleButton("cat", ImVec2(w, h))) {
                if (!onMain) GoBack();
                s_menu.sel = i;
                Change(i, +1);
            }
            bool hovered = ImGui::IsItemHovered();
            if (current) {
                sd->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), onMain ? IM_COL32(255, 150, 0, 70) : IM_COL32(255, 150, 0, 35), 4 * k);
                sd->AddRectFilled(p0, ImVec2(p0.x + 3 * k, p0.y + h), kAccent, 2 * k);
                if (onMain && s_im.lastSel != s_menu.sel) ImGui::SetScrollHereY(0.5f);
            } else if (hovered) {
                sd->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 12), 4 * k);
            }
            sd->AddText(ImVec2(p0.x + 12 * k, p0.y + 6 * k), current ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 215, 215, 255), it.label);
            std::string sum = LinkSummary(it.section);
            sd->AddText(s_im.small, s_im.small->FontSize, ImVec2(p0.x + 12 * k, p0.y + 6 * k + ImGui::GetTextLineHeight()),
                        IM_COL32(140, 140, 150, 255), sum.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 14 * k);

    // The page.
    ImGui::BeginChild("##page", ImVec2(0, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    if (shownPage) DrawPage(shownPage, !onMain, k);
    ImGui::EndChild();

    // Footer: what the selected setting does (or the last note), and the controls.
    ImVec2 fp = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(fp.x, fp.y + 4 * k), ImVec2(fp.x + ImGui::GetContentRegionAvail().x, fp.y + 4 * k), IM_COL32(60, 60, 66, 255));
    ImGui::Dummy(ImVec2(0, 8 * k));
    const Item& cur = kItems[s_menu.sel];
    bool noted = now - s_menu.noteTime < 3.0 && s_menu.capture < 0;
    if (noted) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.47f, 1.0f), "%s", s_menu.note.c_str());
    } else if (s_menu.capture >= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.47f, 1.0f), "%s",
                           cur.flags & BF_STEER ? "Turn the wheel..." : "Press a key, button or pedal...");
    } else {
        std::string help = onMain ? std::string("Settings for ") + cur.label + "." : HelpText(cur);
        std::string def = onMain ? std::string() : DefaultText(cur);
        std::string gdef = onMain ? std::string() : GameDefaultText(cur);
        if (!gdef.empty()) def += (def.empty() ? "" : "    ") + gdef;
        ImGui::TextUnformatted(help.c_str());
        if (!def.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("   %s", def.c_str());
        }
    }
    {
        std::string m = MenuKeyName(), keys;
        if (s_menu.capture >= 0) keys = m + ": cancel";
        else if (onMain) keys = "Up/Down: category    Right: open    " + m + ": close";
        else if (cur.kind == LINK && cur.section != kMainPage && strncmp(cur.label, "<", 1) != 0)
            keys = "Right: open    Left: back    " + m + ": close";
        else if (cur.kind == LINK) keys = "Left/Right: back    " + m + ": close";
        else if (cur.kind == BIND) keys = std::string("Right: add binding    Left: ") + (cur.flags & BF_REQUIRED ? "reset" : "unbind") + "    " + m + ": close";
        else if (cur.kind == ACTION || cur.kind == RESET) keys = "Right: do it    Left: back    " + m + ": close";
        else keys = "Up/Down: select    Left/Right: change    " + m + ": close";
        ImGui::PushFont(s_im.small);
        ImGui::TextDisabled("%s", keys.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
    s_im.lastSel = s_menu.sel;
    s_im.lastPage = s_menu.page;
    UpdateVrPages();

    ImGui::Render();
    RenderImGui(dev, ImGui::GetDrawData(), W, H);
}

}  // namespace

// Each menu row on another mod's setting must name the same key as the kExt entry it points at
// (the EXT enum and kExt listed in different orders swapped rows' settings once).
void CheckExtRows() {
    for (int i = 0; i < kNumItems; i++) {
        int ext = kItems[i].kind == NUMBER ? ExtIndex(kItems[i].value) : -1;
        if (ext < 0) continue;
        const char* section = kExt[ext].mod == MOD_FFB ? "FFB" : "VR";
        if (strcmp(kItems[i].key, kExt[ext].key) != 0 || strcmp(kItems[i].section, section) != 0)
            Log("ui: BUG: row \"%s\" (%s %s) points at %s %s", kItems[i].label, kItems[i].section, kItems[i].key, section,
                kExt[ext].key);
    }
}

void Ui_Frame() {
    static bool checked = false;
    if (!checked) {
        checked = true;
        CheckExtRows();
    }
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)kDevice;
    if (!dev) return;
    Physics_Frame();
    // The FFB mod's soft lock sits at the steering lock.
    if (float* at = ExtSetting(FFB_SOFTLOCKAT))
        *at = g_cfg.steer.empty() ? 1.0f : Clamp(g_cfg.steeringLock / (g_cfg.wheelRotation * g_cfg.steerSensitivity), 0.05f, 1.0f);
    // ... and hides the controllers from the game ([Controls] GameController=0).
    if (float* gc = ExtSetting(FFB_GAMECONTROLLER)) *gc = g_cfg.gameController ? 1.0f : 0.0f;
    if (float* mc = ExtSetting(FFB_MENUCONTROLLER)) *mc = g_cfg.menuController ? 1.0f : 0.0f;
    if (float* ma = ExtSetting(FFB_MENUAXES)) *ma = g_cfg.menuAxes ? 1.0f : 0.0f;
    EngineStatus st = Game_Engine();
    InputSnapshot in = Input_Get();
    // The cockpit's steering wheel turns with the bound wheel (VR mod, also on a flat screen).
    if (float* angle = ExtSetting(VR_WHEELANGLE)) *angle = in.steerBound ? in.wheelDeg : 0.0f;
    // VR seat bindings, held: the VR mod moves the seat while they are.
    auto held = [&](int seat) { return (in.seatDown >> seat & 1) ? 1.0f : 0.0f; };
    if (float* v = ExtSetting(VR_SEATRIGHT)) *v = held(SEAT_RIGHT) - held(SEAT_LEFT);
    if (float* v = ExtSetting(VR_SEATUP)) *v = held(SEAT_UP) - held(SEAT_DOWN);
    if (float* v = ExtSetting(VR_SEATFORWARD)) *v = held(SEAT_FORWARD) - held(SEAT_BACK);
    if (float* v = ExtSetting(VR_SEATRESET)) *v = held(SEAT_RESET);
    // [Controls] RecenterVr works everywhere, not only while driving.
    static uint32_t recenters = in.recenters;
    if (in.recenters != recenters) {
        recenters = in.recenters;
        Log("ui: VR recentre binding: %s", VrRecenter() ? "recentred" : "no VR mod or headset");
    }
    UpdateMenu(in, st.driving);
    if (float* v = ExtSetting(VR_MENUOPEN)) *v = s_menu.open ? 1.0f : 0.0f;
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

    if (!s_menu.open) s_im.wasOpen = false;
    if (s_menu.open && !g_cfg.classicMenu) {
        // Sized to fit both ways: the VR HUD target is nearly square, a monitor is wide.
        float ki = std::min(desc.Width / 1024.0f, desc.Height / 768.0f) * g_cfg.uiScale;
        DrawMenuImGui(dev, desc.Width, desc.Height, ki);
        return;
    }
    if (s_menu.open) DrawMenu(desc.Width, desc.Height, k);
    else DrawGauge(st, desc.Width, desc.Height, k);
    Flush(dev, desc.Width, desc.Height);
}
