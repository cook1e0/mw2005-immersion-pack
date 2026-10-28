#pragma once
#include "vrmath.h"
#include <algorithm>

struct SpectatorCameraInput {
    Quat eye;
    float sourceTanX, sourceTanY;
    int width, height;
    float dt;
    bool reset = false;
};

struct SpectatorCameraOptions {
    float fov = 80, aspect = 16.0f / 9.0f, smoothing = 0.2f;
    bool followHead = true;
    bool adaptive = false;
};

struct SpectatorCamera {
    Quat rotation;
    float tanX, tanY;
    bool corrected = false;
};

class SpectatorController {
public:
    SpectatorCamera Step(const SpectatorCameraInput& in, const SpectatorCameraOptions& opt) {
        const float aspect = std::max(0.25f, opt.aspect);
        const Lens hard = {in.sourceTanX * (1 - 2.0f / std::max(in.width, 8)),
                           in.sourceTanY * (1 - 2.0f / std::max(in.height, 8))};
        float x = tanf(std::max(10.0f, std::min(opt.fov, 160.0f)) * kPi / 360);
        x = std::min(x, std::min(hard.x, aspect * hard.y));
        cropX = x;
        cropY = x / aspect;
        eye = in.eye;
        Angles head = Extract(eye);
        const bool reset = !initialized || in.reset || in.dt <= 0 || in.dt > 0.1f;
        if (reset) {
            yaw = head.yaw;
            pitch = head.pitch;
            roll = 0;
            speedYaw = speedPitch = 0;
            previousYaw = head.yaw;
            previousPitch = head.pitch;
            initialized = true;
        }
        float dt = reset ? 0 : in.dt;
        float rawYawSpeed = dt > 0 ? Wrap(head.yaw - previousYaw) / dt : 0;
        float rawPitchSpeed = dt > 0 ? (head.pitch - previousPitch) / dt : 0;
        float velocityAlpha = 1 - expf(-2 * kPi * dt);
        speedYaw += velocityAlpha * (rawYawSpeed - speedYaw);
        speedPitch += velocityAlpha * (rawPitchSpeed - speedPitch);
        previousYaw = head.yaw;
        previousPitch = head.pitch;
        float targetYaw = opt.followHead ? head.yaw : 0;
        float targetPitch = opt.followHead ? head.pitch : 0;
        float aYaw = Alpha(dt, opt.smoothing, opt.adaptive ? speedYaw : 0);
        float aPitch = Alpha(dt, opt.smoothing, opt.adaptive ? speedPitch : 0);
        if (reset) aYaw = aPitch = 1;
        float desiredYaw = yaw + Wrap(targetYaw - yaw) * aYaw;
        float desiredPitch = pitch + (targetPitch - pitch) * aPitch;

        // Use the final 12% of the available crop margin for early correction.
        // A soft boundary must never force roll when the hard boundary permits level output.
        Lens soft = {cropX + (hard.x - cropX) * 0.88f, cropY + (hard.y - cropY) * 0.88f};
        float correctedYaw = desiredYaw, correctedPitch = desiredPitch;
        bool level = Level(desiredYaw, desiredPitch, head, soft, correctedYaw, correctedPitch);
        if (!level) level = Level(desiredYaw, desiredPitch, head, hard, correctedYaw, correctedPitch);
        float requiredRoll = 0;
        if (!level) {
            correctedYaw = head.yaw;
            correctedPitch = head.pitch;
            requiredRoll = SafeRoll(correctedYaw, correctedPitch, 0, head.roll, hard);
        }
        // Release temporary roll slowly, but apply newly required coverage immediately.
        float releasedRoll = roll * expf(-dt / 0.2f);
        roll = SafeRoll(correctedYaw, correctedPitch, releasedRoll, requiredRoll, hard);
        yaw = Wrap(correctedYaw);
        pitch = correctedPitch;
        Quat result = Rotation(yaw, pitch, roll);
        if (!Covers(result, hard)) {
            // Exact source pose is a verified endpoint even at Euler singularities.
            result = eye;
            yaw = head.yaw;
            pitch = head.pitch;
            roll = head.roll;
        }
        bool corrected = fabsf(Wrap(yaw - desiredYaw)) > 1e-5f || fabsf(pitch - desiredPitch) > 1e-5f || fabsf(roll) > 1e-5f;
        return {result, cropX, cropY, corrected};
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;
    struct Lens { float x, y; };
    struct Angles { float yaw, pitch, roll; };
    bool initialized = false;
    float yaw = 0, pitch = 0, roll = 0, previousYaw = 0, previousPitch = 0;
    float speedYaw = 0, speedPitch = 0, cropX = 0, cropY = 0;
    Quat eye = {0,0,0,1};

    static float Wrap(float a) { return remainderf(a, 2 * kPi); }
    static Quat Rotation(float y, float p, float r) {
        return QMul(QMul(QFromAxisAngle({0,1,0}, y), QFromAxisAngle({1,0,0}, p)), QFromAxisAngle({0,0,1}, r));
    }
    static Angles Extract(Quat q) {
        V3 f = QRotate(q, {0,0,-1});
        float y = atan2f(-f.x, -f.z);
        float p = asinf(std::max(-1.0f, std::min(f.y, 1.0f)));
        Quat residual = QMul(QConj(Rotation(y,p,0)), q);
        return {y, p, Wrap(2 * atan2f(residual.z, residual.w))};
    }
    static float Alpha(float dt, float tau, float speed) {
        if (tau <= 0) return 1;
        // One Euro cutoff: rest smoothing retains the existing seconds control.
        float cutoff = 1 / (2 * kPi * tau) + 0.6f * fabsf(speed);
        return 1 - expf(-2 * kPi * cutoff * dt);
    }
    bool Covers(Quat q, Lens lens) const {
        Quat relative = QMul(QConj(eye), q);
        for (int ix : {-1,1}) for (int iy : {-1,1}) {
            V3 r = QRotate(relative, {ix * cropX, iy * cropY, -1});
            if (-r.z < 1e-5f || fabsf(r.x) > -r.z * lens.x || fabsf(r.y) > -r.z * lens.y) return false;
        }
        return true;
    }
    bool Level(float fromY, float fromP, Angles head, Lens lens, float& toY, float& toP) const {
        float dy = Wrap(head.yaw - fromY), dp = head.pitch - fromP;
        auto covered = [&](float t) { return Covers(Rotation(fromY + dy*t, fromP + dp*t, 0), lens); };
        if (covered(0)) { toY = fromY; toP = fromP; return true; }
        // A verified bracket does not assume coverage is monotonic along the whole path.
        float unsafe = 0;
        for (int i = 1; i <= 32; i++) {
            float safe = i / 32.0f;
            if (!covered(safe)) { unsafe = safe; continue; }
            for (int j = 0; j < 18; j++) {
                float mid = (unsafe + safe) * 0.5f;
                if (covered(mid)) safe = mid; else unsafe = mid;
            }
            toY = fromY + dy*safe;
            toP = fromP + dp*safe;
            return true;
        }
        return false;
    }
    float SafeRoll(float y, float p, float from, float to, Lens lens) const {
        auto covered = [&](float r) { return Covers(Rotation(y,p,r), lens); };
        if (covered(from)) return from;
        float delta = Wrap(to - from);
        if (fabsf(delta) < 1e-7f) return to;
        // Each corner/plane constraint is A*cos(roll)+B*sin(roll)+C <= 0.
        // Enumerate its roots so narrow feasible intervals cannot be skipped by a coarse scan.
        float points[256] = {0,1};
        int count = 2;
        float m[3][3];
        QToMat3(QMul(QConj(eye), Rotation(y,p,0)), m);
        const V3 planes[5] = {{1,0,lens.x},{-1,0,lens.x},{0,1,lens.y},{0,-1,lens.y},{0,0,1}};
        for (int ix : {-1,1}) for (int iy : {-1,1}) for (V3 n : planes) {
            float a = n.x*m[0][0] + n.y*m[1][0] + n.z*m[2][0];
            float b = n.x*m[0][1] + n.y*m[1][1] + n.z*m[2][1];
            float c = -(n.x*m[0][2] + n.y*m[1][2] + n.z*m[2][2]);
            float A = a*ix*cropX + b*iy*cropY, B = -a*iy*cropY + b*ix*cropX;
            float radius = hypotf(A,B);
            if (radius < 1e-7f || fabsf(c) > radius) continue;
            float phase = atan2f(B,A), angle = acosf(std::max(-1.0f,std::min(-c/radius,1.0f)));
            for (int sign : {-1,1}) for (int k = -2; k <= 2; k++) {
                float r = phase + sign*angle + k*2*kPi;
                float t = (r - from) / delta;
                if (t > 0 && t < 1) points[count++] = t;
            }
        }
        std::sort(points, points + count);
        for (int i = 0; i + 1 < count; i++) {
            float low = points[i], high = points[i+1];
            float middle = (low + high) * 0.5f;
            if (!covered(from + delta*middle)) continue;
            float safe = middle;
            for (int j = 0; j < 18; j++) {
                float mid = (low + safe) * 0.5f;
                if (covered(from + delta*mid)) safe = mid; else low = mid;
            }
            return from + delta*safe;
        }
        return to;
    }
};
