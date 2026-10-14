// Driving controls ([Controls]) fed to the game as its own input actions.
#pragma once

void Controls_Install();

// Speed-sensitive wheel steering: the share of the full front wheel lock (45 degrees) that the
// steering lock gives at `kmh`. One smooth curve instead of the game's table (45 degrees parked,
// 15 at 64 km/h, then kinks down to 7): full lock parked, falling off with speed toward
// 1 - sensitivity. At the default 0.85 it stays close to the game's own values (about 19 degrees
// at 64 km/h, 12 at 130, 9 at 250), without the kinks. The wheel maps linearly onto it.
inline float SteeringLockShare(float kmh, float sensitivity) {
    const float kRef = 40.0f;   // km/h where half the reduction is reached
    float x = kmh > 0.0f ? kmh / kRef : 0.0f;
    float falloff = 1.0f / (1.0f + x * __builtin_sqrtf(x));   // 1 / (1 + x^1.5)
    float s = sensitivity < 0.0f ? 0.0f : (sensitivity > 0.95f ? 0.95f : sensitivity);
    return (1.0f - s) + s * falloff;
}
