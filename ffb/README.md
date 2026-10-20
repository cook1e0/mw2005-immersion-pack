# NFS Most Wanted (2005) force feedback mod

Force feedback for `speed.exe` v1.3, written for direct drive wheels (developed on a Fanatec CSL DD):
steering weight from the front tyres, speed-sensitive centring, damping and friction computed by
the mod, sent as one torque through a safety chain that keeps a strong motor calm.

## Install

Download the zip from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest)
and copy `nfsmw_ffb.asi` and `nfsmw_ffb.ini` into the `scripts` folder next to `speed.exe` (with the
zip's `dinput8.dll`, Ultimate ASI Loader, next to `speed.exe`). Without a loader, rename
`nfsmw_ffb.asi` to `dinput8.dll` and put it and the `.ini` next to `speed.exe`. To uninstall, delete it.

In game, pick your wheel as the controller. The in-game force feedback option doesn't matter.

## First run

1. With the gameplay mod: F4 > Force feedback > **Test force**. The wheel pushes right for half a
   second. If it pushes left, turn on **Reverse force direction** (or set `ForceSign=-1`). If
   nothing moves, pick another **Wheel device** (some wheels, Fanatec's, show up twice).
2. Drive. Tune **Strength** first, then **Maximum force** for your base.

## What you feel

| Force | From |
|---|---|
| Steering weight (self-aligning torque) | the front axle's direction of travel vs. where the front wheels point (the tyres' slip angle), scaled by front tyre load; heaviest at `GripPeakAngle`, light past it (understeer) |
| Countersteer in a slide | the same: the front axle travels sideways, the wheel is pulled that way (`DriftAlign`) |
| Centring | a spring that grows with speed (`Centering`, `CenteringParked`, `CenteringSpeed`), and a firm hold around straight ahead once moving (`CentreWeight`) |
| Damping and friction | the wheel's own speed: weight against fast movement, scrub when parked; the friction is smooth through standstill, so a wheel held still doesn't buzz |
| Soft lock | the wheel past the gameplay mod's `[Controls] SteeringLock` |
| Menus, pause, cutscenes | a light centring spring (`MenuCentering`) |
| Engine | a vibration at the engine's firing frequency, strongest standing still (`EngineRumble`) |
| Kerbs and bumps | a front wheel's suspension compressing suddenly: a kick toward that side and a short shake (`Kerbs`) |
| Road surface | the game's own rumble for the surface under the front tyres (`SurfaceRumble`) |
| Crashes | the game's collision reports for the player car: cars, walls and props (benches, sign posts); a jolt sideways from the side hit, then a short shake (`Crashes`) |
| Wall scrapes | contact sliding along a wall: grinding and a pull toward it (`Scrapes`) |
| Off-road | the surface's own name under each front tyre (grass, dirt, mud, gravel, sand...): an uneven rumble with random bumps, half with one wheel off, growing with speed (`OffroadVibration`) |
| Nitrous | the steering goes lighter while the nitrous is on, eased in and out (`NosLighter`) |
| Breakable objects | the game's own breakable props (Smackables: benches, sign posts, fences, bins), recognised by their class in the collision report or when one is knocked loose where the car is: a sharp knock toward that side (`Breakables`) |

The tyres' slip angle uses the game's actual front wheel lock (with speed-sensitive steering only
a few degrees at speed), so the steering keeps getting heavier as you turn in instead of going
light halfway.

Next steps (not in this version yet): jump lightening and landings.

## How it works

```
 game physics tick (~60 Hz)                       FFB thread (UpdateHz, 400)
 telemetry ──snapshot──▶ model ──parts──▶ safety filters ──torque──▶ device ──▶ wheel
                           ▲                                            │
                           └────────── wheel position and speed ◀───────┘
```

- `src/telemetry.cpp`: hooks `LocalPlayer::DoFFB` (called every physics tick) and samples the car
  through `ISuspension`: wheel contact points and velocities give the car's frame (forward = rear
  axle to front axle, right = forward x world up; the world is right-handed and Z-up), speed, the
  front axle's sideways speed, yaw rate and front tyre load.
- `src/ffb_model.cpp`: the forces above, as pure functions (+ = turns the wheel right).
- `src/ffb_filters.h`: the safety chain's parts: a critically damped low-pass for the game-rate
  forces (`SmoothingHz`), a soft limiter that compresses peaks above `LimitKnee` instead of
  clipping, a slew-rate limiter (`SlewRate`), and an oscillation guard that spots the wheel shaking
  by itself and raises damping / lowers the gain until it settles (`OscillationGuard`).
- `src/ffb_engine.cpp`: the loop: telemetry and wheel, model, filters, `MaxForce`, a fade-in after
  every (re)start, a final clamp and a guard against invalid numbers.
- `src/ffb_device.cpp`: the wheel as an output: one constant-force effect, nothing else (the
  wheel's own spring / damper / friction effects differ between drivers; a Fanatec base's spring
  was measured pushing away from centre). Every change is sent with `DIEP_START`
  (`RestartEffects`): a Fanatec base ignores changes to a playing effect otherwise. Device choice
  by Windows' order (`Device`), acquisition, recovery after Alt-Tab, lost devices, stopped
  effects and failed operations, hardware safety switches respected, F10 / F4 restart.
- `src/dinput_wrap.cpp`: wraps the game's DirectInput: the game's own effects get a no-op object,
  and with the gameplay mod's `[Controls] GameController=0` controllers read as untouched (the
  gameplay mod drives the car).
- `tools/ffbtool.exe`: `ffbtool` lists controllers; `ffbtool test [n]` measures FFB device `n`
  with the wheel left alone: each effect type on its own and constant force sent several ways.
  It uses Windows' own DirectInput, so it also works from the game folder.

## Building

MSYS2 with the mingw32 toolchain: `./build.sh` (`./build.sh install` copies into `C:\Games\NFSMW`,
or `$NFSMW_DIR`).

Tests (mingw32): `./tests/run.sh`. The input tests check menu transitions, buffered events and
controller suppression with a synthetic game layout and fake DirectInput device. The FFB tests
check the filters, the model's signs and shapes (centring both ways, countersteer in a slide,
lightening past the grip limit, parked, airborne, soft lock, menus), the engine's safety chain
(never above `MaxForce`, never faster than `SlewRate`, invalid numbers, fade-in, oscillation) and
the device layer against a fake driver that, like the measured Fanatec base, ignores changes
without `DIEP_START`. None of them touch a physical wheel.
