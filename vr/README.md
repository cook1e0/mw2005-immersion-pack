# NFS Most Wanted VR

Native OpenXR VR mod for *Need for Speed: Most Wanted* (2005, PC) with full stereo 3D and 6-DoF head tracking.

## Requirements

- `speed.exe` **v1.3** (6,029,312 bytes)
- An OpenXR runtime with **32-bit** support. Virtual Desktop (VDXR) is tested.

## Install

1. Copy `d3d9.dll` and `nfsmw_vr.ini` into the game folder, next to `speed.exe`.
2. Connect your headset, then launch the game.

To uninstall, delete the two files. If something goes wrong, check `nfsmw_vr.log` in the game folder.

## Controls

| Key | Action |
|-----|--------|
| F6  | Cycle the desktop view |
| F7  | Toggle level horizon |
| F8  | Recenter |

## Settings

Everything is in `nfsmw_vr.ini`. The useful ones:

- **`Resolution` / `ResolutionScale`:** each eye renders at the headset's recommended size by default. Lower `ResolutionScale` for more fps.
- **`LevelRoll` / `LevelPitch`:** how much of the car's body roll and pitch to remove from your view. By default roll is removed and pitch is kept.
- **`HudDistance` / `HudWidth`:** placement of the floating HUD panel.

## Recording

The game window shows a stabilised version of the headset view, including the floating HUD. F6 switches between that view, side-by-side 3D (`sbs`) and the raw eye mirror. For a 16:9 recording, set `SpectatorFov=100` and `SpectatorAspect=1.7778`.

## Known issues

- Rain and some post-processing effects run slightly faster than normal, because the scene is rendered twice per frame.
- The rear-view mirror is part of the flat HUD panel.
- The crash camera of a totaled car (drag race crashes) is skipped in the headset; the view stays in the car.

## How it works

`d3d9.dll` is a proxy that upgrades the game's device to D3D9Ex and shares the eye images with a D3D11 device that submits them to OpenXR. No OpenXR loader DLL is needed.

Each frame, the game's scene renderer (`___eDisplayFrame`) runs once per eye. Before each run, the player camera moves to that eye's pose, and the view's matrices are rebuilt with the headset's FOV. Culling, shadows, reflections and post-FX are therefore correct per eye. Everything drawn after the scene (HUD, menus) is redirected to a transparent layer shown as a floating panel.

## Building

Requires Visual Studio 2017 Build Tools (x86) and Windows SDK 10.0.17763. Run `build.cmd`; the output is `bin\d3d9.dll`.

## Credits

- [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw): the decompilation's v1.3 symbol map and struct layouts made finding the hook points practical.
- [Sh2dow/NFS.CameraMod](https://github.com/Sh2dow/NFS.CameraMod): camera addresses for v1.3.
- [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers (Apache-2.0), in `third_party/openxr`.

## License

MIT, see [LICENSE](../LICENSE). OpenXR headers are Apache-2.0.
