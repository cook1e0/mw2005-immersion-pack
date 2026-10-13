# NFS Most Wanted (2005) force feedback mod

Physics-based steering force feedback for `speed.exe` v1.3, driven from the game's own tyre model
instead of its weak built-in effects.

## Install

Download the zip from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest)
and copy `dinput8.dll` and `nfsmw_ffb.ini` next to `speed.exe`. `dinput8.dll` also works renamed to
`nfsmw_ffb.asi` with an ASI loader. To uninstall, delete it.

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

## First run: automatic calibration

The force direction convention differs between wheel drivers, and the sign of the game's
tyre slip angle can't be read from the binary. The mod measures both against your wheel's
position and saves the results in `nfsmw_ffb.ini` `[Calibration]`:

1. **ForceSign**: on the first menu screen the wheel gets two short gentle nudges (right, then left).
   Keep your hands off the wheel for a moment. Retries automatically if you were holding it.
2. **SlipSign / RightSign**: during the first corners above ~45 km/h. Until then, steering weight
   comes from a speed-dependent centring spring instead of the tyre model.

Watch `nfsmw_ffb.log` for `calibration:` lines. Set the values back to `0` to re-detect.

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
- `src/ffb.cpp`: a 200 Hz thread that turns the latest telemetry into constant, spring,
  damper, friction and sine effects.
- `tools/ffbtool.exe`: lists controllers. `ffbtool test` nudges the wheel right/left and reports
  which way it moved.
- `tools/dis.py`, `tools/vtbl.py`: disassembly and vtable dumps with names (need a v1.3 symbol
  map in `vr/re/`, not included).

## Building

MSYS2 with the mingw32 toolchain: `./build.sh` (`./build.sh install` copies into `C:\Games\NFSMW`,
or `$NFSMW_DIR`).
