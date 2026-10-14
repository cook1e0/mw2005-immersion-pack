### Unreleased: cockpit near plane, VR recentre binding, misshift damage

- **VR near clip plane:** the player view's near plane is now `[VR] NearZ` (default 0.01 m, was
  the game's own), so the dashboard, wheel and pillars right in front of the eyes are no longer
  cut off. Adjustable live in F4 > VR > Near clip plane. 0 in the ini restores the game's value.
  Very low values can make distant surfaces flicker (depth precision).
- **Recenter VR view binding:** F4 > Controls > Recenter VR view binds any key, wheel or button
  to the VR mod's recentre (same as F8). Works while driving and in the game's menus.
- **Misshift damage:** a shift refused for lack of clutch has a 50% chance of doing 5% car damage
  (`[Shifter] MisshiftChance`, `MisshiftDamage`, also in F4 > Shifter and clutch). Needs car damage
  on; `MisshiftChance=0` turns it off.

### v0.11.2: recovery and regression fixes

- **Force feedback recovery:** reacquires previously working wheels after Alt-Tab or focus
  changes, restores stopped or unloaded effects, and retries failed effect operations.
  F4 menu transitions also trigger a recovery check. Forces return gradually after recovery.
- **Manual restart:** press F10 or choose F4 > Force Feedback > Restart wheel force feedback.
  Calibration and tuning are preserved. This resets the mod's effects, not native game FFB;
  `[FFB] KeyReset` changes the hotkey (`0` disables it).
- **Controller input:** neutralises held startup inputs and sends buffered release events
  when the game's controller input is disabled; deferred menu-reset events are preserved.
- **Gameplay:** fixes axis-binding direction and stale player-smoke selection. Legacy
  H-shifter settings can replace untouched packaged defaults, while customised gameplay
  INIs remain unchanged and the legacy file is retained for manual merging.
- **VR:** validates required hooks while preserving compatible widescreen resolution
  detours, and tightens OpenXR tracking/frame handling.
- **Crash diagnostics (VR):** the VR plugin starts `nfsmw_crashdump.exe`, a small helper next
  to `speed.exe`. An unhandled game crash saves `nfsmw_crash.dmp` and a readable
  `nfsmw_crash.txt` (faulting module, offset, exception code, access address). Nothing is
  uploaded. On by default, also with existing ini files; `[VR] CrashDumps=0` disables it. A
  missing helper is logged and the game still runs.

Force feedback now pauses while the game is not the foreground window and comes back when it is.

**Upgrading from v0.11.1:** replace all three plugins in `scripts` and `nfsmw_vr_host.exe`, and
add `nfsmw_crashdump.exe` next to `speed.exe`. Keep your existing `.ini` files and bindings.

**Validation:** all three plugins build, and the input, FFB recovery, gameplay, cockpit, hook,
OpenXR and crash-capture regression tests pass. Not yet tested with a physical wheel or headset;
the crash capture helps diagnose the reported Direct3D crash but does not fix it.

### v0.11.1: pedals in menus

- **Pedals in menus**: analog controller axes are ignored in the game's menus by default, preventing pedals and steering from scrolling them. With the game's controller input enabled, buttons and the d-pad still work. Driving input is unchanged.
- **Menu input settings**: F4 > Controls can restore analog axis navigation or disable all controller menu input. The F4 menu's own bindings and keyboard/mouse input are unaffected.
- **Pause and resume**: menu input uses neutral axis values from the controller's configured ranges, including buffered input and pedals held still when entering or leaving a menu.

**Upgrading from v0.11.0:** replace `scripts\nfsmw_ffb.asi` and `scripts\nfsmw_gameplay.asi`. Keep your existing `.ini` files and bindings; the menu filter uses its defaults when the new settings are missing. It requires the updated force feedback plugin, including when using `NativeFFB=1`. The VR plugin and helper are unchanged.

**Validation:** both updated plugins build successfully and the menu input regression tests pass. Physical wheel/pedal testing remains outstanding.
