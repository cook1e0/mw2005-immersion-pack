# NFS Most Wanted VR

Native OpenXR VR mod for *Need for Speed: Most Wanted* (2005, PC) with full stereo 3D and 6-DoF head tracking.

## Requirements

- `speed.exe` **v1.3**
- An OpenXR runtime: Virtual Desktop (VDXR, tested), SteamVR, or Meta Quest Link.

## Install

1. With [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`dinput8.dll`, in the release zip)
   next to `speed.exe`, copy `nfsmw_vr.asi` and `nfsmw_vr.ini` into the `scripts` folder and `nfsmw_vr_host.exe` plus `nfsmw_crashdump.exe`
   next to `speed.exe`. Without a loader, rename `nfsmw_vr.asi` to `d3d9.dll` and put it and the `.ini` next to `speed.exe`.
2. Connect your headset, then launch the game.

With ThirteenAG's widescreen fix, its end-of-frame colour pass (`ConsoleGamma`, `SMAA`) and edge lines (`HideAABug`) are
skipped on the HUD layer in VR, where they would black out or frame the menu and HUD panel; the headset view never had them.

To uninstall, delete `nfsmw_vr.asi` (or `d3d9.dll`), `nfsmw_vr_host.exe`, `nfsmw_crashdump.exe` and `nfsmw_vr.ini`. If something goes wrong, check `nfsmw_vr.log` in the game folder.

## Crash diagnostics

With `nfsmw_crashdump.exe` next to `speed.exe`, crash capture is enabled by default; existing ini
files need no changes. After an unhandled game crash, send these files from the game folder:

- `nfsmw_crash.dmp` and `nfsmw_crash.txt`
- `nfsmw_vr.log`, `nfsmw_vr_host.log` (if present), and `nfsmw_crashdump.log`

The helper starts automatically, needs no administrator setup, and exits when the game exits.
The latest crash replaces the previous dump and report. Copy them before another crash; logs
are replaced when the game starts. If the game folder cannot be written, the helper tries
`%TEMP%\nfsmw_crash_<pid>.dmp` and `.txt`; its log records the path.

These files stay on the computer; nothing is uploaded. Dumps contain portions of game memory.
Set `[VR] CrashDumps=0` to disable capture. Missing or failed helpers leave the game playable and
are reported in `nfsmw_vr.log`. Capture covers unhandled exceptions after graphics initialisation;
forced process termination, hangs, and another component replacing the exception filter may
require an external dump tool.

## Runtimes

The game is a 32-bit program, and most OpenXR runtimes have no 32-bit version. The mod uses the
runtime that is set as active (in the SteamVR, Virtual Desktop or Meta Quest Link settings) and
picks the way to reach it by itself:

| Runtime | Headsets | How it runs |
|---|---|---|
| Virtual Desktop (VDXR) | Quest, Pico, ... over Wi-Fi | inside the game (32-bit runtime) |
| Meta Quest Link / Air Link | Quest, Rift | inside the game (32-bit runtime) |
| SteamVR | Index, Vive, Pimax, Bigscreen Beyond, Quest via Steam Link or ALVR, ... | through `nfsmw_vr_host.exe` |

### SteamVR

1. In SteamVR, open **Settings > OpenXR** and click **Set SteamVR as OpenXR Runtime** (it shows as
   "current" when it already is).
2. Make sure `nfsmw_vr_host.exe` is next to `speed.exe`.
3. Start SteamVR with your headset connected, then launch the game.

SteamVR only has a 64-bit OpenXR runtime, so the mod starts `nfsmw_vr_host.exe`, a small 64-bit
helper that runs the OpenXR session and hands the game's eye images to SteamVR. It closes with the
game and writes `nfsmw_vr_host.log`. SteamVR lists the game as "nfsmw_vr_host".

Run SteamVR's Room Setup once before playing. Without a play area, SteamVR may start Room Setup on
its own and close the game's VR session (the game then continues on the monitor).

### Choosing a runtime for this game only

`Bridge=auto` in `nfsmw_vr.ini` uses the helper when the active runtime has no 32-bit version from
the same install, and runs inside the game otherwise. `Bridge=1` always uses the helper, `Bridge=0`
never does.

To use a runtime other than the active one, start the game with the `XR_RUNTIME_JSON` environment
variable set to the runtime's manifest, for example from a `.bat` file next to `speed.exe`:

```bat
rem Meta Quest Link (32-bit manifest, runs inside the game)
set XR_RUNTIME_JSON=C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_32.json
start "" speed.exe
```

For a 64-bit-only manifest such as SteamVR's
(`C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json`), also set
`Bridge=1`.

## Controls

| Key | Action |
|-----|--------|
| F6  | Cycle the desktop view |
| F7  | Toggle level horizon |
| F8  | Recenter |
| Numpad 8 / 2 | Seat forward / back |
| Numpad 4 / 6 | Seat left / right |
| Numpad 9 / 3 | Seat up / down |
| Numpad 5 | Seat back to the driver's head |

In the bumper and hood cameras you sit in the driver's seat, with the car's own dashboard, wheel and the driver's hands in front of you; the driver's head is left out. Seat adjustments are saved per car. This also works on a flat screen without a headset.

With the gameplay mod, **F4 > VR** has **Recenter view** (same as F8), **HUD size**, **HUD distance**,
**HUD height** and **Near clip plane**, and **F4 > Controls > Recenter VR view** binds the recenter to
any key or wheel button. Panel changes take effect immediately, including through the SteamVR helper,
and are saved to `nfsmw_vr.ini` when the menu closes.

The seat follows the car's current body transform, with a fixed position inside the car. Horizon
levelling still changes the view's roll/pitch relative to the body: F7 toggles it, or set
`LevelRoll=0` and `LevelPitch=0` for full body rotation.

## Settings

Everything is in `nfsmw_vr.ini`. The useful ones:

- **`Resolution` / `ResolutionScale`:** each eye renders at the headset's recommended size by default. Lower `ResolutionScale` for more fps.
- **`LevelRoll` / `LevelPitch`:** how much of the car's body roll and pitch to remove from your view. By default roll is removed and pitch is kept.
- **`HudDistance` / `HudWidth`:** placement of the floating HUD panel.
- **`NearZ`:** near clip plane in metres (default 0.05; the game's own is 0.5). Lower it if the dashboard or wheel close to your eyes is cut off; raise it if distant surfaces flicker. 0 = the game's own.

## Recording

The game window shows a stabilised version of the headset view, including the floating HUD. F6 switches between that view, side-by-side 3D (`sbs`) and the raw eye mirror. For a 16:9 recording, set `SpectatorFov=100` and `SpectatorAspect=1.7778`.

## Known issues

- Rain and some post-processing effects run slightly faster than normal, because the scene is rendered twice per frame.
- The rear-view mirror is part of the flat HUD panel.
- The game originally has only 400 KB of temporary render memory per frame. Rendering two eyes
  can exceed that in busy scenes, dropping cars in the second eye and potentially crashing. The
  mod reserves 2 MB per buffer and logs usage/failures. This addresses that allocation limit;
  SteamVR headset testing is still needed to confirm the reported crash is fully resolved.

Install `nfsmw_vr.asi` and `nfsmw_vr_host.exe` from the same build. A helper version mismatch is
reported in `nfsmw_vr.log`. `FakeHmd=1` is a desktop stereo test mode; use `FakeHmd=0` for playing
with a real headset.

## How it works

The mod wraps `Direct3DCreate9` (as the `d3d9.dll` proxy, or as an `.asi` by redirecting the game's import of it), upgrades the game's device to D3D9Ex and shares the eye images with a D3D11 device that submits them to OpenXR. No OpenXR loader DLL is needed.

For runtimes without a 32-bit version, the same OpenXR code is built as the 64-bit `nfsmw_vr_host.exe`. The game starts it and forwards each frame's calls (wait for the frame, eye poses, submit) over shared memory. The eye textures are D3D9Ex shared handles, which the helper opens directly, so no image is copied through the CPU.

Each frame, the game's scene renderer (`___eDisplayFrame`) runs once per eye. Before each run, the player camera moves to that eye's pose, and the view's matrices are rebuilt with the headset's FOV. Culling, shadows, reflections and post-FX are therefore correct per eye. Everything drawn after the scene (HUD, menus) is redirected to a transparent layer shown as a floating panel. The totaled crash camera is skipped so the view stays in the car.

## Building

Requires Visual Studio 2017 Build Tools (x86 and x64) and Windows SDK 10.0.17763. Run `build.cmd`; the output is `bin\d3d9.dll` (released as `nfsmw_vr.asi`), `bin\nfsmw_vr_host.exe`, and the x86 `bin\nfsmw_crashdump.exe`. Keep the generated PDB files with the matching build for dump analysis.

Run `tests\run.cmd` for cockpit, required-hook validation and OpenXR frame regressions.
They exercise production code with a synthetic game layout and fake runtime calls;
they do not launch or modify the game. Incompatible required render hooks leave VR inactive;
an existing valid resolution detour (such as the widescreen fix's) is chained.

Run `tests\crash_run.ps1` for crash-capture integration tests. They use a synthetic x86 process,
check the dump's exception/context/module/thread streams, and cover handled exceptions, normal
exit, disabled capture, and a missing helper. Test outputs stay under `dist\validation`.

## Credits

- [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw): the decompilation's v1.3 symbol map and struct layouts made finding the hook points practical.
- [Sh2dow/NFS.CameraMod](https://github.com/Sh2dow/NFS.CameraMod): camera addresses for v1.3.
- [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers (Apache-2.0), in `third_party/openxr`.

## License

MIT, see [LICENSE](../LICENSE). OpenXR headers are Apache-2.0.
