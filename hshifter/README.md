# NFS Most Wanted (2005) H-pattern shifter + clutch mod

Real H-pattern gear selection and an analog clutch for `speed.exe` v1.3, working with any
DirectInput controller (wheel shifters, separate pedal sets, button boxes) or the keyboard.

- **H-pattern**: the gearbox follows the stick. Neutral and reverse are real gates. Gates the car
  doesn't have act as neutral.
- **Clutch**: drives the game's own friction-clutch model, so slip, launches, free revving with the
  pedal down and lock-up on release all come from the game's physics. Buttons or keys work as a
  digital clutch that is eased in and out.
- **Clutch required to shift** (optional): moving the stick into a gate without the clutch leaves
  the box in neutral until you press the clutch. Optional rev-matched clutchless shifts.
- Reverse is refused while rolling forward (and forward gears while rolling backward).
- **Engine wear** (optional): no rev limiter. Above the redline the engine heats up, loses power
  when hot, and blows at full heat. A blown engine totals the car in a race; in free roam, use
  *Jump to Safe House* from the pause menu. Manual shifting only.
- **Tuning menu**: F4 while driving. Engine wear, shifter and clutch settings, changed live and
  saved to the ini. Works with a wheel's d-pad and shows on the HUD panel in VR.

## Install

Download the zip from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest) and copy `dsound.dll`, `nfsmw_hshifter.ini`
and `hshiftertool.exe` next to `speed.exe`. Then, with your pedals and shifter connected, run in
the game folder:

```
hshiftertool setup
```

It asks you to press the clutch and put the stick in each gear, and writes the bindings to
`nfsmw_hshifter.ini`. `hshiftertool list` and `hshiftertool monitor` show the attached controllers
and live input values. To uninstall, delete `dsound.dll`.

The game's Automatic/Manual setting doesn't matter while the shifter is active. `Toggle=` (e.g.
`key:F9`) switches between the H-shifter and the game's sequential shifting while driving.

## Settings

All in `nfsmw_hshifter.ini` (commented). The ones you are most likely to change:

| Setting | |
|---|---|
| `[Clutch] BiteStart / BiteEnd` | pedal travel where the clutch starts to grip and grips fully |
| `[Clutch] Strength` | clutch friction relative to the game's clutch; lower = longer slip |
| `[Shifter] RequireClutch / ClutchToShift` | need the clutch (and how far down) to engage a gear |
| `[Shifter] RevMatchRPM` | allow clutchless shifts within this many rpm of the new gear |
| `[Shifter] Latching` | gear stays selected after its button is released (keyboard, button boxes) |
| `[Engine] EngineWear / RevLimiter` | engine heat and blow-ups on/off; keep the rev limiter |
| `[Engine] HeatUp / HeatUpMaxRPM / CoolDown` | heat per second at the redline / at max rpm, and cooling below it |
| `[Engine] PowerLossStart / PowerLoss` | heat where power starts dropping, and how much is lost at full heat |
| `[Engine] Totaled` | 0 = a blown engine shows "Blown Engine" instead of totaling the car |
| `[UI] Menu / MenuUp / ...` | tuning menu bindings |
| `[General] LogInputs` | log stick position and clutch travel to `nfsmw_hshifter.log` |

Binding syntax: `joy0:b12` (button), `joy0:s0` / `joy0:-rz` (axis), `joy0:pov0:up` (hat),
`key:LSHIFT` (keyboard), comma-separated for several. `[Devices] JoyN=` pins a slot to a controller
by name.

## How it works

Addresses and offsets are for v1.3. They are checked at startup, and nothing is patched if the
code doesn't match.

- `InputPlayer::FetchInput` calls `DoShifting(delta)` (0x6A8D80) and `DoAutoReverse` (0x6A8D89).
  Both calls are redirected. With the shifter active, the gear comes from the stick through
  `ITransmission::Shift`, and the game's auto-reverse (which shifts to R when braking at a
  standstill and swaps the pedals while reversing) is skipped.
- `InputPlayer::IsAutomaticShift` (0x68D090) reports manual while the shifter is active, so the
  automatic gearbox, which also skips neutral, stays out of the way.
- `EngineRacer::OnTaskSimulate` (0x6AF740) is detoured for the player's engine. While the pedal is
  down, the engine's `Clutch` (+0x198) is held in its ENGAGING state, where the torque passed
  between engine and gearbox is proportional to their speed difference times the global
  `ClutchStiffness` (0x8F8D9C). That stiffness is scaled by the pedal for the duration of the call.
  With the pedal up and engine and gearbox speeds matched, the clutch is handed back to the game
  fully engaged.
- Engine wear follows `EngineDragster::ComputeEngineHeat` but runs on the player's `EngineRacer`,
  from the same `OnTaskSimulate` detour. EngineRacer's vtable slots `UseRevLimiter` (0x8AB78C) and
  `GetEngineTorque` (0x8AB7A8, power loss) are replaced. At full heat the engine is marked blown
  and the car destroyed through `IDamageable::Destroy`, as a drag race crash does. Drag races keep
  the game's model.
- The tuning menu is drawn after the game's HUD (eDisplayFrame, 0x6E75A7) as untextured quads with
  a GDI-rasterised font, so the VR mod puts it on its HUD panel.
- Input comes from a separate non-exclusive DirectInput instance (the system `dinput8.dll`, never a
  proxy in the game folder) polled at ~500 Hz on its own thread.

Based on the dbalatoni13/nfsmw decompilation (`EngineRacer` by Brawltendo).

## Building

MSYS2 with the mingw32 toolchain: `./build.sh` builds into `build/`, `./build.sh install` also
copies the files into `C:\Games\NFSMW` (or `$NFSMW_DIR`).

## Limitations

- The engine can't stall: the game has no engine-off state and keeps the engine at idle or above.
- Nothing grinds audibly when a shift is refused; the box just stays in neutral.
- Not yet tested with a physical wheel and shifter, only keyboard bindings.
