# v0.11.0 — Steering and VR fixes

Download `nfsmw05-mod.zip`. See the [README](https://github.com/cook1e0/nfsmw05-mod#install)
for installation instructions.

- F4 > Controls: steering linearity (0.5–3.0; 1 = linear).
- F4 > VR: recenter view and HUD size, distance and height. HUD settings update live and save
  when the menu closes, including when using the 64-bit SteamVR helper.
- F4 > Force feedback: centring grows with speed. Adjust the parked fraction and the speed
  where it reaches full strength (defaults: 30% parked, full at 150 km/h).
- The cockpit seat follows the current car body, rather than the previous camera pose.
  Camera switches use the current transform, driver LOD changes keep the seat fixed, and
  unloading a car releases it safely. Flat-screen frames update the camera too.
- Temporary render memory increased from 400 KB to 2 MB per buffer. Both eyes allocate within
  one frame; overflowing the old buffer could drop right-eye cars and crash unchecked code.
- Plugin/helper version check; install both VR binaries from this package.
- Includes the v0.10.1 widescreen-fix compatibility changes: `HideAABug` edge clears and
  `ConsoleGamma`/`SMAA` colour passes are skipped on the transparent VR HUD layer, preventing
  a black border or blacked-out panel. Flat-screen behaviour is preserved.

Validation: all three mods and the x64 helper build successfully; cockpit regression tests pass.
A desktop stereo driving run checked 50,576 car draws with zero seat/body transform difference
and reached 705 KB of temporary render memory without allocation-failure or crash log entries.
This supports the allocation fix but does not establish that every cause of the reported
SteamVR crash is resolved. A real SteamVR headset session remains to be tested.

For headset play, use `FakeHmd=0`. F7 toggles horizon levelling; `LevelRoll=0` and `LevelPitch=0`
keep the camera's rotation fully attached to the body. Horizon levelling remains enabled by
default for comfort.

Upgrading from v0.10.x: replace all three `scripts/nfsmw_*.asi` plugins and
`nfsmw_vr_host.exe` next to `speed.exe`. Keep your existing `.ini` files to preserve settings and
bindings; new settings use their defaults when absent. The zip's `.ini` files are for new installs.
