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

### API layers (frame generation, OpenXR Toolkit)

The mod loads the OpenXR API layers installed on the PC, as other OpenXR games do. These include
frame generation tools such as [VectorXR](https://github.com/DienerTech/vectorxr) and
[OFXR Bridge](https://github.com/djules75/OFXR-Bridge), and OpenXR Toolkit. Most of these layers
are 64-bit only, so they load only in `nfsmw_vr_host.exe`. With a runtime that runs inside the game
(Virtual Desktop, Meta Quest Link), set `Bridge=1` to use them. `nfsmw_vr.log` lists every
64-bit-only layer that this leaves out, and `nfsmw_vr_host.log` lists the layers that loaded.
Turn off the runtime's own smoothing (SteamVR Motion Smoothing, Virtual Desktop's Synchronous
Spacewarp) while a frame generation tool is on. `ApiLayers=0` loads no layers.

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

In the bumper and hood cameras you sit in the driver's seat, with the car's own dashboard, wheel and the driver's hands in front of you; the driver's head is left out. Seat adjustments are saved per car. This also works on a flat screen without a headset (the in-car dashboard's display is there, empty: the HUD stays on the screen).

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
- **`SteeringWheel`:** the car's steering wheel turns with your wheel, by the same angle (needs the gameplay mod with a bound steering axis and `[Controls] WheelRotation` set to your wheel's rotation). The wheel is found in each car's interior model; the log says `steering wheel at ...` or why not.
- **`NearZ`:** near clip plane in metres (default 0.05; the game's own is 0.5). Lower it if the dashboard or wheel close to your eyes is cut off; raise it if distant surfaces flicker. 0 = the game's own.

## Recording

F6 switches between the spectator view, side-by-side 3D (`sbs`), and the raw eye mirror (`eye`).

`SpectatorPreset=broadcast` gives the spectator view an 80-degree horizontal FOV and a 16:9 aspect ratio. A missing preset, or `headset`, keeps the previous presentation. Non-zero `SpectatorFov` and `SpectatorAspect` override either preset. Zero selects that preset's automatic value. F4 > VR exposes the preset, FOV, aspect, smoothing, and HUD switch. Editing a value selects `custom` and saves the resolved FOV and aspect. Selecting a preset preserves numeric overrides.

Broadcast and Custom reuse one rendered eye. They smooth yaw and pitch, then check all four output corner rays against the actual rendered projection. Corrections first move yaw and pitch toward the eye without head roll. If that path cannot provide coverage, temporary roll restores coverage and releases gradually when space returns. Corrected yaw and pitch become the next filter state. A frame gap longer than 0.1 seconds, a recenter, or a mode transition resets history. The crop fits both source dimensions with a sampling inset. Coverage takes priority over a level image and smoothing. Levelling is relative to the base game camera; `LevelRoll` still controls vehicle roll.

The spectator keeps the game's HUD on its 4:3 canvas. Pixel-based telemetry uses a separate layer with its native proportions, and F4 uses its native canvas aspect to keep text proportional. Game menus and videos fit a centred 4:3 rectangle. The raw eye and SBS presentations retain their existing behaviour. A HUD already drawn into the eye image remains visible when the separate spectator HUD is hidden.

`SpectatorWidth` controls window size, not source detail. The default, 0, makes the window borderless and as large as the monitor allows at the view's aspect (the whole screen for 16:9 on a 16:9 monitor); a pixel width gives a normal framed window that size, shrunk only if it doesn't fit the screen. Cropping enlarges part of the eye image. The log reports effective framing and source pixels. `FovScale` changes the headset render too; wider values trade pixel density and potentially rendering time for more coverage and are not changed by a spectator preset.

Broadcast uses speed-dependent yaw and pitch filtering. `SpectatorSmoothing` controls smoothing at rest. `SpectatorAdaptive=0` selects the fixed exponential filter for comparison. Temporary roll releases with a 0.2-second time constant. Camera correction starts in the outer 12% of the available crop margin. These settings affect the desktop image only.

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
The suite also checks spectator coverage, correction feedback, roll release, and filter response with deterministic random poses and scripted motion. `tests\spectator_render_run.cmd` runs the production spectator shader on a D3D9 device, checks rendered pixels, HUD/menu framing, and device-state restoration, and saves captures under `dist\validation`.

The automated suites use synthetic game layouts, fake runtime calls, and a standalone D3D9 device. They do not launch or modify the game. Incompatible required render hooks leave VR inactive; an existing valid resolution detour, such as the widescreen fix's, is chained.

To run the fixed motion sequence in the game without a headset, set `FakeHmd=1`, `FakeMotion=1`, and `SpectatorPreset=broadcast` in `[VR]`. FakeHmd is a fallback when no runtime session is available. The 12-second loop contains yaw and pitch sweeps, step flicks, and a yaw flick with a 30-degree lean. The log's `spectator test` lines count final camera frames, coverage failures (`outside`), and corrected frames. Restore both debug keys to zero before playing in a headset. The numerical tests run the same motion function at 72, 90, and 120 Hz.

For dashboard crop diagnostics, set `KeyDump=120` in `[VR]`, restart, and press F9 while driving. The BMP captures next to `speed.exe` include both eyes, the submitted HUD, the original HUD before dashboard extraction (`hud_raw`), the dashboard atlas (`dash`), telemetry (`overlay`), and the displayed desktop frame (`spectator`). The submitted HUD can be empty because the in-car display has consumed it; use `hud_raw` to check source crop coordinates. `KeyDump=0` disables the key.

`DashRenderer=1` renders the stock UI objects into six dashboard cells: minimap and markers,
radar, speedometer and meters, pursuit status, race/bounty information, and messages. The ordinary
HUD remains complete. Unknown UI content stays on the HUD layer; the camera-dependent radar
surround is replaced by the physical dashboard. The diagnostic comparison panel has been removed.
Each native part is placed exactly where its crop would put it, at the crop's scale, so the
element X/Y/Size controls and any saved layout look the same with both renderers; `DashRenderer=0`
selects the calibrated crops, which are only less sharp.

Native components support the stock `HUD_SingleRace.fng` / `HUD_Drag.fng` layout with either
`FixHUD` setting of the widescreen fix. With `FixHUD=1` the fix widens the HUD's canvas and moves the
centre parts half the extra width right and the right parts all of it; the native parts and the
crops both take that off again, so the display is the same as with `FixHUD=0`. A missing layout
anchor, incompatible draw, clipped component or failed target uses the complete HUD's crop path for
that frame. Map quads may extend past their circular UV mask. Off-screen animation geometry is
checked only where the game's viewport could have shown it.

`FakeHmd=1` always uses a window so the recommended eye size does not become a desktop display
mode. `Resolution=auto` can therefore render at 1832x1920 while the Broadcast window stays 1600x900.
F4 and telemetry render on a separate canvas with the desktop client's aspect, matching the mouse
mapping. Their headset composition preserves the same proportions inside the 4:3 HUD layer.

`HudProbe=1` enables bounded diagnostics and an extra speedometer-only atlas; it does not add a
panel beside the dashboard. `HudProbeSource=420,216,660,500` controls only that diagnostic atlas.
The first six qualifying UI-tree changes capture 45 HUD frames. F9 requests another burst and
saves the eye, HUD, atlas and spectator BMPs. A stable direct dashboard also saves one automatic
capture per launch while this debug setting is enabled. Set `HudProbe=0` for normal use.

CSV captures next to `speed.exe` share a `nfsmw_hudprobe_PID_NNN` prefix: frames, draw formats and
vertex bounds, the live object tree, and CPU clipping calls. The clip trace covers two checked
ordinary `AddPoly` call sites; zero records do not prove every path is unclipped. CPU timings
include diagnostics and do not establish GPU cost or an FPS change.

Run `tests\hud_probe_run.cmd` for multisampling, scissor mapping, shader masks, unchanged ordinary
HUD targets, state restoration, transparent clearing, live-tree replacement, six-cell composition
at 1024x768 / 2560x1440 / 1832x1920, and whole-frame fallback. To replay recorded geometry through
the production ownership and bounds tables, run `obj\tests\hud_probe_render_tests.exe --replay
C:\Games\NFSMW\nfsmw_hudprobe_PID_NNN` after building the tests. Exit 2 means no component draws;
exit 1 means visible content exceeded its cell. This checks bounds, not textures or GPU timing.

Run `tests\crash_run.ps1` for crash-capture integration tests. They use a synthetic x86 process,
check the dump's exception/context/module/thread streams, and cover handled exceptions, normal
exit, disabled capture, and a missing helper. Test outputs stay under `dist\validation`.

## Credits

- [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw): the decompilation's v1.3 symbol map and struct layouts made finding the hook points practical.
- [Sh2dow/NFS.CameraMod](https://github.com/Sh2dow/NFS.CameraMod): camera addresses for v1.3.
- [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers (Apache-2.0), in `third_party/openxr`.

## License

MIT, see [LICENSE](../LICENSE). OpenXR headers are Apache-2.0.
