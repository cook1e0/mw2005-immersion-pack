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

**Upgrading from v0.11.1:** replace all three plugins in `scripts`, replace
`nfsmw_vr_host.exe`, and add `nfsmw_crashdump.exe` next to `speed.exe`. Keep existing INIs
and bindings. The symbols ZIP is optional and only needed for VR crash-dump analysis.

**Validation:** all three plugins build; input, FFB recovery, gameplay, cockpit, hook,
OpenXR and crash-capture regression tests pass. Physical wheel/headset testing remains
outstanding; these changes do not establish or fix the cause of the reported Direct3D crash.

#### Automatic crash diagnostics

- The VR plugin starts `nfsmw_crashdump.exe`, an x86 helper installed next to `speed.exe`.
  Unhandled game exceptions save `nfsmw_crash.dmp` plus a readable `nfsmw_crash.txt`, with the
  faulting module, offset, exception code and invalid-access address. Nothing is uploaded.
- Existing ini files enable capture by default; `[VR] CrashDumps=0` disables it. Missing helpers
  are logged and leave the game playable. Handled exceptions do not produce dumps.
- The build preserves matching VR/helper debug symbols for analysing the generated dumps.

**Install:** replace `scripts\nfsmw_vr.asi` and `nfsmw_vr_host.exe`, add `nfsmw_crashdump.exe`
next to `speed.exe`, and keep existing ini files. This adds diagnosis; it does not establish or
fix the cause of the reported Direct3D crash.

**Validation:** a synthetic x86 process produces a readable dump with exception/context/module/
thread streams. Normal exit, handled exceptions, disabled capture and missing helpers are
covered. The cockpit, hook validation and XR frame regression harnesses pass. Testing with the
friend's game/headset remains outstanding.

### Changes since v0.11.0

- **Pedals in menus**: analog controller axes are ignored in the game's menus by default, preventing pedals and steering from scrolling them. With the game's controller input enabled, buttons and the d-pad still work. Driving input is unchanged.
- **Menu input settings**: F4 > Controls can restore analog axis navigation or disable all controller menu input. The F4 menu's own bindings and keyboard/mouse input are unaffected.
- **Pause and resume**: menu input uses neutral axis values from the controller's configured ranges, including buffered input and pedals held still when entering or leaving a menu.

**Upgrading from v0.11.0:** replace `scripts\nfsmw_ffb.asi` and `scripts\nfsmw_gameplay.asi`. Keep your existing `.ini` files and bindings; the menu filter uses its defaults when the new settings are missing. It requires the updated force feedback plugin, including when using `NativeFFB=1`. The VR plugin and helper are unchanged.

**Validation:** both updated plugins build successfully and the menu input regression tests pass. Physical wheel/pedal testing remains outstanding.
