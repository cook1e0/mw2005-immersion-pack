# Grip loss in the tyre feel (research and design)

Research into adding a grip-loss feel to the self-aligning-torque (SAT) part of the FFB mod, without
changing the parts that already feel right. The short version:

- The mod already has a brush-style SAT falloff, but it is driven by a **geometric** slip angle
  (steering angle minus the front axle's travel direction) and a fixed `GripPeakAngle`.
- The game exposes the tyres' **actual** slip angle and a per-tyre **traction/grip scale**. Both are
  readable through the existing `ISuspension` interface and are cheap to sample.
- Use `GetWheelTraction()` to make the wheel go light as the tyre passes its limit. The game's slip
  angle is a second, optional accuracy step. Keep the geometric estimate as a fallback and make the
  whole thing a single `GripLoss` setting; `GripLoss=0` with `TireSlipSource=0` reproduces today's
  feel exactly.
- Add understeer/oversteer and tyre-scrub cues later, behind their own settings, after a short
  telemetry probe has confirmed signs and units in-game.

The rest of this document is the evidence, the proposed model, and a phased implementation plan.

> **Status: implemented (Phases 0–3).** `GripLoss` (default 0.5) uses `GetWheelTraction` to lighten
> the wheel past the front tyres' limit; `RearGripLoss` (default 0.5) does the same for the rear so
> a rear-led slide is felt in a car whose front stays gripped, and `TyreScrub` (default 0.2) buzzes
> when any tyre is past its limit. `UndersteerFeel` stays off and `TireSlipSource=0` keeps the
> geometric slip shape. The game's slip angle is used for its **magnitude** only; the proven
> geometric sign is kept, so a wrong raw sign cannot invert the force. `GripLoss=0` with
> `TireSlipSource=0` reproduces the old model exactly. The `tyre:` telemetry log line (with
> `LogTelemetry=1`) is the in-game probe; it showed the front tyres at 1.00 while the rears dropped
> to 0.28–0.50 on a test car with 1.25 front / 0.85 rear grip, which is why the rear cue is on by
> default.

---

## 1. What "grip loss" should feel like

A real tyre's self-aligning torque is lateral force times pneumatic trail:

```
Mz = Fy * (t + tm)
```

- `Fy` rises with slip angle, peaks, then plateaus or falls slowly.
- The pneumatic trail `t` peaks at a small slip angle and collapses toward zero at the limit.
- So `Mz` peaks **before** peak lateral force and falls toward zero as the tyre starts to slide.
  That early peak and fall is the "the front is about to let go" feeling in the wheel.

A good game FFB model should therefore:

1. Build weight as slip angle rises.
2. Peak slightly before the car actually loses grip.
3. Fall away smoothly, not cliff-edge, as the tyre goes past its limit.
4. Keep a small directional pull while sliding (so the wheel is light, not dead).
5. Ideally distinguish front breakaway (understeer) from rear breakaway (oversteer).

The current model does 1, 3 (partly), and 4. It approximates 2 with a fixed peak angle. It cannot
do 5 because it only looks at the front axle's travel direction.

---

## 2. What the mod does today

`src/ffb_model.cpp`, `Model::Compute`:

```cpp
float x = FrontSlipAngle(t, w, p, roadLock_) / (gripPeakDeg * pi / 180);
float shape = x * expf(0.5f * (1.0f - x * x)) + driftAlign * tanhf(0.5f * x);
f.aligning = -aligning * shape * load * speedGain * drive;
```

`FrontSlipAngle` is:

```cpp
steer = wheel share * road lock
travel = atan2(vLatFront, max(|vLong|, 1))
return steer - travel
```

Strengths:

- It is pure and testable; the tests already check signs, peak and falloff.
- It uses the game's actual steering lock (`maxSteerDeg`), so the steering weight keeps building
  with speed-sensitive steering instead of going light halfway.
- `x * exp(0.5(1 - x^2))` has the right qualitative shape: 0 at 0, peak 1 at `x = 1`, then falls.
- `DriftAlign * tanh(0.5x)` keeps a pull toward the travel direction in a slide, which produces the
  countersteer cue.

Limits for grip loss specifically:

- **Geometric slip is not tyre slip.** `steer - atan2(vLatFront, |vLong|)` includes body slip and
  the car's yaw geometry. It is a good proxy, but it cannot tell how close each tyre is to its own
  friction limit.
- **`GripPeakAngle` is a single tuning for all cars and tyres.** The real peak moves with load,
  temperature, surface and tyre type.
- **The falloff is shape-only.** Two different grip states with the same geometric slip produce the
  same force, even if one tyre is gripping and the other is sliding.
- **Front axle only.** There is no rear grip state, so oversteer is inferred indirectly from where
  the front axle travels.
- **No "light before the limit" cue.** The shape only starts falling after `x = 1`; real SAT starts
  falling earlier relative to lateral force.

The last point matters: adding a true grip scale lets the wheel lighten **before** the geometric
brush curve would, which is the sensation the user is asking for.

---

## 3. Signals available from the game

Everything below is in `speed.exe` v1.3, through the `ISuspension` interface that `telemetry.cpp`
already queries. The player's suspension vtable is `0x8ABAC8` (`VT_SuspensionRacer_ISuspension`).
The concrete `SuspensionRacer` keeps its four `Tire` objects at `+0x168`, `[0]`/`[1]` front,
`[2]`/`[3]` rear.

Verified slots (addresses are the functions in the local `speed.exe`):

| Slot | Address | Function | Returns | Backing field | Confidence |
|---|---|---|---|---|---|
| `+0x04` | `0x6A94B0` | `GetWheelTraction` | per-tyre traction scale | `Tire +0x110` | high |
| `+0x18` | `0x6A9550` | `GetWheelLoad` | wheel load, N | `Tire +0xD4` | high (already used) |
| `+0x30` | `0x6A94F0` | `GetWheelSlip` | longitudinal slip (ratio?) | `Tire +0xF0` | low, probe |
| `+0x34` | `0x6A9510` | `GetToleratedSlip` | tolerated slip (same units as above?) | `Tire +0x124` | low, probe |
| `+0x38` | `0x6A9530` | `GetWheelSkid` | lateral skid (m/s?) | `Tire +0x130` | medium |
| `+0x3C` | `0x6A9710` | `GetWheelSlipAngle` | tyre slip angle, **turns** | `Tire +0x118` | high |
| `+0x50` | `0x68E570` | `GetWheelAngularVelocity` | wheel angular speed, rad/s | computes from `+0x110` etc. | medium |
| `+0x5C` | `0x68D340` | `CalculateUndersteerFactor` | 0..1 | front skid / steer | high |
| `+0x60` | `0x68D540` | `CalculateOversteerFactor` | 0..1 | rear skid / speed | high |
| `+0x68` | `0x6A94D0` | `GetWheelRadius` | wheel radius, m | `Tire +0xC4` | high |

### 3.1 `GetWheelTraction` — the key new signal

`+0x04` is a one-line getter:

```asm
mov eax, [esp+4]
mov ecx, [ecx + eax*4 + 0x168]   ; the tyre
fld dword ptr [ecx + 0x110]
ret 4
```

`GetWheelAngularVelocity` (`+0x50`, `0x68E570`) compares `Tire +0x110` with `1.0`:

- `>= 1.0` → use the stored angular velocity at `+0xD0`;
- `< 1.0` → the tyre is sliding; derive angular velocity from `+0xFC / radius`.

That is a strong indication that `+0x110` is a **grip scale**, `1.0` = full grip, lower = the tyre
is over its friction limit. Disassembly of the tyre force step shows the field set to `1.0`, then to
`available grip / demanded grip` when demand exceeds what the tyre can supply; the probe below should
confirm it. Treat it as:

```
traction = clamp(GetWheelTraction(i), 0, 1)
```

and clamp anyway; do not assume it never exceeds 1 or goes negative.

**Caveat:** it is a *force-demand* ratio, not a pure lateral-grip meter. Hard braking or a locked
wheel can lower it even in a straight line (combined slip). The proposed model multiplies it into
the SAT shape, which is already near zero at zero slip, so straight-line braking has little effect.
Still, probe it and keep the default conservative.

### 3.2 `GetWheelSlipAngle` — better slip input

`+0x3C` returns `Tire +0x118`, which the gameplay mod already reads in `controls.cpp`:

```cpp
constexpr int kTire_SlipAngle = 0x118;
float slip = 0.5f * (|rearLeft.slip| + |rearRight.slip|) * k;
```

`k` is the game's factor at `0x890960`, and its value is **360.0**. So `Tire +0x118` is an angle in
**turns** (revolutions), not radians. The gameplay mod multiplies by 360 to get degrees and clamps
to `< 90`.

For FFB:

```cpp
float slipRad = GetWheelSlipAngle(i) * 2.0f * pi;   // turns -> radians
```

or degrees with `* 360.0f`.

Sign convention is not yet confirmed against the wheel. The geometric `FrontSlipAngle` is positive
when the wheels point right of the travel direction. Probe a steady right turn; if the game's value
is negative there, negate it once at sampling time. Because the two front tyres have different
Ackermann angles, average the grounded front wheels, and keep the sign.

This is the more accurate signal for the SAT, but the geometric angle stays the safe default until
the probe confirms the sign and units. It includes the effects of load, camber and the game's tyre
model that the geometric estimate cannot see.

### 3.3 `GetWheelSkid` and the understeer/oversteer factors

`+0x38` returns `Tire +0x130`. The game's own factor functions use it:

`CalculateUndersteerFactor` (`+0x5C`, `0x68D340`):

- averages `GetWheelSkid(0)` and `(1)` (front);
- averages `GetWheelSteer(0)` and `(1)`;
- forms a ratio, clamps the result to at most `1.0`;
- falls back to `0.5 * front steer` when its preconditions are not met.

`CalculateOversteerFactor` (`+0x60`, `0x68D540`):

- averages `GetWheelSkid(2)` and `(3)` (rear);
- divides by speed (the car's local forward velocity);
- clamps the result to at most `1.0`.

So both are `0..1` cues, already computed by the game, and they are cheap to call. But:

- the understeer factor's fallback can be non-zero while gripping, so gate it by speed and by a
  minimum front slip;
- their internal scale is tuned for the game's own handling aids, not for FFB;
- using them **and** the raw slip angle risks double-counting understeer.

Recommendation: do not use them in the first implementation. Add `UndersteerFeel` as an optional
second-stage control, default `0`, and only after the probe shows they behave sensibly.

### 3.4 Units and validity

- `GetWheelLoad` is already used and summed; keep it.
- `GetWheelSlip` / `GetToleratedSlip` are likely a longitudinal slip ratio and its limit. Do **not**
  use them for lateral grip until the probe shows their range. They may be useful later for a
  "wheel locked / spinning" cue.
- Only sample a wheel when it is on the ground. If both front wheels are airborne, there is no SAT
  anyway; if one is airborne, use the grounded one.
- Only call the enhanced slots when `*(uintptr_t*)susp == 0x8ABAC8`. Other suspension classes may not
  implement them, and the existing code already uses this guard for the tyre surface.
- Sample on the game thread inside `Sample()`; never call the game from the FFB thread.

---

## 4. Proposed model

Three layers, each independently tunable. Layer 1 is the core; layers 2 and 3 are optional.

### 4.1 Layer 1 — true slip angle + traction-scaled SAT

Add to `Telemetry`:

```cpp
float tireGrip[4];        // GetWheelTraction, 0..1
float tireSlipAngle[4];   // GetWheelSlipAngle, radians (converted from turns)
float tireSkid[4];        // GetWheelSkid, raw (only for the probe / later)
bool  tireStateValid;     // false when the vtable does not match or data is implausible
```

Add to `ModelParams`:

```cpp
float gripLoss = 0.5f;    // 0 = no traction lightening, 1 = strong
float rearGripLoss = 0.5f;// the same for the rear tyres (a rear-led slide)
int   tireSlipSource = 0; // 0 = geometric slip, 1 = the game's tyre slip angle
```

Helper in `ffb_model.cpp`:

```cpp
// The least grip of the grounded wheels; 1 when unknown (so the model stays exactly as it was).
float FrontGrip(const Telemetry& t);
float TireSlip(const Telemetry& t, const WheelState& w, const ModelParams& p, float roadLock);
```

`TireSlip` returns the game slip angle when `t.tireStateValid && t.frontOnGround > 0 &&
p.tireSlipSource == 1`, otherwise `FrontSlipAngle(...)`. `FrontGrip` returns the **least** `tireGrip`
of the grounded front wheels, or `1.0` when unknown: the wheel nearest its limit decides where the
axle lets go, while an average would hide it behind the still-gripping inside wheel.

Then:

```cpp
float slip = TireSlip(t, w, p, roadLock_);
float grip = FrontGrip(t);                       // 1 = gripping, 0 = fully sliding
float over = Clampf(1.0f - grip, 0.0f, 1.0f);    // 0 = no loss
float g    = Clampf(p.gripLoss, 0.0f, 1.0f) * over;

// The limit arrives earlier and the whole force is lower.
float peak = Clampf(p.gripPeakDeg * (1.0f - 0.35f * g), 1.0f, 30.0f);
float x    = slip / (peak * kPi / 180.0f);
float shape = x * expf(0.5f * (1.0f - x * x)) + p.driftAlign * tanhf(0.5f * x);
float amp   = 1.0f - 0.55f * g;

f.aligning = -p.aligning * shape * amp * load * speedGain * drive;
```

Properties:

- `gripLoss = 0` → `g = 0` → `peak` and `amp` unchanged → no traction lightening. The slip input is
  still whatever `TireSlipSource` selects, so this is only "today's model exactly" when the source
  is also `0`.
- `tireStateValid = false` → `grip = 1` → no traction lightening (geometry fallback).
- `grip = 1` (full grip) → no traction lightening, even with `gripLoss = 1`.
- `TireSlipSource = 0` with `GripLoss = 0` → **identical to today's model**.
- As `grip` falls, the SAT peak moves to a smaller slip angle and the force scale drops. The wheel
  goes light progressively, before the geometric curve alone would.
- At `gripLoss = 0.5` and a fully sliding tyre (`grip = 0`), the peak is 17.5% earlier and the force
  27.5% lower. Noticeable without making the wheel dead. At `gripLoss = 1` the effect is large
  enough to feel like a different car, which is useful for tuning.
- The front term is gated by the front slip angle (`Smoothstep(0.1, 0.5, |slip| / GripPeakAngle)`),
  so hard straight-line braking (which also lowers the traction ratio) does not lighten the wheel
  by itself. The rear term has no such gate: a rear slide or burnout should lighten the steering.
- `TireSlipSource=1` takes only the game slip angle's **magnitude**; the sign comes from the proven
  geometric `FrontSlipAngle`, so an unknown raw sign cannot invert the force.

Why warp the peak **and** scale the force, instead of only scaling:

- Scaling alone reads as "the wheel is weaker", not "the tyre has reached its limit".
- Moving the peak earlier reads as "the limit arrives sooner", which is the physical cue.
- Doing both gives a clear breakaway without making the wheel dead; `DriftAlign` still provides the
  sliding pull.

Tuning notes:

- If the wheel feels **dead at the limit**, lower `GripLoss` or raise `DriftAlign` slightly.
- If the drop is **too sudden**, lower `GripLoss`; the 25 Hz low-pass already softens it, and the
  `SlewRate` limiter prevents a hard step.
- If the wheel **never feels light enough**, raise `GripLoss` before touching `Aligning`. `Aligning`
  changes the whole curve; `GripLoss` only changes the past-the-limit part.

### 4.2 Layer 2 — optional understeer / rear grip cues

Understeer:

```cpp
if (p.understeerFeel > 0.0f && t.understeer > 0.0f)
    amp *= 1.0f - Clampf(p.understeerFeel, 0.0f, 1.0f) * t.understeer;
amp = fmaxf(amp, 0.25f);   // never fully dead
```

Only when speed is above ~5 m/s and front slip is above the peak; otherwise the factor's fallback
can lighten the wheel while parking or driving straight. Default `0`.

Rear grip / oversteer:

- `rearGrip = least traction of the grounded rear wheels`.
- `oversteer = CalculateOversteerFactor()` (0..1), used only as an optional extra.
- A rear slide should **not** simply lighten the front SAT: in a real countersteer the front tyres
  are doing a lot of work, and the current geometric model already pulls the wheel into the slide.
  The in-game probe settled this: on a car with 1.25 front / 0.85 rear grip the front traction stayed
  at 1.00 in normal cornering while the rear dropped to 0.28–0.50. A front-only cue therefore had
  nothing to react to. The rear term is what gives that car a felt grip loss.
- `RearGripLoss`: multiply `amp` by `1 - RearGripLoss * (1 - rearGrip)`, default `0.5`. It lightens
  the wheel when the rear tyres are past their limit. It can weaken the countersteer cue in a clean
  rear breakaway, so keep it tunable and set it to `0` for the old feel.
- `OversteerKick`: a short torque in the direction the rear is sliding, with its own attack and
  decay, added as an effect. This needs a sign probe and is easy to get wrong; leave it out of the
  first implementation.

### 4.3 Layer 3 — optional tyre scrub

A vibration whose amplitude grows as any tyre passes its limit:

```cpp
// In Effects::Step, once per update.
float grip = least traction of the grounded wheels;   // 1 = all gripping
float over = 1.0f - grip;
float level = p.tyreScrub * Ramp(0.25f, 0.9f, over) * Ramp(3.0f, 15.0f, t.speed);
phase += 2*pi * (28.0f + 0.4f * t.speed) * dt;   // ~28-45 Hz
parts.scrub = level * sinf((float)phase);
```

Rules:

- Default `TyreScrub = 0.2`. It is the most intrusive cue, so keep it subtle (`0.05-0.3`). A strong
  scrub masks the force drop and can be confused with a mechanical problem. `0` turns it off.
- Use all four tyres, not just the front: a rear slide or burnout is exactly when the wheel should
  buzz, and the front may still be gripping.
- Include `scrub` in `EffectParts::Sum()` and therefore in `fx`, so the oscillation guard ignores
  the wheel while it is deliberately shaking. This is the same treatment as the other effects.
- Do not run it while airborne, in menus, or below walking pace.

---

## 5. Configuration and UI

New `[FFB]` keys, with the shipped defaults:

```ini
; How much the front tyres' own grip state lightens the steering past the limit.
; 0 = the old geometric feel only; 0.5 = noticeable; 1 = strong.
GripLoss=0.5
; Extra lightening from the game's understeer factor (0 = off)
UndersteerFeel=0
; How much the rear tyres' own grip state lightens the steering past their limit.
; The cue to raise first in a rear-wheel-drive car that oversteers (0 = off).
RearGripLoss=0.5
; Vibration while any tyre slides (0 = off; 0.05-0.3)
TyreScrub=0.2
; 0 = current geometric slip angle (safe), 1 = the game's tyre slip angle with fallback
TireSlipSource=0
```

Config plumbing:

- `common.h`: add the four floats and the source flag.
- `common.cpp`: parse with `ReadFloat` / `ReadInt`, clamp:
  - `GripLoss` 0..1
  - `UndersteerFeel` 0..1
  - `RearGripLoss` 0..1
  - `TyreScrub` 0..0.5
  - `TireSlipSource` 0..1
  - add them to the config log line.
- `dllmain.cpp`: add them to `NfsmwFfb_Setting` so the gameplay mod's F4 menu can change them live.
- `ui.cpp`: add entries under `STEERING FEEL` in **both** menu tables (the normal and the VR one):
  - "Front grip loss" → `GripLoss`, 0..1, step 0.05, `%.0f%%`
  - "Understeer feel" → `UndersteerFeel`, 0..1
  - "Rear grip loss" → `RearGripLoss`, 0..1 (also on the main F4 Force feedback page)
  - "Tyre scrub" → `TyreScrub`, 0..0.5
  - The enum near `FFB_ALIGNING` and the `static_assert(KeyIs(...))` ordering need the new entries.
- `nfsmw_ffb.ini`: document the keys as above, near `Aligning` / `DriftAlign`.
- `README.md`: add a row to "What you feel" and a sentence to the SAT paragraph.

Default behaviour:

- `GripLoss=0.5`, `RearGripLoss=0.5`, `TyreScrub=0.2`, `UndersteerFeel=0`, `TireSlipSource=0`.
  This keeps the proven geometric SAT shape and adds the traction-driven lightening from the front
  and the rear. `TireSlipSource=1` can be tried as a separate step.
- Setting `GripLoss=0`, `RearGripLoss=0`, `TyreScrub=0` **and** `TireSlipSource=0` gives the old
  model exactly, which is the escape
  hatch. Either one alone changes only its own part.

---

## 6. Telemetry probe (do this first)

Before changing the force, add a temporary log behind the existing `LogTelemetry` setting. Sample
the new values in `telemetry.cpp` and log at ~10 Hz. In the snippet, `g[]`, `s[]` and `k[]` are the
per-wheel traction, slip angle (still in turns) and skid arrays:

```cpp
if (g_cfg.logTelemetry && t.time - lastTireLog > 0.1) {
    lastTireLog = t.time;
    Log("tyre: FL grip=%.3f slip=%.2f skid=%.2f | FR grip=%.3f slip=%.2f skid=%.2f | "
        "RL grip=%.3f slip=%.2f | RR grip=%.3f slip=%.2f | U=%.2f O=%.2f steer=%.1f",
        g[0], s[0] * 360.0f, k[0], g[1], s[1] * 360.0f, k[1],
        g[2], s[2] * 360.0f, g[3], s[3] * 360.0f, understeer, oversteer, t.steerDeg);
}
```

Drive a fixed route and check:

| Situation | Expected |
|---|---|
| Straight line, steady speed | front grip ≈ 1.0, slip ≈ 0, understeer ≈ 0 |
| Gentle right turn | front slip positive if the sign matches `FrontSlipAngle`; grip ≈ 1.0 |
| Turn-in to understeer | front grip falls below 1; front slip keeps growing; understeer rises |
| Handbrake / power oversteer | rear grip falls; oversteer > 0 |
| Wheelspin launch | rear grip falls, front grip stays near 1 |
| Hard braking in a straight line | front grip may dip; note how much |
| Front wheels airborne | grip value is meaningless; note it and gate it out |
| Reverse | slip sign flips; make sure the model still pulls the right way |

What to confirm:

1. **Sign**: does `GetWheelSlipAngle` have the same sign as `FrontSlipAngle`? If not, negate at
   sampling time.
2. **Units**: `*360` should give plausible degrees (a few degrees in a turn, tens in a slide). If
   not, the field is not turns and the conversion must change.
3. **Range**: is `GetWheelTraction` in `0..1`? Does it stay at 1 in a straight line? How far does it
   fall in a slide?
4. **Timing**: does traction fall before, with, or after the geometric slip peak? This decides
   whether the extra lightening leads or follows the shape.
5. **Combined slip**: does traction fall under braking/acceleration without lateral slip? If it does,
   gate the grip term by `|slip| > small threshold` or by `frontOnGround == 2 && speed > 5`.

Only after these are known should `GripLoss` be tuned in-game.

---

## 7. Implementation plan

### Phase 0 — probe only

- `telemetry.h`: add `tireGrip[4]`, `tireSlipAngle[4]`, `tireSkid[4]`, `understeer`, `oversteer`,
  `tireStateValid`.
- `telemetry.cpp`: add the slots to the `ISuspension` enum; sample them under the vtable guard;
  clamp; log at 10 Hz.
- No model changes. Build, drive, collect logs, confirm signs/units/ranges.

### Phase 1 — core grip loss

- `telemetry.*`: keep the new fields; remove or quiet the probe log.
- `ffb_model.h`: add `gripLoss`; add `FrontGrip` / `TireSlip` helpers (private or in the anonymous
  namespace).
- `ffb_model.cpp`: add the game-slip path and the traction-scaled shape from section 4.1; default
  `TireSlipSource` to `0` so the proven geometric curve is what ships first.
- `ffb_engine.cpp`: copy `gripLoss` and `tireSlipSource` into `ModelParams`.
- `common.*`, `dllmain.cpp`, `nfsmw_ffb.ini`, `ui.cpp`: add `GripLoss` and `TireSlipSource`.
- `tests/ffb_tests.cpp`: see section 8.
- Build, run the tests, then drive. Tune `GripLoss` and, if needed, `GripPeakAngle`.

### Phase 2 — optional cues

- Add `UndersteerFeel` and the understeer factor, default `0`.
- Add `RearGripLoss` and the rear traction (least grounded wheel), default `0.5`; the in-game probe
  showed the rear is the tyre that actually lets go in an oversteer-prone car.
- Add `TyreScrub` as an effect, default `0.2`; wire it into `EffectParts::Sum()` and `fx`. It reads
  the least grip of all four grounded tyres, so a rear slide triggers it too.

### Phase 3 — polish

- Update `README.md` and the F4 descriptions.
- Add the new keys to the config log line.
- Add the tests below; run `./tests/run.sh`.
- If the drop feels too sharp on a direct drive wheel, consider a dedicated attack/release filter
  for `grip` (fast attack, slower release) instead of changing the global `SmoothingHz`.

---

## 8. Tests to add

Pure model tests (no game, no device):

1. **Backward compatibility**: with `gripLoss = 0` and `tireSlipSource = 0`, `Model::Compute`
   returns the same aligning force as before for a table of telemetry/wheel states.
2. **Unknown tyre state**: with `tireStateValid = false`, the result equals the geometric model even
   with `gripLoss = 1`.
3. **Full grip**: with `tireStateValid = true` and all `tireGrip = 1`, the result equals the
   geometric model for the same slip angle.
4. **Grip loss lightens**: at a fixed slip angle past the peak, lowering front `tireGrip` lowers
   `|aligning|`.
5. **Grip loss moves the peak**: with low `tireGrip`, the maximum aligning force occurs at a smaller
   slip angle than with full grip.
6. **Airborne**: with `frontOnGround = 0`, aligning is zero regardless of `tireGrip`.
7. **Clamping**: `gripLoss` outside 0..1, `tireGrip` outside 0..1, and NaN inputs do not produce
   non-finite forces.
8. **Understeer** (if added): `understeerFeel = 0` is a no-op; positive values only lighten when
   speed and slip gates are met.
9. **Scrub** (if added): zero at standstill and at full grip; non-zero only when moving and sliding;
   included in `EffectParts::Sum()`.

Engine tests:

10. The new forces still respect `MaxForce`, `SlewRate`, the soft limiter and the fade-in.
11. The oscillation guard still sees the scrub effect in `fx` and does not fight it.

---

## 9. Risks and open questions

- **`GetWheelTraction` is a force-demand ratio.** It can drop under braking or wheelspin, not only
  lateral sliding. The slip-shaped multiplication limits the damage, but the probe must check
  straight-line braking. If it is too intrusive, gate the grip term by `|slip| > 0.5 * peak`.
- **Double counting.** The geometric brush curve already lightens past the peak; adding traction
  lightening on top can make the wheel feel dead. Keep `GripLoss` around `0.5` and tune `DriftAlign`
  if the slide needs more life.
- **Sign and units of `GetWheelSlipAngle`.** High confidence that it is turns (`*360` matches the
  gameplay mod's use), but the sign is unconfirmed. Probe.
- **Understeer factor fallback.** It returns `0.5 * front steer` when its preconditions fail, which
  can be non-zero while gripping. Gate it or leave it off.
- **Chatter.** Traction can flicker at the limit at 60 Hz. The existing 25 Hz low-pass on `aligning`
  plus the slew limiter should absorb it; if not, add a fast-attack/slow-release filter on `grip`.
- **Other suspension classes.** Guard every enhanced call with the `0x8ABAC8` vtable check.
- **Performance.** Four traction calls, four slip-angle calls and two factor calls per physics tick
  are negligible compared with the existing per-wheel position/velocity/load/compression calls.

---

## 10. Recommendation

All phases are implemented; the shipped starting point is:

```ini
GripLoss=0.5
TireSlipSource=0
UndersteerFeel=0
RearGripLoss=0.5
TyreScrub=0.2
```

That adds a game-state-driven grip-loss feel without changing the proven geometric SAT shape.
`TireSlipSource=1` can be tested separately once the traction cue is accepted, and the optional
understeer/rear/scrub cues can follow if the core feel is not enough.

---

## Sources

Game data and code:

- Local `speed.exe` v1.3, `ISuspension` vtable `0x8ABAC8`; slot getters disassembled and checked
  against the mod's existing use of `Tire +0x118` in `gameplay/src/controls.cpp`.
- `nfsmw05-mod/ffb/src/telemetry.cpp`, `ffb_model.cpp`, `ffb_engine.cpp`, `common.*`, `ui.cpp`.
- NFSPluginSDK `ISuspension.h`:
  <https://github.com/berkayylmao/NFSPluginSDK/blob/master/NFSPluginSDK/Game.MW05/Types/ISuspension.h>
- NFSMW 2005 SDK: <https://github.com/s-b-repo/nfsmw-2005-sdk>

Tyre and FFB design references:

- Pneumatic trail and its falloff at the limit:
  <https://www.griggsracing.com/glossary/pneumatic-trail/>
- MAIRA steering effects (understeer/oversteer from yaw-rate error, seat-of-pants from lateral
  acceleration, optional vibration):
  <https://mairapp.com/home/documentation/advanced/steering-effects/>
- rFactor FFB tuning (`FFB steer force grip function/weight/factor`, `front grip fract`):
  <https://community.racesimcentral.net/threads/rfactor-better-ffb-tutorial.2281/page-3>
- irFFB (telemetry-driven understeer and seat-of-pants effects on top of steering torque):
  <https://github.com/nlp80/irFFB>
