#pragma once
#include "vrmath.h"

// Fixed 12-second test sequence, shared by FakeHmd and the camera tests.
// Sweeps, step flicks, then a yaw flick while leaning 30 degrees.
inline Quat Spectator_TestPose(double seconds) {
    float t = (float)fmod(seconds, 12.0), yaw = 0, pitch = 0, roll = 0;
    if (t < 3) yaw = 45 * sinf(t * 2.0943951f);
    else if (t < 6) pitch = 40 * sinf((t-3) * 2.0943951f);
    else if (t < 8) yaw = t < 6.5f ? 0 : t < 7.0f ? 85.0f : 0;
    else if (t < 10) pitch = t < 8.5f ? 0 : t < 9.0f ? 65.0f : 0;
    else { roll = 30 * sinf((t-10) * 1.5707963f); yaw = t < 10.5f ? 0 : t < 11.0f ? 90.0f : 0; }
    const float deg = 3.14159265f / 180;
    return QMul(QMul(QFromAxisAngle({0,1,0},yaw*deg), QFromAxisAngle({1,0,0},pitch*deg)),
                QFromAxisAngle({0,0,1},roll*deg));
}
