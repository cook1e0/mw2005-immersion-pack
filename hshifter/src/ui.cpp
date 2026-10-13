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
// LINK switches between the main page and the fine tuning page (BF_TUNING items).
// CAR switches the current car between its own physics values and the [Physics] ones; the
// per-car sliders (Physics_IsPerCar) are saved to the car's section while it has its own.
enum Kind { HEADER, TOGGLE, NUMBER, BIND, LINK, CAR, ACTION, SOUND };
enum { BF_REQUIRED = 1, BF_CLUTCH = 2, BF_TUNING = 4 };

// Force feedback strength: the FFB mod's (dinput8.dll) [FFB] Gain, changed live through its
// NfsmwFfb_Gain export and saved to nfsmw_ffb.ini.
float s_ffbGain = 1.0f;
float* FfbGain() {
    static float* p;
    static bool looked;
    if (!looked) {
        looked = true;
        HMODULE m = GetModuleHandleA("dinput8.dll");
        FARPROC f = m ? GetProcAddress(m, "NfsmwFfb_Gain") : nullptr;
        p = f ? ((float* (__cdecl*)())f)() : nullptr;
        Log("ui: force feedback mod %s", p ? "found" : "not loaded");
    }
    return p;
}

std::string FfbIniPath() {
    std::string path = IniPath();
    size_t slash = path.find_last_of("\\/");
    return path.substr(0, slash + 1) + "nfsmw_ffb.ini";
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

const Item kItems[] = {
    // Main page: switches and controls.
    {HEADER, "ENGINE", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Engine wear", "Engine", "EngineWear", &g_cfg.engineWear, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Rev limiter", "Engine", "RevLimiter", &g_cfg.revLimiter, 0, 0, 0, 0, nullptr, 0},
    {HEADER, "DAMAGE AND HUD", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Car damage", "Damage", "Damage", &g_cfg.damage, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Engine smoke when damaged", "Damage", "Smoke", &g_cfg.damageSmoke, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Drivable popped tires", "Tires", "PoppedTires", &g_cfg.poppedTires, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Damage / heat meter", "HUD", "Meter", &g_cfg.hudMeter, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Speedbreaker", "HUD", "Speedbreaker", &g_cfg.speedbreaker, 0, 0, 0, 0, nullptr, 0},
    {HEADER, "SHIFTER AND PHYSICS", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Shifting", "Shifter", "Sequential", &g_cfg.sequential, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Gear keys stay in gear", "Shifter", "Latching", &g_cfg.latching, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Clutch needed to shift", "Shifter", "RequireClutch", &g_cfg.requireClutch, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Grind on missed clutch", "Shifter", "Grind", &g_cfg.grind, 0, 0, 0, 0, nullptr, 0},
    {SOUND, "Grind sound", "Shifter", "GrindSound", &g_cfg.grindSound, 0, 0, 0, 0, nullptr, 0},
    {TOGGLE, "Physics sliders", "Physics", "Physics", &g_cfg.physics, 0, 0, 0, 0, nullptr, 0},
    {NUMBER, "Traffic density", "Traffic", "Density", &g_cfg.trafficDensity, 0.0f, 3.0f, 0.25f, 100, "%.0f%%", 0},
    {NUMBER, "Force feedback strength", "FFB", "Gain", &s_ffbGain, 0.0f, 2.0f, 0.05f, 100, "%.0f%%", 0},
    {LINK, "Fine tuning", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0},
    {HEADER, "CONTROLS", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0},
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
    BINDING("Pop tires (testing)", "Tires", "DebugPopTires", debugPopTires, 0),
    BINDING("Menu", "UI", "Menu", menu, BF_REQUIRED),
    BINDING("Menu up", "UI", "MenuUp", menuUp, BF_REQUIRED),
    BINDING("Menu down", "UI", "MenuDown", menuDown, BF_REQUIRED),
    BINDING("Menu left", "UI", "MenuLeft", menuLeft, BF_REQUIRED),
    BINDING("Menu right", "UI", "MenuRight", menuRight, BF_REQUIRED),

    // Fine tuning page: the numbers.
    {LINK, "Back", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {HEADER, "ENGINE WEAR", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {NUMBER, "Heating at redline", "Engine", "HeatUp", &g_cfg.heatUp, 0.01f, 2.0f, 0.01f, 1, "%.2f /s", BF_TUNING},
    {NUMBER, "Heating at max rpm", "Engine", "HeatUpMaxRPM", &g_cfg.heatUpMax, 0.01f, 5.0f, 0.01f, 1, "%.2f /s", BF_TUNING},
    {NUMBER, "Cooling", "Engine", "CoolDown", &g_cfg.coolDown, 0.005f, 1.0f, 0.005f, 1, "%.3f /s", BF_TUNING},
    {NUMBER, "Power loss from", "Engine", "PowerLossStart", &g_cfg.powerLossStart, 0.0f, 0.95f, 0.05f, 100, "%.0f%% heat", BF_TUNING},
    {NUMBER, "Power lost at full heat", "Engine", "PowerLoss", &g_cfg.powerLoss, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {HEADER, "CAR DAMAGE", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {NUMBER, "Harmless impacts up to", "Damage", "MinImpact", &g_cfg.damageMinImpact, 0.0f, 30.0f, 0.5f, 1, "%.1f m/s", BF_TUNING},
    {NUMBER, "Damage strength", "Damage", "Strength", &g_cfg.damageStrength, 0.1f, 5.0f, 0.1f, 1, "%.1fx", BF_TUNING},
    {NUMBER, "Engine smoke from", "Damage", "SmokeFrom", &g_cfg.smokeFrom, 0.5f, 1.0f, 0.05f, 100, "%.0f%% damage", BF_TUNING},
    {NUMBER, "Engine smoke trail", "Damage", "SmokeTrail", &g_cfg.smokeTrail, 0.0f, 1.0f, 0.05f, 100, "%.0f%% of car speed", BF_TUNING},
    {NUMBER, "Power loss from", "Damage", "PowerLossStart", &g_cfg.damageLossStart, 0.0f, 0.95f, 0.05f, 100, "%.0f%% damage", BF_TUNING},
    {NUMBER, "Power lost at full damage", "Damage", "PowerLoss", &g_cfg.damageLoss, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {HEADER, "POPPED TIRES", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {NUMBER, "Popped tire handicap", "Tires", "Handicap", &g_cfg.tireHandicap, 0.0f, 3.0f, 0.05f, 1, "%.2fx", BF_TUNING},
    {ACTION, "Pop all four tires (testing)", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {HEADER, "PHYSICS (1 = GAME DEFAULT)", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {CAR, "Settings for this car only", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {NUMBER, "Traction control", "Physics", "TractionControl", &g_cfg.tractionControl, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {NUMBER, "Stability control", "Physics", "Stability", &g_cfg.stability, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {NUMBER, "Drift assist", "Physics", "DriftAssist", &g_cfg.driftAssist, 0.0f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {NUMBER, "Grip", "Physics", "Grip", &g_cfg.grip, 0.3f, 2.0f, 0.05f, 1, "%.2fx", BF_TUNING},
    {NUMBER, "Front torque share", "Physics", "TorqueSplit", &g_cfg.torqueSplit, 0.0f, 3.0f, 0.05f, 1, "%.2fx", BF_TUNING},
    {NUMBER, "Gravity", "Physics", "Gravity", &g_cfg.gravity, 0.1f, 3.0f, 0.05f, 1, "%.2f g", BF_TUNING},
    {TOGGLE, "Gravity for all cars", "Physics", "GravityAllCars", &g_cfg.gravityAll, 0, 0, 0, 0, nullptr, BF_TUNING},
    {HEADER, "SHIFTER AND CLUTCH", nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, BF_TUNING},
    {NUMBER, "Clutch travel to shift", "Shifter", "ClutchToShift", &g_cfg.shiftPedal, 0.05f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {NUMBER, "Clutchless rev-match", "Shifter", "RevMatchRPM", &g_cfg.revMatchRpm, 0.0f, 3000.0f, 50.0f, 1, "%.0f rpm", BF_TUNING},
    {NUMBER, "Clutch strength", "Clutch", "Strength", &g_cfg.strength, 0.1f, 2.0f, 0.1f, 1, "%.1f", BF_TUNING},
    {NUMBER, "Bite point start", "Clutch", "BiteStart", &g_cfg.biteStart, 0.05f, 1.0f, 0.05f, 100, "%.0f%%", BF_TUNING},
    {NUMBER, "Bite point end", "Clutch", "BiteEnd", &g_cfg.biteEnd, 0.0f, 0.95f, 0.05f, 100, "%.0f%%", BF_TUNING},
};
constexpr int kNumItems = sizeof(kItems) / sizeof(kItems[0]);
constexpr int kMaxRows = 18;        // rows shown at once; the list scrolls

struct {
    bool open = false;
    bool dirty = false;
    bool clutchCal = false;         // [Clutch] Released/Pressed changed by binding a pedal
    int page = 0;                   // 0 = main, 1 = fine tuning
    int sel = 1;                    // selected item (index into kItems)
    int first = 0;                  // first row shown
    bool synced = false;
    uint32_t menu = 0, nav[4] = {};
    int capture = -1;               // item waiting for a press
    double captureStart = 0;
    std::string note;               // short feedback under the list
    double noteTime = -1e9;
} s_menu;

// Sound moments for a missed shift: the totaled car's engine failure, and the two the engine
// sound plays once when its engine state changes (SFXCTL_Engine::UpdateParams, states 1 and 2).
const struct {
    uint32_t moment;
    const char* name;
} kGrindSounds[] = {
    {0xC565AC30, "Totaled"},
    {0xBC2DFA2F, "Engine state 1"},
    {0xBAE41D1B, "Engine state 2"},
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
    for (int i = 0; i < kNumItems; i++)
        if (((kItems[i].flags & BF_TUNING) != 0) == (s_menu.page == 1)) rows.push_back(i);
    return rows;
}

void SetPage(int page) {
    s_menu.page = page;
    s_menu.first = 0;
    for (int i : PageRows())
        if (kItems[i].kind != HEADER) {
            s_menu.sel = i;
            break;
        }
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

std::string ValueText(const Item& it, int index) {
    char buf[64];
    if (it.value == &g_cfg.sequential) return g_cfg.sequential ? "Sequential" : "H-pattern";
    if (it.kind == TOGGLE) return *(bool*)it.value ? "On" : "Off";
    if (it.kind == CAR) return *Physics_Car() ? (Physics_OwnSettings() ? "On" : "Off") : "No car";
    if (it.kind == LINK) return s_menu.page == 0 ? ">" : "<";
    if (it.kind == ACTION) return "Go";
    if (it.kind == SOUND) return SoundName(*(uint32_t*)it.value);
    if (it.kind == BIND) {
        if (index == s_menu.capture) return "Press a key, button or pedal...";
        std::string s;
        for (auto& t : SplitBindings(*(std::string*)it.value))
            s += (s.empty() ? "" : ", ") + (t.compare(0, 4, "key:") == 0 ? t.substr(4) : t);
        if (s.empty()) return "-";
        if (s.size() > 28) s = s.substr(0, 25) + "...";
        return s;
    }
    if (it.value == &s_ffbGain && !FfbGain()) return "No FFB mod";
    float v = *(float*)it.value;
    if (it.value == &g_cfg.revMatchRpm && v <= 0.0f) return "Off";
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
    std::vector<std::string> list = SplitBindings(field);
    for (auto& t : list)
        if (SameBinding(t, binding)) {
            Note(std::string(it.label) + ": " + binding + " is already bound");
            return;
        }
    list.push_back(binding);
    if ((it.flags & BF_CLUTCH) && axisRest >= 0) {
        // Pedal: remember where it rests; floored is taken to be the other end of its travel.
        g_cfg.clutchReleased = axisRest;
        g_cfg.clutchPressed = -1;
        s_menu.clutchCal = true;
    }
    Input_SetBinding(field, JoinBindings(list));
    s_menu.dirty = true;
    Note(std::string(it.label) + ": added " + binding);
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
    Input_SetBinding(field, value);
    s_menu.dirty = true;
    Note(std::string(it.label) + (value.empty() ? ": unbound" : ": back to default"));
    Log("ui: %s = %s", it.key, field.c_str());
}

void Change(int index, int dir) {
    const Item& it = kItems[index];
    if (it.kind == TOGGLE) {
        bool& b = *(bool*)it.value;
        b = !b;
    } else if (it.kind == NUMBER) {
        if (it.value == &s_ffbGain && !FfbGain()) return;
        float& v = *(float*)it.value;
        v = Clamp(roundf((v + dir * it.step) / it.step) * it.step, it.lo, it.hi);
        if (it.value == &s_ffbGain) *FfbGain() = v;
        // Keep the bite point ordered: start (released side) above end.
        if (g_cfg.biteStart < g_cfg.biteEnd + 0.05f) {
            if (it.value == &g_cfg.biteStart) g_cfg.biteEnd = std::max(0.0f, g_cfg.biteStart - 0.05f);
            else if (it.value == &g_cfg.biteEnd) g_cfg.biteStart = std::min(1.0f, g_cfg.biteEnd + 0.05f);
        }
    } else if (it.kind == LINK) {
        SetPage(1 - s_menu.page);
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
        if (dir < 0) return;
        Game_PopTires();
        Note("Tires popped (repair: safehouse, race restart or DebugRepair)");
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
        if (it.kind == HEADER || it.kind == LINK || it.kind == CAR || it.kind == ACTION) continue;
        char buf[32];
        std::string text;
        if (it.kind == TOGGLE) snprintf(buf, sizeof(buf), "%d", *(bool*)it.value ? 1 : 0), text = buf;
        else if (it.kind == NUMBER) snprintf(buf, sizeof(buf), "%g", *(float*)it.value), text = buf;
        else if (it.kind == SOUND) snprintf(buf, sizeof(buf), "0x%08X", *(uint32_t*)it.value), text = buf;
        else text = *(std::string*)it.value;
        if (it.value == &s_ffbGain) {
            if (FfbGain()) WritePrivateProfileStringA(it.section, it.key, text.c_str(), FfbIniPath().c_str());
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
    Log("ui: settings saved to %s", IniPath());
}

void CancelCapture() {
    if (s_menu.capture < 0) return;
    Input_CancelCapture();
    s_menu.capture = -1;
}

void SetOpen(bool open) {
    if (open == s_menu.open) return;
    s_menu.open = open;
    Log("ui: menu %s", open ? "opened" : "closed");
    if (open && FfbGain()) s_ffbGain = *FfbGain();
    if (!open) {
        CancelCapture();
        if (s_menu.dirty) {
            Save();
            s_menu.dirty = false;
        }
    }
    Game_SetMenuOpen(open);
}

void Step(int dir) {
    std::vector<int> rows = PageRows();
    int n = (int)rows.size();
    int pos = (int)(std::find(rows.begin(), rows.end(), s_menu.sel) - rows.begin());
    if (pos >= n) pos = 0;
    do pos = (pos + dir + n) % n;
    while (kItems[rows[pos]].kind == HEADER);
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
    else if (cur.kind == BIND)
        hint = std::string("Right: add binding    Left: ") + (cur.flags & BF_REQUIRED ? "reset" : "unbind") + "    " +
               MenuKeyName() + ": close";
    else if (cur.kind == ACTION) hint = "Right: do it    " + MenuKeyName() + ": close";
    else if (cur.kind == LINK) hint = "Left/Right: " + std::string(s_menu.page == 0 ? "open" : "back") + "    " + MenuKeyName() + ": close";
    else hint = "Up/Down: select    Left/Right: change    " + MenuKeyName() + ": close";
    bool noted = Now() - s_menu.noteTime < 3.0 && s_menu.capture < 0;
    std::string titleText = s_menu.page == 0 ? "DRIVETRAIN TUNING" : "FINE TUNING";
    if (s_menu.page == 1 && *Physics_Car()) {
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
        SetPage(s_menu.page);
        pos = (int)(std::find(page.begin(), page.end(), s_menu.sel) - page.begin());
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
        if (sel && !capturing) v = "< " + v + " >";
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

}  // namespace

void Ui_Frame() {
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)kDevice;
    if (!dev) return;
    Physics_Frame();
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
