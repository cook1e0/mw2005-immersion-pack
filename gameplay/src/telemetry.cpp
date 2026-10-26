// The telemetry panel (telemetry.h).
//
// Every number is the game's: the player's car at its last suspension step (Physics_Sample). The
// acceleration is the change of the body's velocity between steps, in the car's own axes (+ right,
// + forward), and the yaw rate how its forward axis turns towards its right one. Each tyre's load
// and grip limit are the tyre model's own, and how hard it works is its force (side, and drive plus
// brake along it) over that limit, or past the limit the share of its force it could not keep. The
// grip circle is the four grip limits over the car's weight: what the tyres could give right now,
// downforce and all. The suspension's travel is its compression from where it sits at a standstill.
//
// The needles move towards those values with [Telemetry] Smoothing as the time constant; a step
// harder than a car can pull (a crash, a respawn) is left out rather than drawn as a spike.
#include "common.h"
#include "telemetry.h"
#include "draw2d.h"
#include "inisave.h"
#include "input.h"
#include "physics.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr float kG = 9.81f;
constexpr float kSpike = 4.0f;          // g: more than this between two steps is an impact, not driving
const char* const kWheelName[4] = {"FL", "FR", "RL", "RR"};

inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// What the panel shows: the needles.
struct Shown {
    float speed = 0;            // m/s
    float gLat = 0, gLon = 0;   // g, + right, + forward
    float gMax = 0;             // the most of it since the panel was shown or the car changed
    float yaw = 0;              // rad/s, + turning right
    float circle = 1.0f;        // the grip circle (g): the tyres' grip limits over the car's weight
    float front = 0.5f;         // the front axle's share of the load on the tyres
    float down = 0;             // downforce, N
    float util[4] = {};         // each tyre's force over its grip limit (1 = at it)
    float travel[4] = {};       // m, + = compressed from where it rests
    bool wheels = false;        // the tyre and suspension values are known
} s_d;

// The previous step the panel saw, for the velocity and axis changes.
struct {
    bool have = false;
    uint32_t step = 0;
    float vel[3], fwd[3];
} s_prev;

// Where each wheel's suspension rests, learnt at a standstill, for the car of this mass.
float s_rest[4];
bool s_restKnown = false;
float s_restMass = 0.0f;
float s_travelMax = 0.02f;      // the largest travel lately: the bars' scale

// The G dot's trail: where it was and when (Now()).
constexpr int kTrail = 128;     // at most one point per 1/60 s: two seconds
struct { float lat, lon; double t; } s_trail[kTrail];
int s_trailAt = 0;

double s_tPrev = 0;
bool s_wasShown = false;
draw2d::Font s_font;

void ClearTrail() {
    for (auto& p : s_trail) p.t = -1e9;
}

// Back to nothing: the panel shown again, or another car.
void Reset() {
    s_d = Shown();
    s_prev.have = false;
    ClearTrail();
    s_travelMax = 0.02f;
}

// Moves the needles towards the car's latest step, over dt seconds of frame time.
void Update(float dt) {
    const CarSample& c = Physics_Sample();
    if (c.step == s_prev.step && s_prev.have) return;      // paused, or no step since the last frame
    uint32_t steps = c.step - s_prev.step;
    if (!c.body || !(c.dt > 0.0f) || !(c.mass > 1.0f)) {
        s_prev.have = false;
        return;
    }
    if (fabsf(c.mass - s_restMass) > 1.0f) {               // another car: its own rest, scale and best
        s_restMass = c.mass;
        s_restKnown = false;
        s_d.gMax = 0.0f;
        s_travelMax = 0.02f;
    }
    float span = c.dt * (float)steps;
    bool fresh = !s_prev.have || steps > 30;                // the first step, or after a long gap
    float a[3] = {}, yaw = 0.0f;
    if (!fresh)
        for (int k = 0; k < 3; k++) a[k] = (c.vel[k] - s_prev.vel[k]) / span;
    if (!fresh) {
        float turn[3] = {(c.fwd[0] - s_prev.fwd[0]) / span, (c.fwd[1] - s_prev.fwd[1]) / span, (c.fwd[2] - s_prev.fwd[2]) / span};
        yaw = Dot3(turn, c.right);
    }
    s_prev.have = true;
    s_prev.step = c.step;
    memcpy(s_prev.vel, c.vel, sizeof(s_prev.vel));
    memcpy(s_prev.fwd, c.fwd, sizeof(s_prev.fwd));
    if (fresh) return;
    float gLat = Dot3(a, c.right) / kG, gLon = Dot3(a, c.fwd) / kG;
    if (!(hypotf(gLat, gLon) <= kSpike) || !(fabsf(yaw) < 10.0f)) return;    // an impact, or NaN

    float w = g_cfg.telSmooth > 0.001f ? 1.0f - expf(-dt / g_cfg.telSmooth) : 1.0f;
    auto Move = [w](float& v, float to) { v += (to - v) * w; };
    Move(s_d.speed, sqrtf(Dot3(c.vel, c.vel)));
    Move(s_d.gLat, gLat);
    Move(s_d.gLon, gLon);
    Move(s_d.yaw, yaw);
    Move(s_d.down, c.downforce);

    s_d.wheels = c.wheels;
    if (c.wheels) {
        float load = 0, front = 0, grip = 0;
        bool gripKnown = true;
        for (int i = 0; i < 4; i++) {
            const CarSample::Wheel& wh = c.wheel[i];
            load += wh.load;
            if (i < 2) front += wh.load;
            grip += wh.grip;
            if (!(wh.grip > 0.0f) && wh.load > 0.0f) gripKnown = false;
            float util = 0.0f;
            if (wh.kept > 0.0f && wh.kept < 0.999f) util = 1.0f / wh.kept;
            else if (wh.grip > 0.0f) util = hypotf(wh.side, wh.along) / wh.grip;
            Move(s_d.util[i], Clamp(util, 0.0f, 2.0f));
        }
        // With the weight on the wheels (not in the air, not on two of them).
        if (load > 0.5f * c.mass * kG) {
            Move(s_d.front, front / load);
            if (gripKnown) Move(s_d.circle, Clamp(grip / (c.mass * kG), 0.3f, 4.0f));
        }
        // The suspension at rest: where it sits at a standstill on all four wheels (first seen
        // wherever the car is, then learnt again each time it stops).
        bool still = s_d.speed < 1.0f && hypotf(gLat, gLon) < 0.05f;
        bool down = c.wheel[0].onGround && c.wheel[1].onGround && c.wheel[2].onGround && c.wheel[3].onGround;
        if (down && (!s_restKnown || still)) {
            float k = s_restKnown ? std::min(1.0f, dt / 0.5f) : 1.0f;
            for (int i = 0; i < 4; i++) s_rest[i] += (c.wheel[i].compression - s_rest[i]) * k;
            s_restKnown = true;
        }
        for (int i = 0; i < 4; i++) {
            float travel = s_restKnown ? c.wheel[i].compression - s_rest[i] : 0.0f;
            Move(s_d.travel[i], travel);
            s_travelMax = std::max(s_travelMax - dt * 0.01f, std::max(0.02f, fabsf(s_d.travel[i])));
        }
    }
    float now = hypotf(s_d.gLat, s_d.gLon);
    if (now <= s_d.circle * 1.5f) s_d.gMax = std::max(s_d.gMax, now);

    // The trail: a point at most every 1/60 s.
    double t = Now();
    if (t - s_trail[s_trailAt].t >= 1.0 / 60.0) {
        s_trailAt = (s_trailAt + 1) % kTrail;
        s_trail[s_trailAt] = {s_d.gLat, s_d.gLon, t};
    }

    if (g_cfg.logPhysics) {
        static double last;
        if (t - last >= 1.0) {
            last = t;
            Log("telemetry: %.0f km/h, %+.2f g right %+.2f g forward, yaw %+.2f rad/s, circle %.2f g, front %.0f%%, "
                "down %.0f N; FL FR RL RR load %.0f %.0f %.0f %.0f N, grip %.0f %.0f %.0f %.0f N, work %.2f %.2f %.2f %.2f, "
                "compression %.3f %.3f %.3f %.3f (rest %.3f %.3f %.3f %.3f)",
                s_d.speed * 3.6f, s_d.gLat, s_d.gLon, s_d.yaw, s_d.circle, s_d.front * 100.0f, s_d.down, c.wheel[0].load,
                c.wheel[1].load, c.wheel[2].load, c.wheel[3].load, c.wheel[0].grip, c.wheel[1].grip, c.wheel[2].grip,
                c.wheel[3].grip, s_d.util[0], s_d.util[1], s_d.util[2], s_d.util[3], c.wheel[0].compression,
                c.wheel[1].compression, c.wheel[2].compression, c.wheel[3].compression, s_rest[0], s_rest[1], s_rest[2],
                s_rest[3]);
        }
    }
}

}  // namespace

void Telemetry_Frame(bool driving) {
    // The binding switches the panel and saves it, like ToggleAbs.
    static uint32_t toggles = Input_Get().telemetryToggles;
    uint32_t now = Input_Get().telemetryToggles;
    if (now != toggles) {
        toggles = now;
        g_cfg.telemetry = !g_cfg.telemetry;
        IniSave_Queue({{IniPath(), "Telemetry", "Show", g_cfg.telemetry ? "1" : "0"}});
        Log("telemetry: panel %s (binding)", g_cfg.telemetry ? "shown" : "hidden");
    }
    double t = Now();
    float dt = s_tPrev > 0.0 ? (float)std::min(t - s_tPrev, 0.25) : 1.0f / 60.0f;
    s_tPrev = t;
    bool shown = g_cfg.telemetry && driving;
    if (shown && !s_wasShown) Reset();
    s_wasShown = shown;
    if (shown) Update(dt);
}

bool Telemetry_Shown() { return s_wasShown; }

// The layout is in units of a screen 768 pixels tall, times [Telemetry] Size; X and Y put the
// panel's centre on the screen.
void Telemetry_Draw(IDirect3DDevice9* dev, int w, int h) {
    if (!dev || w < 64 || h < 64 || !s_wasShown) return;
    using namespace draw2d;
    float k = (float)h / 768.0f * g_cfg.telSize;
    const float PW = 420, PH = 252;
    float pw = PW * k, ph = PH * k;
    if (pw > (float)w - 16.0f || ph > (float)h - 16.0f) return;    // no room: nothing rather than a mess
    int px = std::max(8, (int)lroundf(16.0f * k));
    if (px != s_font.px) BuildFont(s_font, px);
    if (!s_font.height) return;
    UseFont(s_font);

    float alpha = g_cfg.telOpacity;
    auto C = [alpha](int a, int r, int g, int b) { return D3DCOLOR_ARGB((int)lroundf((float)a * alpha), r, g, b); };
    // A filled circle, as a fan, two segments to a Quad4.
    auto Disc = [](float cx, float cy, float r, int segs, D3DCOLOR col) {
        float step = 6.2831853f / (float)segs;
        for (int i = 0; i < segs; i += 2) {
            float a0 = i * step, a1 = a0 + step, a2 = a1 + step;
            float xy[8] = {cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r,
                           cx + cosf(a2) * r, cy + sinf(a2) * r};
            Quad4(xy, col);
        }
    };
    auto Ring = [](float cx, float cy, float r, float thick, D3DCOLOR col) {
        const int segs = 48;
        for (int i = 0; i < segs; i++) {
            float a0 = i * 6.2831853f / segs, a1 = (i + 1) * 6.2831853f / segs;
            Line(cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, thick, col);
        }
    };
    const Shown& d = s_d;
    char buf[64];

    float x0 = Clamp(g_cfg.telX * (float)w - pw * 0.5f, 8.0f, (float)w - pw - 8.0f);
    float y0 = Clamp(g_cfg.telY * (float)h - ph * 0.5f, 8.0f, (float)h - ph - 8.0f);
    float fh = (float)s_font.height;

    Quad(x0, y0, x0 + pw, y0 + ph, C(110, 8, 10, 14));
    Quad(x0, y0, x0 + pw, y0 + 26 * k, C(130, 34, 38, 46));
    float ty = y0 + (26 * k - fh) * 0.5f;
    ShadowText(x0 + 12 * k, ty, "TELEMETRY", C(255, 255, 210, 90), k);
    snprintf(buf, sizeof(buf), "%.0f km/h", d.speed * 3.6f);
    ShadowText(x0 + pw - 12 * k - TextWidth(buf), ty, buf, C(255, 235, 240, 245), k);

    // The G-force circle: the outer ring is the grip the tyres have now, the dot where the car is,
    // the faint line behind it where it has been. Right is right, up is forward: the bottom half is
    // braking, the top half accelerating.
    if (g_cfg.telShowG) {
        float cx = x0 + 106 * k, cy = y0 + 136 * k, R = 86 * k;
        float limit = std::max(0.3f, d.circle);
        float ppg = R / limit;                              // pixels in one g
        Disc(cx, cy, R, 40, C(140, 12, 15, 20));
        for (float g = 0.5f; g < limit - 0.05f; g += 0.5f) Ring(cx, cy, g * ppg, k, C(80, 90, 100, 115));
        Line(cx - R, cy, cx + R, cy, k, C(80, 90, 100, 115));
        Line(cx, cy - R, cx, cy + R, k, C(80, 90, 100, 115));
        Ring(cx, cy, R, 2.0f * k, C(170, 210, 165, 70));    // the limit: past it the tyres slide
        double now = Now();
        float len = g_cfg.telTrail;
        for (int i = 0; len > 0.0f && i < kTrail; i++) {
            float age = (float)(now - s_trail[i].t);
            if (age < 0.0f || age > len) continue;
            float f = 1.0f - age / len;                     // the older, the fainter
            float tx = s_trail[i].lat * ppg, tyy = -s_trail[i].lon * ppg, tl = hypotf(tx, tyy);
            if (tl > R) tx *= R / tl, tyy *= R / tl;
            Disc(cx + tx, cy + tyy, (0.8f + 1.4f * f) * k, 8, C((int)(30.0f + 80.0f * f), 110, 190, 255));
        }
        float vx = d.gLat * ppg, vy = -d.gLon * ppg, vl = hypotf(vx, vy);
        if (vl > R) vx *= R / vl, vy *= R / vl;             // past the limit it stays on the ring
        Line(cx, cy, cx + vx, cy + vy, 1.5f * k, C(190, 250, 120, 60));
        Disc(cx + vx, cy + vy, 4.5f * k, 16, C(255, 255, 240, 220));
        snprintf(buf, sizeof(buf), "%.1f g", limit);
        Text(cx + R * 0.71f + 2 * k, cy - R * 0.71f - fh, buf, C(200, 210, 165, 70));
    }

    // The bars, each block a 2x2 - the front pair on the top row, the rear on the bottom - the
    // tyres on the left, the suspension on the right.
    const float bw = 30 * k, gap = 16 * k;
    const float bx0 = x0 + 218 * k;
    const float sx0 = bx0 + 2 * bw + 2 * gap;
    const float bt = y0 + 56 * k;
    const float rbh = 52 * k;
    const float row2 = bt + rbh + 14 * k + fh;
    auto Bar = [&](float bx, float top, int i, bool susp) {
        Quad(bx, top, bx + bw, top + rbh, C(140, 14, 17, 22));
        if (d.wheels && susp) {
            // Up compressed, down extended, scaled to the most travel seen lately.
            float mid = top + rbh * 0.5f;
            Line(bx, mid, bx + bw, mid, k, C(120, 90, 100, 115));
            float t = Clamp(d.travel[i] / s_travelMax, -1.0f, 1.0f), len = rbh * 0.5f * fabsf(t);
            if (t >= 0.0f) Quad(bx, mid - len, bx + bw, mid, C(225, 110, 175, 255));
            else Quad(bx, mid, bx + bw, mid + len, C(225, 255, 170, 90));
        } else if (d.wheels) {
            // Green working, amber near the limit, red past it; the bar runs past the limit line.
            float u = Clamp(d.util[i], 0.0f, 1.3f), fill = rbh * (u / 1.3f);
            D3DCOLOR col = u < 0.7f ? C(225, 90, 205, 110) : u < 1.0f ? C(235, 250, 190, 60) : C(245, 245, 70, 55);
            Quad(bx, top + rbh - fill, bx + bw, top + rbh, col);
            float ly = top + rbh - rbh / 1.3f;
            Line(bx - 2 * k, ly, bx + bw + 2 * k, ly, k, C(160, 235, 240, 245));
        }
        ShadowText(bx + bw * 0.5f - TextWidth(kWheelName[i]) * 0.5f, top + rbh + 2 * k, kWheelName[i], C(235, 200, 210, 220), k);
    };
    for (int b = 0; b < 2; b++) {
        bool susp = b == 1;
        if (!(susp ? g_cfg.telShowSusp : g_cfg.telShowTyres)) continue;
        float bx = susp ? sx0 : bx0;
        ShadowText(bx, y0 + 34 * k, susp ? "SUSPENSION" : "TYRES", C(255, 120, 190, 255), k);
        Bar(bx, bt, 0, susp);
        Bar(bx + bw + gap, bt, 1, susp);
        Bar(bx, row2, 2, susp);
        Bar(bx + bw + gap, row2, 3, susp);
    }

    // The bottom row: the force now and the most of the run, the front axle's share of the load, and
    // the downforce (as the mass it would weigh).
    float by = y0 + ph - fh * 1.25f;
    snprintf(buf, sizeof(buf), "%.2f g   max %.2f g", hypotf(d.gLat, d.gLon), d.gMax);
    ShadowText(x0 + 12 * k, by, buf, C(255, 235, 240, 245), k);
    float rx = x0 + pw - 12 * k;
    if (g_cfg.telShowAero) {
        snprintf(buf, sizeof(buf), "down %.0f kg", d.down / kG);
        float tw = TextWidth(buf);
        ShadowText(rx - tw, by, buf, C(230, 150, 190, 255), k);
        rx -= tw + 14 * k;
    }
    if (g_cfg.telShowTyres && d.wheels) {
        snprintf(buf, sizeof(buf), "front %.0f%%", d.front * 100.0f);
        ShadowText(rx - TextWidth(buf), by, buf, C(230, 200, 210, 220), k);
    }
    Flush(dev, w, h);
}
