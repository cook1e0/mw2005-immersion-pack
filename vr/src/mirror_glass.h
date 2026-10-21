// The glass of a car's side mirror, found in its model: the largest flat patch of triangles
// facing the back of the car (toward the driver). Model space: x forward, y left, z up.
//
// Most cars have the mirrors as their own solids (<CAR>_KIT00_LEFT_SIDE_MIRROR_A, ...); a few
// (911GT2, both M3 GTRs, Camaro, Corvette C6.R, SL65) have them in the body, where the search is
// limited to each side at mirror height. Checked offline against every car in the game: glass
// about 0.13..0.24 m wide and 0.08..0.14 m high, facing back and a little outward.
#pragma once
#include "vrmath.h"
#include <math.h>
#include <algorithm>
#include <vector>

struct MirrorGlass {
    bool ok = false;
    V3 centre{}, normal{};      // normal: out of the glass, toward the driver
    V3 u{}, v{};                // in the glass: u to the right as the driver sees it, v up
    float halfU = 0, halfV = 0;
    std::vector<V3> tris;       // the glass's own triangles (three points each), for its outline
};

namespace mirror {

inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Norm(V3 a) {
    float l = sqrtf(Dot(a, a));
    return l > 1e-9f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 0};
}

enum class GlassKind { Side, RearView };

// A triangle found facing back: its points, that normal, its area.
struct GlassTri {
    const V3* p;
    V3 n;
    float area;
};

// The flat patch a set of triangles makes: the plane through them, its centre and basis in it, its
// extents and how much of its bounding rectangle the triangles fill.
inline MirrorGlass FitGlass(const std::vector<const GlassTri*>& tris, float& fill) {
    V3 n{0, 0, 0};
    float area = 0;
    for (const GlassTri* t : tris) {
        n = {n.x + t->n.x * t->area, n.y + t->n.y * t->area, n.z + t->n.z * t->area};
        area += t->area;
    }
    n = Norm(n);
    V3 u = Norm(Cross({0, 0, 1}, n));
    V3 v = Cross(n, u);
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f, sumD = 0;
    int count = 0;
    for (const GlassTri* t : tris)
        for (int k = 0; k < 3; k++) {
            float pu = Dot(t->p[k], u), pv = Dot(t->p[k], v);
            minU = fminf(minU, pu); maxU = fmaxf(maxU, pu);
            minV = fminf(minV, pv); maxV = fmaxf(maxV, pv);
            sumD += Dot(t->p[k], n);
            count++;
        }
    float cu = 0.5f * (minU + maxU), cv = 0.5f * (minV + maxV), cd = sumD / count;
    MirrorGlass g;
    g.centre = {u.x * cu + v.x * cv + n.x * cd, u.y * cu + v.y * cv + n.y * cd, u.z * cu + v.z * cv + n.z * cd};
    g.normal = n;
    g.u = u;
    g.v = v;
    g.halfU = 0.5f * (maxU - minU);
    g.halfV = 0.5f * (maxV - minV);
    fill = area / fmaxf(4.0f * g.halfU * g.halfV, 1e-9f);
    return g;
}

// Whether a flat patch looks like that kind of mirror's glass.
inline bool LooksLike(GlassKind kind, const MirrorGlass& g, float fill) {
    if (kind == GlassKind::Side)
        // A side mirror's size, out at the side and not behind the cabin, and mostly filled.
        return g.halfU > 0.03f && g.halfU < 0.18f && g.halfV > 0.025f && g.halfV < 0.13f && fill > 0.3f &&
               fabsf(g.centre.y) > 0.6f && g.centre.x > -0.6f;
    // The rear view mirror: about 0.2 x 0.06 m on the car's centre line, facing straight back
    // (sun visors and roof strips nearby are wider, thinner or tilted).
    return g.halfU > 0.075f && g.halfU < 0.15f && g.halfV > 0.02f && g.halfV < 0.05f && fill > 0.6f &&
           g.normal.x < -0.9f && fabsf(g.centre.y) < 0.12f;
}

// `tris`: three points per triangle. Windings vary. With `outward` (a mirror's own model) each
// normal is turned away from the middle of the points, so a housing's flat front is never taken
// for glass facing back. Without (a region of a bigger model, whose middle is nowhere near the
// mirror) a triangle facing either way counts. Flat patches facing back are tried largest first;
// the first that looks like `kind`'s glass is it.
inline MirrorGlass FindGlass(const std::vector<V3>& tris, bool outward = true, GlassKind kind = GlassKind::Side) {
    MirrorGlass none;
    if (tris.size() < 3) return none;
    V3 mid{0, 0, 0};
    for (const V3& p : tris) mid = {mid.x + p.x, mid.y + p.y, mid.z + p.z};
    float inv = 1.0f / tris.size();
    mid = {mid.x * inv, mid.y * inv, mid.z * inv};
    std::vector<GlassTri> rear;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        V3 c = Cross(Sub(tris[i + 1], tris[i]), Sub(tris[i + 2], tris[i]));
        float a = 0.5f * sqrtf(Dot(c, c));
        if (a <= 1e-7f) continue;
        V3 n = Norm(c);
        V3 at = {(tris[i].x + tris[i + 1].x + tris[i + 2].x) / 3 - mid.x, (tris[i].y + tris[i + 1].y + tris[i + 2].y) / 3 - mid.y,
                 (tris[i].z + tris[i + 1].z + tris[i + 2].z) / 3 - mid.z};
        if (outward ? Dot(n, at) < 0 : n.x > 0) n = {-n.x, -n.y, -n.z};
        if (n.x < -0.5f) rear.push_back({&tris[i], n, a});
    }
    // Groups of connected triangles in one plane: grown from a triangle by the ones in its plane
    // that come within 2 cm of one already in. A piece elsewhere that happens to lie in the same
    // plane (a strip of the dashboard behind the Punto's rear view mirror) stays out, so it can't
    // stretch the glass past a mirror's size.
    auto touches = [](const GlassTri& a, const GlassTri& b) {
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++)
                if (fabsf(a.p[i].x - b.p[k].x) < 0.02f && fabsf(a.p[i].y - b.p[k].y) < 0.02f && fabsf(a.p[i].z - b.p[k].z) < 0.02f)
                    return true;
        return false;
    };
    struct Group { std::vector<const GlassTri*> tris; float area = 0; };
    std::vector<Group> groups;
    std::vector<bool> used(rear.size(), false);
    for (size_t i = 0; i < rear.size(); i++) {
        if (used[i]) continue;
        V3 n = rear[i].n;
        float d = Dot(n, rear[i].p[0]);
        Group gr;
        used[i] = true;
        gr.tris.push_back(&rear[i]);
        gr.area += rear[i].area;
        for (size_t k = 0; k < gr.tris.size(); k++)
            for (size_t j = 0; j < rear.size(); j++) {
                if (used[j] || Dot(n, rear[j].n) <= 0.97f || fabsf(Dot(n, rear[j].p[0]) - d) >= 0.01f || !touches(*gr.tris[k], rear[j]))
                    continue;
                used[j] = true;
                gr.tris.push_back(&rear[j]);
                gr.area += rear[j].area;
            }
        groups.push_back(gr);
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.area > b.area; });
    for (const Group& gr : groups) {
        float fill;
        MirrorGlass g = FitGlass(gr.tris, fill);
        if (LooksLike(kind, g, fill)) {
            g.ok = true;
            for (const GlassTri* t : gr.tris) g.tris.insert(g.tris.end(), t->p, t->p + 3);
            return g;
        }
    }
    return none;
}

// The rear view mirror's glass in an interior model's region (InRearViewRegion). Cars whose glass
// is a proper connected patch are found by the searcher above, but some glasses are a shallow
// faceted shell whose triangles don't share edges (the Eclipse's), where growing from shared edges
// finds a single facet, or a patch folded with the back of the housing a few millimetres behind.
// So first the largest triangle facing back seeds a patch of everything lying in its plane,
// whichever way it is wound, as long as the patch as a whole still looks like the mirror. A piece
// elsewhere in that plane (a scrap of the Punto's dashboard) stretches the patch past a mirror's
// size and leaves the glass to the connected search, whose 2 cm limit keeps it out.
inline MirrorGlass FindRearViewPlane(const std::vector<V3>& tris) {
    MirrorGlass none;
    std::vector<GlassTri> rear;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        V3 c = Cross(Sub(tris[i + 1], tris[i]), Sub(tris[i + 2], tris[i]));
        float a = 0.5f * sqrtf(Dot(c, c));
        if (a <= 1e-7f) continue;
        V3 n = Norm(c);
        if (n.x > 0) n = {-n.x, -n.y, -n.z};
        if (n.x < -0.5f) rear.push_back({&tris[i], n, a});
    }
    // Largest first: a mirror's own triangles are bigger than the trim around it, and a visor
    // strip that is too wide is passed over for the mirror behind it.
    std::sort(rear.begin(), rear.end(), [](const GlassTri& a, const GlassTri& b) { return a.area > b.area; });
    for (const GlassTri& seed : rear) {
        float d = Dot(seed.n, seed.p[0]);
        std::vector<const GlassTri*> patch;
        for (const GlassTri& t : rear)
            if (Dot(seed.n, t.n) >= 0.766f && fabsf(Dot(seed.n, t.p[0]) - d) < 0.012f) patch.push_back(&t);
        float fill;
        MirrorGlass g = FitGlass(patch, fill);
        if (!LooksLike(GlassKind::RearView, g, fill)) continue;
        g.ok = true;
        for (const GlassTri* t : patch) g.tris.insert(g.tris.end(), t->p, t->p + 3);
        return g;
    }
    return none;
}

inline MirrorGlass FindRearViewGlass(const std::vector<V3>& tris) {
    MirrorGlass g = FindRearViewPlane(tris);
    return g.ok ? g : FindGlass(tris, false, GlassKind::RearView);
}

// Triangles of an interior model in front of the driver's head (model space), up by the roof
// on the centre line: where a rear view mirror hangs (from 15 cm ahead: the CTS's driver sits close
// to it).
inline bool InRearViewRegion(const V3* p, const float head[3]) {
    for (int k = 0; k < 3; k++)
        if (p[k].x <= head[0] + 0.15f || p[k].x >= head[0] + 0.95f || fabsf(p[k].y) >= 0.25f || p[k].z <= head[2] - 0.2f ||
            p[k].z >= head[2] + 0.3f)
            return false;
    return true;
}

// Triangles of a body mesh that lie on one side (side = 1 left, -1 right) at mirror height.
inline bool InBodyMirrorRegion(const V3* p, float side) {
    for (int k = 0; k < 3; k++)
        if (p[k].y * side <= 0.65f || p[k].z <= 0.5f || p[k].z >= 1.05f) return false;
    return true;
}

}  // namespace mirror
