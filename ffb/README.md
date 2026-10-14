# NFS Most Wanted (2005) force feedback mod

Physics-based steering force feedback for `speed.exe` v1.3, driven from the game's own tyre model
instead of its weak built-in effects.

## Install

Download the zip from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest)
and copy `nfsmw_ffb.asi` and `nfsmw_ffb.ini` into the `scripts` folder next to `speed.exe` (with the
zip's `dinput8.dll`, Ultimate ASI Loader, next to `speed.exe`). Without a loader, rename
`nfsmw_ffb.asi` to `dinput8.dll` and put it and the `.ini` next to `speed.exe`. To uninstall, delete it.

In game, pick your wheel as the controller. The in-game force feedback option doesn't matter.

## What you feel

| Force | Source in the game |
|---|---|
| Steering weight (self-aligning torque) | front tyre slip angle (`SuspensionRacer::Tire` +0x118) and load, shaped like a real tyre: builds up to `SlipAnglePeak`, then goes light as the front loses grip |
| Pull toward the direction of travel while sliding | same slip angle, `DriftAlign` |
| Bumps, kerbs, landings | front suspension compression rate, left vs. right |
| Impact jolts | sudden body velocity change (wheel contact point velocities) |
| Damper / friction / parking scrub | vehicle speed |
| Engine buzz, brake lock | `IEngine::GetRPM`, tyre brake-lock flag |
| Centring spring in menus, pause, cutscenes | `LocalPlayer::CanDoFFB` (pause / AI / NIS) |
| Centring spring while driving (`CenterSpring`) | grows with speed: `CenterSpringParked` of it at standstill, all of it from `CenterSpringSpeed` km/h |
| Soft lock (`SoftLock`) | wheel position past the gameplay mod's `[Controls] SteeringLock` |
| Speed centring (`SpeedCentering`): `-K * v * sin(angle)` | car speed and the wheel's angle as a share of the steering lock |
| Understeer lightening (`UndersteerLightening`) | front slip past the peak, or yaw rate below what the front wheel angle and wheelbase ask for |
| Surface rumble (`SurfaceRumble`) | the tyre's road surface (`simsurface` WheelEffectFrequency / Intensity): grass, dirt, gravel |
| Kerbs (`Curbs`, `CurbJerk`) | jerk of the body's vertical motion in its own frame (slopes and crests stay below `CurbJerk`), side from the front suspension |
| Impacts (`Collisions`) | head-on: a shake both ways; side and angled: a one-way kick (direction of the velocity change) |
| Wall scrapes (`Scrapes`) | acceleration spikes above the smoothed acceleration: pull toward the wall and a fast rumble |
| Airborne and landings (`AirborneForce`, `Landing`) | no wheel on the ground: steering forces drop; touching down: a jolt from the downward speed |

## First run: automatic calibration

The force direction convention differs between wheel drivers, and the sign of the game's
tyre slip angle can't be read from the binary. The mod measures both against your wheel's
position and saves the results in `nfsmw_ffb.ini` `[Calibration]`:

1. **ForceSign**: on the first menu screen the wheel gets two short gentle nudges (right, then left).
   Keep your hands off the wheel for a moment. Retries automatically if you were holding it.
2. **SlipSign / RightSign**: during the first corners above ~45 km/h. Until then, steering weight
   comes from a speed-dependent centring spring instead of the tyre model.

Watch `nfsmw_ffb.log` for `calibration:` lines. Set the values back to `0` to re-detect.

The direction test measures each push against the wheel's drift just before it. Some drivers ignore
negative force magnitudes: when a wheel reacts to pushes one way only, the mod switches to giving
the side as the effect's direction (`ForceDirection=1`) and tests again. `ffbtool test [n]` tries
both on FFB device `n` (some wheels, e.g. Fanatec, show up as two devices of the same name).

## Recovery and restart

After Alt-Tab, the mod reacquires wheels that were previously working and restores their
effects automatically. It also checks for stopped or unloaded effects twice per second,
including while the gameplay mod's F4 menu is open. Failed starts and parameter updates are
retried with a delay. Forces stop while the game is unfocused and return gradually after recovery.

If a driver still gets stuck, press **F10**, or use **F4 > Force Feedback > Restart wheel force
feedback** with the updated gameplay mod. This reacquires the wheel and rebuilds the mod's effects;
your saved calibration and tuning are kept. `[FFB] KeyReset` changes the hotkey (`0` disables it).
The restart option requires this mod's effects (`NativeFFB=0`); it does not reset the game's own
effects in native mode. Recovery leaves physical power and safety switches under the wheel's control.

Check `nfsmw_ffb.log` for `recovery scheduled`, `wheel effects recovered` and `manual restart` lines.

## Tuning

Everything is in `nfsmw_ffb.ini` (commented). Start with `Gain`, then `SelfAligningTorque`.
If the log reports clipping, lower `Gain`. For gear-driven wheels with a deadzone (G25/G27/G29/G920)
try `MinForce=0.05`. `LogTelemetry=1` writes physics values every 0.1 s.

## How it works

- `src/dinput_wrap.cpp`: wraps `IDirectInput8A` and every force-feedback device the game opens.
  It forces exclusive mode (required for FFB), turns the wheel's autocentre off, and hands the
  game's own effects a no-op object so they don't fight ours.
- `src/telemetry.cpp`: replaces the `call LocalPlayer::DoFFB` in `LocalPlayer::OnTask` (0x6F22C6)
  with a hook that samples the player car through the game's `ISuspension` / `IEngine` vtables,
  then calls the original.
- With the gameplay mod's `[Controls] GameController=0`, every controller the game opens is wrapped
  and reads as untouched (axes centred in the game's ranges, buttons up, hats centred), so the game ignores it in
  driving and menus while the gameplay mod feeds the controls. The F4 menu changes it live
  through the `NfsmwFfb_Setting` export (also Gain, the centring spring and SoftLock).
- `src/ffb.cpp`: a 200 Hz thread that turns the latest telemetry into constant, spring,
  damper, friction and sine effects.
- The gameplay ini's `[Controls] MenuAxes=0` (default) centres controller axes only in the
  game's frontend and pause menus, preserving buttons and hats. `MenuController=0` disables
  those too. Driving input and the gameplay mod's own bindings remain available. Both immediate
  state and buffered events use neutral values from the current data format and axis ranges;
  buffered input is resynchronised on menu entry and resume, including stationary pedals.
  Works with `NativeFFB=1`, but needs this mod enabled (`[FFB] Enabled=1`).
- `tools/ffbtool.exe`: lists controllers. `ffbtool test` nudges the wheel right/left and reports
  which way it moved.
- `tools/dis.py`, `tools/vtbl.py`: disassembly and vtable dumps with names (need a v1.3 symbol
  map in `vr/re/`, not included).

## Building

MSYS2 with the mingw32 toolchain: `./build.sh` (`./build.sh install` copies into `C:\Games\NFSMW`,
or `$NFSMW_DIR`).

Input regression tests (mingw32): `./tests/run.sh`. They check menu transitions,
deferred buffered events, controller suppression and live re-enabling using a synthetic
game layout and fake DirectInput device, plus effect recovery with a simulated driver
(focus loss, stopped/download-lost effects, failed operations and manual restart).
They do not access a physical wheel.
