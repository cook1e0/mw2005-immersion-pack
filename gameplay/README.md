# NFS Most Wanted (2005) gameplay mod

Gameplay changes for `speed.exe` v1.3, all optional and tuned in game from the F4 menu. At its
core: real H-pattern gear selection and an analog clutch, working with any DirectInput controller
(wheel shifters, separate pedal sets, button boxes) or the keyboard. Until v0.6.0 this was the
H-shifter mod (`nfsmw_hshifter.ini`, which is renamed to `nfsmw_gameplay.ini` on the first start).

- **H-pattern**: the gearbox follows the stick. Neutral and reverse are real gates. Gates the car
  doesn't have act as neutral.
- **Clutch**: drives the game's own friction-clutch model, so slip, launches, free revving with the
  pedal down and lock-up on release all come from the game's physics. Buttons or keys work as a
  digital clutch that is eased in and out.
- **Clutch required to shift** (optional): moving the stick into a gate without the clutch leaves
  the box in neutral and grinds; that gear only goes in once the stick has been back to neutral
  and in again with the clutch pressed. Optional rev-matched clutchless shifts.
- Reverse is refused while rolling forward (and forward gears while rolling backward).
- **Engine wear** (optional): no rev limiter. Above the redline the engine heats up, loses power
  when hot, and blows at full heat. A blown engine totals the car in a race; in free roam, use
  *Jump to Safe House* from the pause menu. Manual shifting only.
- **Car damage** (optional): crashes into walls, traffic and cops damage the car; at 100% it is
  totaled like a blown engine. Props (cones, barrels, signs) don't count.
- **Damage / heat meter**: the speedbreaker meter next to the tachometer shows car damage, filling
  from the bottom up, with a car icon instead of the clock. Engine heat above 50% fills the same
  bar in red, and at 75% heat or damage the meter pulses and the car icon flashes red. The
  speedbreaker itself is off by default. A spike strip icon above it turns from grey to flashing
  red while a tire is popped.
- **Engine smoke**: a damaged (75%) or hot (50% heat) engine smokes, trailing behind the car.
- **Popped tires**: spike strips leave the car drivable, with less grip and some drag.
- **No SUV cops** (on by default): no SUV police at all; heavy support is skipped and SUV
  patrols are replaced by other cars from the heat level.
- **Physics sliders**: turn down the game's hidden driving aids (traction control, stability,
  drift assist) so front, rear and all-wheel drive cars handle differently, and set grip,
  the front torque share and gravity, for all cars or per car. Only your car is affected
  (gravity optionally all cars).
- **Wheel and pedals**: steering, accelerate, brake and the driving buttons (handbrake, nitrous,
  speedbreaker, reset, camera, look back) are bound in the F4 menu and handed to the game as its
  own input, so the game's controller settings aren't needed. *Steering lock* sets how much of
  the wheel's rotation gives full lock in game (e.g. 540 of 900 degrees); the force feedback mod
  adds a soft lock there. *Game's own controller input: Off* makes the game ignore the
  controllers completely, its menus included (needs the force feedback mod;
  the keyboard keeps working).
- **Menu controller input**: analog axes are ignored in the game's menus by default, so pedals
  and steering cannot scroll them. Buttons and the d-pad still work. F4 > Controls >
  *Controller input in game menus: Off* disables those too; *Analog axes in game menus: On*
  restores axis navigation. Driving and the F4 menu's own bindings are unaffected. Requires
  the updated force feedback mod; it also works with its `NativeFFB=1` setting.
- **Tuning menu**: F4 while driving. Engine wear, damage, shifter and clutch settings, traffic,
  the force feedback mod's strength and centring spring, and the VR mod's view recentre and HUD
  panel size, distance and height (when those mods are installed), changed
  live and saved to the ini, and a CONTROLS section to bind gears, clutch and menu keys by
  pressing them. Works with a wheel's d-pad and shows on the HUD panel in VR.
- **Force feedback restart**: F4 > Force Feedback > *Restart wheel force feedback* requests
  a wheel reacquisition and effect rebuild from the updated force feedback mod, keeping
  calibration and tuning. F10 provides the same restart directly from the FFB mod.

## Install

Download the zip from the [latest release](https://github.com/cook1e0/nfsmw05-mod/releases/latest) and copy `nfsmw_gameplay.asi` and `nfsmw_gameplay.ini`
into the `scripts` folder next to `speed.exe` (with the zip's `dinput8.dll`, Ultimate ASI Loader, next
to `speed.exe`). Without a loader, rename `nfsmw_gameplay.asi` to `dsound.dll` and put it and the
`.ini` next to `speed.exe`. Then, with your pedals and shifter connected, start a drive, press F4 and bind
the clutch and each gear under CONTROLS: select one, press Right, then press the pedal or put the
stick in that gear. To uninstall, delete `nfsmw_gameplay.asi` (or `dsound.dll`).

The game's Automatic/Manual setting doesn't matter while the shifter is active. `Toggle=` (e.g.
`key:F9`) switches between the H-shifter and the game's sequential shifting while driving.

## Settings

All in `nfsmw_gameplay.ini` (commented). The ones you are most likely to change:

| Setting | |
|---|---|
| `[Clutch] BiteStart / BiteEnd` | pedal travel where the clutch starts to grip and grips fully |
| `[Clutch] Strength` | clutch friction relative to the game's clutch; lower = longer slip |
| `[Shifter] RequireClutch / ClutchToShift` | need the clutch (and how far down) to engage a gear |
| `[Shifter] MisshiftChance / MisshiftDamage` | a gear refused for lack of clutch: chance (0.5) of this much car damage (0.05 = 5%); needs `[Damage] Damage=1` |
| `[Shifter] RevMatchRPM` | allow clutchless shifts within this many rpm of the new gear |
| `[Shifter] Latching` | gear stays selected after its button is released (keyboard, button boxes) |
| `[Shifter] Sequential / ShiftUp / ShiftDown` | sequential shifting instead of the H-pattern, and its up/down bindings (Left Shift / Left Ctrl) |
| `[Engine] EngineWear / RevLimiter` | engine heat and blow-ups on/off; keep the rev limiter |
| `[Engine] HeatUp / HeatUpMaxRPM / CoolDown` | heat per second at the redline / at max rpm, and cooling below it |
| `[Engine] PowerLossStart / PowerLoss` | heat where power starts dropping, and how much is lost at full heat |
| `[Engine] Totaled` | 0 = a blown engine shows "Blown Engine" instead of totaling the car |
| `[Damage] Damage / MinImpact / Strength` | car damage on/off, harmless impact size, damage per hit |
| `[HUD] Meter / Speedbreaker` | damage/heat meter in place of the speedbreaker meter; keep the speedbreaker |
| `[Physics] TractionControl / Stability / DriftAssist` | driving aid strength, 1 = the game's own |
| `[Physics] Grip / TorqueSplit / Gravity` | tire grip, multiplier on the torque sent to the front wheels, gravity in g |
| `[Car.<name>]` | a car's own physics values (set with *Settings for this car only* in the menu) |
| `[Controls] Steering / Accelerate / Brake / Handbrake / ...` | driving controls read by this mod instead of the game |
| `[Controls] WheelRotation / SteeringLock` | the wheel's rotation set in its driver, and the part of it that gives full lock in game |
| `[Controls] SteeringLinearity` | steering response curve: 1 = linear, higher = finer around the centre |
| `[Controls] SpeedSensitiveSteering` | 0 (default): a bound wheel steers directly, full lock (45 deg) at `SteeringLock` at any speed; 1: the game's speed-sensitive wheel steering. Without a bound wheel the game steers as with a gamepad (a few degrees of lock at speed, slow ramp) |
| `[Controls] GameController` | 0 = the game ignores the controllers (driving and menus); bind them here instead |
| `[Controls] MenuController / MenuAxes` | game menus only: buttons/d-pad on by default; analog axes off to prevent pedal scrolling (needs FFB mod) |
| `[Controls] RecenterVr` | recenters the VR view (the VR mod's F8), from any key or button |
| `[UI] Menu / MenuUp / ...` | tuning menu bindings |
| `[General] LogInputs` | log stick position and clutch travel to `nfsmw_gameplay.log` |

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
- Driving controls: `SimSystem::DoFetchInput` (0x6E8AC0) is redirected, and before each
  `InputPlayer::FetchInput` the bound steering, pedals and buttons are queued into the player's
  `ActionQueue` (+0x10) as the game's own actions (`ActionQueue::ReceiveAction`, 0x6440B0:
  `{id, port, value}`, ids from the GAMEACTION_* table). On PC one `GameDevice` merges keyboard
  and joystick, so `GameController=0` is done by the force feedback mod's DirectInput proxy, which
  gives the game an untouched reading of every controller.
- The tuning menu is [Dear ImGui](https://github.com/ocornut/imgui) (v1.91.9b, vendored unmodified
  in `third_party/imgui`, MIT) with its own renderer: `DrawIndexedPrimitiveUP` and a MANAGED font
  texture (DYNAMIC/DEFAULT on D3D9Ex), so device resets need nothing from us. The mouse wheel comes
  from a chained window procedure, since the game reads the mouse through DirectInput.
  `[UI] ClassicMenu=1` brings back the old flat menu.
- The tuning menu is drawn after the game's HUD (eDisplayFrame, 0x6E75A7) as untextured quads with
  a GDI-rasterised font, so the VR mod puts it on its HUD panel.
- Input comes from a separate non-exclusive DirectInput instance (the system `dinput8.dll`, never a
  proxy in the game folder) polled at ~500 Hz on its own thread.

Based on the dbalatoni13/nfsmw decompilation (`EngineRacer` by Brawltendo).

## Building

MSYS2 with the mingw32 toolchain: `./build.sh` builds into `build/`, `./build.sh install` also
copies the files into `C:\Games\NFSMW` (or `$NFSMW_DIR`).

Run `./tests/run.sh` for axis binding, smoke selection and legacy settings migration
regressions. The harness uses synthetic game addresses and temporary INI files under `build/`;
it does not launch the game or access controllers. If shipped INI defaults change, update
`kGameplayDefaultIniFingerprint` to the value reported by this test.

When upgrading from the old H-shifter mod, an untouched gameplay INI from the package is
backed up as `nfsmw_gameplay.ini.pre-hshifter.bak` before importing the legacy settings.
A customised gameplay INI is kept, with the old shifter INI retained for manual merging.

## Limitations

- The engine can't stall: the game has no engine-off state and keeps the engine at idle or above.
- A refused shift plays the gear grind of the intro's sabotaged BMW (`[Shifter] GrindSound`, a
  sound moment key; 0 = silent), with the drag race missed-shift exhaust pop.
- `TorqueSplit` multiplies the car's own front share, so a rear-wheel drive car (0% front) stays
  rear-wheel drive.
- Not yet tested with a physical wheel and shifter, only keyboard bindings.
- Car damage: the player's `DamageRacer` keeps the game's damage total (`IDamageable::GetHealth`),
  which racers never fill. `DamageVehicle::OnImpact` (DamageRacer vtable 0x8AD378) is replaced to
  add impact force / mass beyond `MinImpact` to it, skipping smackables. At 100% the car is
  destroyed through `IDamageable::Destroy`.
- HUD meter: `ISpeedBreakerMeter::SetPursuitLevel` (0x8A2110) gets the damage instead of the
  speedbreaker charge. The heat overlay is a clone of the meter's masked arc
  (`FEMultiImage::Clone`) added to the HUD package next to it; the clock icon gets the
  `COPS_ICON` texture. `LocalPlayer::CanDoGameBreaker` (0x6EDC90) turns the speedbreaker off.
- Physics sliders: `SuspensionRacer::OnTaskSimulate` (0x6B1DD0) is detoured. For the player's
  car, `Tweak_GlobalGripScale`, `Tweak_TractionFowardSlip` (traction control), `MaxYawBonus` and
  `DriftStabilizerTable` (stability), `DriftRearFrictionTable` (drift assist) and the tires'
  `GRIP_SCALE` per axle are swapped for the slider values during the call and restored after.
  Gravity: `RigidBody::OnBeginFrame` (0x6A7050) scales `rigidbodyspecs.GRAVITY` the same way.
