# NFS Most Wanted (2005) mods

Mods for *Need for Speed: Most Wanted* (2005, PC, `speed.exe` v1.3) aimed at sim-style driving:
VR, force feedback, and a real H-pattern gearbox with a clutch. Each one loads through a
different proxy DLL, so they can be installed together or on their own.

| Mod | Folder | Loads as | What it does |
|---|---|---|---|
| VR | [`vr/`](vr/) | `d3d9.dll` | Native OpenXR stereo 3D with 6-DoF head tracking; HUD and menus on a floating panel |
| Force feedback | [`ffb/`](ffb/) | `dinput8.dll` | Steering forces from the game's own tyre model |
| H-shifter + clutch | [`hshifter/`](hshifter/) | `dsound.dll` | H-pattern shifting, analog clutch, engine heat / over-rev / blow-ups, in-game tuning menu |

To install, download the zip from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest)
and copy the files of the mods you want next to `speed.exe`; no building needed. Each folder's
README covers settings and how the mod works.

## Highlights

- **Engine wear** (H-shifter mod): the drag race engine model in every mode. No rev limiter,
  over-revving heats the engine, a hot engine loses power, and at full heat it blows: the car is
  totaled in races, dead in free roam until you jump to a safehouse, busted in a pursuit.
- **Tuning menu** (H-shifter mod, F4 while driving): engine wear, shifter and clutch settings
  live in game, usable with a wheel's d-pad. Drawn with the HUD, so it is on the VR HUD panel in
  the headset.
- In VR, the totaled crash camera is skipped so the view stays in the car.

## Requirements

- `speed.exe` v1.3. Copies with an extra appended PE section (some no-CD / widescreen patched
  exes) work too: the mods check the code at every hook site, not the file size.
- VR: an OpenXR runtime with 32-bit support (Virtual Desktop's VDXR is tested).

## Building

- `vr/`: Visual Studio 2017 Build Tools (x86) and Windows SDK 10.0.17763, run `build.cmd`.
- `ffb/`, `hshifter/`: MSYS2 with the mingw32 toolchain, run `./build.sh` (`./build.sh install`
  copies into `C:\Games\NFSMW`, or `$NFSMW_DIR`).

## Credits

- [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw): the decompilation and its v1.3
  symbol map made the hook points practical to find (and Brawltendo's `EngineRacer` work).
- [Sh2dow/NFS.CameraMod](https://github.com/Sh2dow/NFS.CameraMod): camera addresses for v1.3.

## License

MIT, see [LICENSE](LICENSE). OpenXR headers in `vr/third_party` are Apache-2.0.
