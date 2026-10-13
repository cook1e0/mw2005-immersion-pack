# NFS Most Wanted (2005) mods

Mods for *Need for Speed: Most Wanted* (2005, PC, `speed.exe` v1.3) aimed at sim-style driving:
VR, force feedback, and gameplay changes (a real H-pattern gearbox with a clutch, engine wear,
damage and more). Install any of them, or all three.

| Mod | Folder | Plugin (or proxy DLL) | What it does |
|---|---|---|---|
| VR | [`vr/`](vr/) | `nfsmw_vr.asi` (`d3d9.dll`) | Native OpenXR stereo 3D with 6-DoF head tracking; HUD and menus on a floating panel |
| Force feedback | [`ffb/`](ffb/) | `nfsmw_ffb.asi` (`dinput8.dll`) | Steering forces from the game's own tyre model, centring spring, soft lock |
| Gameplay | [`gameplay/`](gameplay/) | `nfsmw_gameplay.asi` (`dsound.dll`) | Wheel and pedal controls with a steering lock, H-pattern shifter and clutch, engine heat and blow-ups, car damage and smoke, drivable popped tires, physics sliders, no SUV cops, in-game tuning menu |

## Install

1. Download `nfsmw05-mod.zip` from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest).
2. Copy `dinput8.dll` ([Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader))
   next to `speed.exe`. Skip this if you already have it, e.g. from the
   [widescreen fix](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/nfsmw).
3. Copy the files of the mods you want: each `.asi` goes into the `scripts` folder next to
   `speed.exe` (create it if needed), the rest next to `speed.exe`:

   | Mod | `scripts\` | Next to `speed.exe` |
   |---|---|---|
   | VR | `nfsmw_vr.asi` | `nfsmw_vr_host.exe`, `nfsmw_vr.ini` |
   | Force feedback | `nfsmw_ffb.asi` | `nfsmw_ffb.ini` (`ffbtool.exe` optional: lists and tests wheels) |
   | Gameplay | `nfsmw_gameplay.asi` | `nfsmw_gameplay.ini` |

4. Gameplay: press F4 while driving. Bind your wheel and pedals under *Controls* (set *Wheel
   rotation* to your wheel driver's setting and *Steering lock* to taste, then turn *Game's own
   controller input* off), and an H-shifter's clutch and gears under *Gear bindings*.
5. VR: connect your headset before launching the game.

**Upgrading from v0.9.1 or older:** the mods used to be `d3d9.dll`, `dinput8.dll` and
`dsound.dll` next to `speed.exe`. Delete the old `d3d9.dll` and `dsound.dll` (if a mod is
installed both ways, the old DLL runs and the `.asi` stays inactive); `dinput8.dll` is
replaced by the loader. Your `.ini` files stay as they are.

**Without an ASI loader:** each `.asi` still works renamed to its proxy DLL next to
`speed.exe`: `nfsmw_vr.asi` as `d3d9.dll`, `nfsmw_ffb.asi` as `dinput8.dll`,
`nfsmw_gameplay.asi` as `dsound.dll`.

**Upgrading from v0.6.0 or older:** the H-shifter mod is now the gameplay mod. Replace
`dsound.dll`; on the first start your `nfsmw_hshifter.ini` is renamed to
`nfsmw_gameplay.ini` with all your settings and bindings. `hshiftertool.exe` and
`nfsmw_hshifter.log` can be deleted.

To uninstall a mod, delete its `.asi`. Settings are in the `.ini` files; each mod writes a `.log`
next to `speed.exe`.

## Requirements

- `speed.exe` v1.3
- VR: an OpenXR runtime: Virtual Desktop (VDXR, tested), SteamVR, or Meta Quest Link. See
  [vr/README.md](vr/README.md#runtimes) for SteamVR setup

## Building

- `vr/`: Visual Studio 2017 Build Tools (x86) and Windows SDK 10.0.17763, run `build.cmd`.
- `ffb/`, `gameplay/`: MSYS2 with the mingw32 toolchain, run `./build.sh` (`./build.sh install`
  copies into `C:\Games\NFSMW`, or `$NFSMW_DIR`: as `scripts\nfsmw_*.asi` if that is installed,
  otherwise as the proxy DLL).
- Release zip: build all three, then `./package.sh path/to/Ultimate-ASI-Loader/dinput8.dll`
  (the x86 loader) writes `dist/nfsmw05-mod.zip`.

## Credits

- [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw): the decompilation and its v1.3
  symbol map (`EngineRacer` by Brawltendo).
- [Sh2dow/NFS.CameraMod](https://github.com/Sh2dow/NFS.CameraMod): camera addresses for v1.3.

## License

MIT, see [LICENSE](LICENSE). OpenXR headers in `vr/third_party` are Apache-2.0;
[Dear ImGui](https://github.com/ocornut/imgui) in `gameplay/third_party/imgui` is MIT.
The release zip includes [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
by ThirteenAG (MIT, `release/LICENSE-asi-loader.txt`).
