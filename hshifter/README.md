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
- Works next to the force feedback (`dinput8.dll`) and VR (`d3d9.dll`) mods. This one loads as
  `dsound.dll`.

## Install

```sh
./build.sh install        # MSYS2 mingw32; copies dsound.dll, hshiftertool.exe, nfsmw_hshifter.ini
```

Then, with your pedals and shifter connected, run in the game folder:

```
hshiftertool setup
```

It asks you to press the clutch and put the stick in each gear, and writes the bindings to
`nfsmw_hshifter.ini`. `hshiftertool list` and `hshiftertool monitor` show the attached controllers
and live input values. To uninstall, delete `dsound.dll`.

In game, play as usual. The in-game Automatic/Manual setting doesn't matter while the shifter is
active. `Toggle=` (e.g. `key:F9`) switches between the H-shifter and the game's sequential
shifting while driving, and the clutch keeps working either way.

## Settings

All in `nfsmw_hshifter.ini` (commented). The ones you are most likely to change:

| Setting | |
|---|---|
| `[Clutch] BiteStart / BiteEnd` | pedal travel where the clutch starts to grip and grips fully |
| `[Clutch] Strength` | clutch friction relative to the game's clutch; lower = longer slip |
| `[Shifter] RequireClutch / ClutchToShift` | need the clutch (and how far down) to engage a gear |
| `[Shifter] RevMatchRPM` | allow clutchless shifts within this many rpm of the new gear |
| `[Shifter] Latching` | gear stays selected after its button is released (keyboard, button boxes) |
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
- Input comes from a separate non-exclusive DirectInput instance (the system `dinput8.dll`, never a
  proxy in the game folder) polled at ~500 Hz on its own thread.

`re/` holds the decompilation sources this is based on (dbalatoni13/nfsmw, credits to Brawltendo
for `EngineRacer`) and the annotated disassembly of `OnTaskSimulate`.

## Limitations

- The engine can't stall: the game has no engine-off state and keeps the engine at idle or above.
- Nothing grinds audibly when a shift is refused; the box just stays in neutral.
- The mod hasn't been tested with a physical wheel/shifter yet (only keyboard bindings in game).
