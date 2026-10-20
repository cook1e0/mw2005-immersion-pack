// Minimal vector/quaternion/matrix helpers.
//
// Conventions:
//  * Quat/V3 poses are OpenXR poses (right-handed, +x right, +y up, -z forward).
//  * M4 is a row-major 4x4 in the game's row-vector convention (p' = p * M,
//    translation in row 3), same layout as the game's bMatrix4 / D3DXMATRIX.
#pragma once
#include <math.h>

struct V3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct M4 { float m[4][4]; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline Quat QConj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline V3 QRotate(Quat q, V3 v) {
    Quat p = {v.x, v.y, v.z, 0};
    Quat r = QMul(QMul(q, p), QConj(q));
    return {r.x, r.y, r.z};
}

// Spherical interpolation between unit quaternions (shortest path).
inline Quat QSlerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float wa, wb;
    if (d > 0.9995f) {
        wa = 1 - t;
        wb = t;
    } else {
        float th = acosf(d), sn = sinf(th);
        wa = sinf((1 - t) * th) / sn;
        wb = sinf(t * th) / sn;
    }
    Quat r = {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return {r.x / n, r.y / n, r.z / n, r.w / n};
}

inline Quat QFromAxisAngle(V3 axis, float angle) {
    float s = sinf(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f)};
}

// Column-vector rotation matrix (x' = R x) from a unit quaternion.
inline void QToMat3(Quat q, float R[3][3]) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w);     R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w);     R[2][1] = 2 * (y * z + x * w);     R[2][2] = 1 - 2 * (x * x + y * y);
}

inline M4 MMul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Inverse of a rigid transform (rotation + translation, row-vector form).
inline M4 MInvRigid(const M4& a) {
    M4 r = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
    for (int j = 0; j < 3; j++)
        r.m[3][j] = -(a.m[3][0] * r.m[0][j] + a.m[3][1] * r.m[1][j] + a.m[3][2] * r.m[2][j]);
    r.m[3][3] = 1;
    return r;
}

// Removes roll (and optionally pitch) from a world->camera view matrix, keeping its
// position and heading. Rows of the inverse view matrix are the camera's right,
// down and forward axes in world space; we rebuild them around the world's up axis.
inline M4 LevelCamera(const M4& V, V3 up, float rollAmount, float pitchAmount) {
    M4 W = MInvRigid(V);
    V3 right = {W.m[0][0], W.m[0][1], W.m[0][2]};
    V3 fwd = {W.m[2][0], W.m[2][1], W.m[2][2]};
    auto dot = [](V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    auto cross = [](V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    auto norm = [&](V3 a) { float l = sqrtf(dot(a, a)); return l > 1e-6f ? a * (1.0f / l) : a; };

    V3 flatFwd = fwd - up * dot(fwd, up);
    if (dot(flatFwd, flatFwd) < 1e-4f) return V; // looking straight up/down: leave it
    flatFwd = norm(flatFwd);
    V3 f = norm(fwd * (1 - pitchAmount) + flatFwd * pitchAmount);

    // Down axis completes the basis with the original handedness (right x down = +/-forward).
    V3 origDown = {W.m[1][0], W.m[1][1], W.m[1][2]};
    float handed = dot(cross(right, origDown), fwd) >= 0 ? 1.0f : -1.0f;
    // Fully level "right" axis: horizontal and perpendicular to f, on the side that puts "down"
    // toward the ground, whatever way up the car is: a car on its roof doesn't turn the view over.
    V3 levelRight = norm(cross(up, f));
    if (dot(cross(f, levelRight) * handed, up) > 0) levelRight = levelRight * -1.0f;
    // Part of the roll kept: turn the level axis toward the original one by that share of the
    // angle between them (blending the vectors would collapse when they point opposite ways).
    V3 r = levelRight;
    if (rollAmount < 0.999f) {
        V3 cur = norm(right - f * dot(right, f));
        float angle = atan2f(dot(cross(levelRight, cur), f), dot(levelRight, cur)) * (1 - rollAmount);
        r = norm(levelRight * cosf(angle) + cross(f, levelRight) * sinf(angle));
    }
    V3 d = cross(f, r) * handed;

    M4 L = W;
    const V3 axes[3] = {r, d, f};
    for (int i = 0; i < 3; i++) {
        L.m[i][0] = axes[i].x;
        L.m[i][1] = axes[i].y;
        L.m[i][2] = axes[i].z;
    }
    return MInvRigid(L);
}
