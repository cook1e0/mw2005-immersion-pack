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
#include <vector>

struct MirrorGlass {
    bool ok = false;
    V3 centre{}, normal{};      // normal: out of the glass, toward the driver
    V3 u{}, v{};                // in the glass: u to the right as the driver sees it, v up
    float halfU = 0, halfV = 0;
};

namespace mirror {

inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Norm(V3 a) {
    float l = sqrtf(Dot(a, a));
    return l > 1e-9f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 0};
}

// `tris`: three points per triangle. Windings vary. With `outward` (a mirror's own model) each
// normal is turned away from the middle of the points, so a housing's flat front is never taken
// for glass facing back. Without (the body, searched only at mirror height on one side, whose
// middle is nowhere near the mirror) a triangle facing either way counts.
inline MirrorGlass FindGlass(const std::vector<V3>& tris, bool outward = true) {
    struct Tri { const V3* p; V3 n; float area; };
    MirrorGlass g;
    if (tris.size() < 3) return g;
    V3 mid{0, 0, 0};
    for (const V3& p : tris) mid = {mid.x + p.x, mid.y + p.y, mid.z + p.z};
    float inv = 1.0f / tris.size();
    mid = {mid.x * inv, mid.y * inv, mid.z * inv};
    std::vector<Tri> rear;
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
    // The largest group of triangles in one plane.
    float bestArea = 0;
    std::vector<const Tri*> best;
    std::vector<bool> used(rear.size(), false);
    for (size_t i = 0; i < rear.size(); i++) {
        if (used[i]) continue;
        V3 n = rear[i].n;
        float d = Dot(n, rear[i].p[0]);
        std::vector<const Tri*> group;
        float area = 0;
        for (size_t j = 0; j < rear.size(); j++) {
            if (used[j] || Dot(n, rear[j].n) <= 0.97f || fabsf(Dot(n, rear[j].p[0]) - d) >= 0.01f) continue;
            used[j] = true;
            group.push_back(&rear[j]);
            area += rear[j].area;
        }
        if (area > bestArea) {
            bestArea = area;
            best.swap(group);
        }
    }
    if (best.empty()) return g;
    V3 n{0, 0, 0};
    for (const Tri* t : best) n = {n.x + t->n.x * t->area, n.y + t->n.y * t->area, n.z + t->n.z * t->area};
    n = Norm(n);
    V3 u = Norm(Cross({0, 0, 1}, n));
    V3 v = Cross(n, u);
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f, sumD = 0;
    int count = 0;
    for (const Tri* t : best)
        for (int k = 0; k < 3; k++) {
            float pu = Dot(t->p[k], u), pv = Dot(t->p[k], v);
            minU = fminf(minU, pu); maxU = fmaxf(maxU, pu);
            minV = fminf(minV, pv); maxV = fmaxf(maxV, pv);
            sumD += Dot(t->p[k], n);
            count++;
        }
    float cu = 0.5f * (minU + maxU), cv = 0.5f * (minV + maxV), cd = sumD / count;
    g.centre = {u.x * cu + v.x * cv + n.x * cd, u.y * cu + v.y * cv + n.y * cd, u.z * cu + v.z * cv + n.z * cd};
    g.normal = n;
    g.u = u;
    g.v = v;
    g.halfU = 0.5f * (maxU - minU);
    g.halfV = 0.5f * (maxV - minV);
    float fill = bestArea / fmaxf(4.0f * g.halfU * g.halfV, 1e-9f);
    // A mirror's size, out at the side and not behind the cabin, and mostly filled.
    g.ok = g.halfU > 0.03f && g.halfU < 0.18f && g.halfV > 0.025f && g.halfV < 0.13f && fill > 0.3f &&
           fabsf(g.centre.y) > 0.6f && g.centre.x > -0.6f;
    return g;
}

// Triangles of a body mesh that lie on one side (side = 1 left, -1 right) at mirror height.
inline bool InBodyMirrorRegion(const V3* p, float side) {
    for (int k = 0; k < 3; k++)
        if (p[k].y * side <= 0.65f || p[k].z <= 0.5f || p[k].z >= 1.05f) return false;
    return true;
}

}  // namespace mirror
