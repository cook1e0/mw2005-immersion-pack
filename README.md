# NFS Most Wanted (2005) mods

Mods for *Need for Speed: Most Wanted* (2005, PC, `speed.exe` v1.3) aimed at sim-style driving:
VR, force feedback, and a real H-pattern gearbox with a clutch. Install any of them, or all three.

| Mod | Folder | Loads as | What it does |
|---|---|---|---|
| VR | [`vr/`](vr/) | `d3d9.dll` | Native OpenXR stereo 3D with 6-DoF head tracking; HUD and menus on a floating panel |
| Force feedback | [`ffb/`](ffb/) | `dinput8.dll` | Steering forces from the game's own tyre model |
| H-shifter + clutch | [`hshifter/`](hshifter/) | `dsound.dll` | H-pattern shifting, analog clutch, engine heat / over-rev / blow-ups, in-game tuning menu |

## Install

1. Download `nfsmw05-mod.zip` from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest).
2. Copy the files of the mods you want next to `speed.exe`:

   | Mod | Files |
   |---|---|
   | VR | `d3d9.dll`, `nfsmw_vr.ini` |
   | Force feedback | `dinput8.dll`, `nfsmw_ffb.ini` (`ffbtool.exe` optional: lists and tests wheels) |
   | H-shifter + clutch | `dsound.dll`, `nfsmw_hshifter.ini`, `hshiftertool.exe` |

3. H-shifter: with your pedals and shifter connected, run `hshiftertool setup` in the game folder
   to bind the clutch and each gear, or bind them in game under CONTROLS in the F4 menu.
4. VR: connect your headset before launching the game.

To uninstall a mod, delete its DLL. Settings are in the `.ini` files; each mod writes a `.log`
next to `speed.exe`.

## Requirements

- `speed.exe` v1.3
- VR: an OpenXR runtime with 32-bit support (Virtual Desktop's VDXR is tested)

## Building

- `vr/`: Visual Studio 2017 Build Tools (x86) and Windows SDK 10.0.17763, run `build.cmd`.
- `ffb/`, `hshifter/`: MSYS2 with the mingw32 toolchain, run `./build.sh` (`./build.sh install`
  copies into `C:\Games\NFSMW`, or `$NFSMW_DIR`).

## Credits

- [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw): the decompilation and its v1.3
  symbol map (`EngineRacer` by Brawltendo).
- [Sh2dow/NFS.CameraMod](https://github.com/Sh2dow/NFS.CameraMod): camera addresses for v1.3.

## License

MIT, see [LICENSE](LICENSE). OpenXR headers in `vr/third_party` are Apache-2.0.
