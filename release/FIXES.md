### Changes since v0.11.0

- **Pedals in menus**: analog controller axes are ignored in the game's menus by default, preventing pedals and steering from scrolling them. With the game's controller input enabled, buttons and the d-pad still work. Driving input is unchanged.
- **Menu input settings**: F4 > Controls can restore analog axis navigation or disable all controller menu input. The F4 menu's own bindings and keyboard/mouse input are unaffected.
- **Pause and resume**: menu input uses neutral axis values from the controller's configured ranges, including buffered input and pedals held still when entering or leaving a menu.

**Upgrading from v0.11.0:** replace `scripts\nfsmw_ffb.asi` and `scripts\nfsmw_gameplay.asi`. Keep your existing `.ini` files and bindings; the menu filter uses its defaults when the new settings are missing. It requires the updated force feedback plugin, including when using `NativeFFB=1`. The VR plugin and helper are unchanged.

**Validation:** both updated plugins build successfully and the menu input regression tests pass. Physical wheel/pedal testing remains outstanding.
