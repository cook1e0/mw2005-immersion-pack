### v0.13.0: direct wheel steering, turning steering wheel, new force feedback

- **Wheel steering:** a bound wheel used to reach the game as a gamepad: a few degrees of front
  wheel lock at speed (40 parked, ~3 at top speed) and a slow steering ramp, so half the wheel's
  rotation barely steered. The game's own real-wheel mode is now used: the front wheels follow
  the wheel directly, full lock (45 degrees) at `[Controls] SteeringLock`, at any speed. F4 >
  Controls > Speed-sensitive steering restores a lock that shrinks with speed.
- **Turning steering wheel:** the player car's steering wheel turns with yours, by the same angle,
  in every car with a modelled wheel (found automatically in the interior model; 36 of 38 cars).
  `[VR] SteeringWheel=0` turns it off. Works in VR and on a flat screen.
- **Force feedback:** new speed-sensitive centring (`-K * v * sin(angle)`), understeer lightening,
  surface rumble on grass/dirt/gravel from the game's surface data, kerb kicks from the body's
  vertical jerk (hills don't trigger them), directional impacts (head-on shake, side kick), wall
  scrapes, a limp wheel in the air and a landing jolt. All in F4 > Force feedback.
- **VR: driver's arms and hands hidden:** in the cockpit the driver model's arms and hands are left
  out (they followed neither your hands nor the turning steering wheel). F4 > VR > Hide the
  driver's arms and hands, `[VR] HideArms`. Found per driver model (in front of the head, above the
  lap, beside the chest), checked against every car.
- **VR: reflections no longer turn with your head:** the glow and specular passes (the bloom and the
  wet-road / paint highlights laid over the image) share the player's camera but were set up once
  per frame from the game's camera; each eye now sets them up from its own view.
- **VR: car and window reflections stay put when you move your head:** the game places the
  reflection viewpoint 2.78 m from each car and model in the direction of the camera. In the
  cockpit the camera sits close to the car's centre, so a few centimetres of head movement swung
  that direction around and every reflection with it. The eyes now use the seat position for it.
- **Force feedback effects:** engine rumble (at the engine's revs, mostly standing still), kerbs and
  bumps (a kick toward the front wheel that was hit from below, and a short shake), the game's road
  surface rumble, crash jolts from the game's own collision reports (cars, walls, and props such as
  benches, sign posts and fences) and wall scrapes (grinding and a pull toward the wall). Each has
  its own slider in F4 > Force feedback > Effects.
- **No more buzzing standing still:** the friction flipped direction with every tiny wheel
  movement; it is now smooth through standstill.
- **Centre weight:** once moving, the wheel pulls back to straight ahead from a few degrees off,
  so you hold it against the pull instead of it staying wherever you leave it (F4 > Force feedback
  > Centre weight). Friction is lighter at speed.
- **Steering weight with speed-sensitive steering:** the tyre force used the full 45 degree lock
  even when the game allowed only a few degrees at speed, so it peaked a quarter of the way in and
  went light beyond. It now uses the game's actual lock and keeps building as you turn in.
- **Speed-sensitive wheel steering rewritten for a wheel:** the game's version was made for pads.
  Its lock grew up to 1.4 times the further the wheel was turned and up to 1.7 times off the
  throttle or on the brakes (progressive steering, lifting tightened a turn), its speed table
  kinked (45 degrees parked, 15 at 64 km/h, then 11, 8, 7), and while countersteering it widened
  the lock to the rear tyres' slip angle and dropped it back the moment the wheel crossed centre,
  so the front wheels jumped as a slide was caught. Now the lock is one smooth curve of speed
  (F4 > Controls > Speed sensitivity, 85% is close to the game's own values) times the car's
  steering tuning; the front wheels always follow the wheel in proportion, and in a slide the
  lock widens with the slide on both sides alike. Set Steering linearity back to 1.0 if you used
  it to compensate. The drift assist and stability control are left as the game has them.
- **Force feedback: breakable objects:** smashing a bench, sign post, fence or bin gives a sharp
  knock toward that side (F4 > Force feedback > Breakable objects, `[FFB] Breakables`). These are
  recognised as the game's own breakable props (Smackables), by their class in the collision
  report or when one is knocked loose where your car is, not guessed from light contacts.
- **VR: reflections sit still when you move your head:** the car paint and window shaders turn
  their reflections with the view's rotation (WorldView), which in the headset is your head's. While
  the eyes render, that one matrix now comes from the seat (the game camera without your head),
  so reflections stay put as you look around; positions are untouched. `[VR] StableReflections`.
- **VR: working side mirrors:** in the headset the game's rear view mirror image is shown on the
  player car's side mirrors instead of at the top of the HUD (hidden in VR whatever the game's
  option). The mirror glass is found in each car's model (checked offline on every car, including
  the few that have their mirrors in the body); the left mirror shows the left half of the rear
  view, the right one the right half. `[VR] Mirrors`, `MirrorSplit`, `MirrorFlip` (if the image
  is the wrong way round), `MirrorBrightness`. The image is taken from the rear view's own render
  target (the first try read the HUD's texture and showed black).
- **Drivetrain multiplication:** F4 > Physics > Drivetrain multiplication (`[Physics] Drivetrain`,
  also per car) exaggerates how the driven wheels lose side grip under power: at 2x a rear drive
  car's power oversteer and a front drive car's understeer are doubled, all-wheel drive cars get
  both by their torque split; below 1 it softens them. It scales the game's own tyre model (the
  forward force a driving tyre adds against its grip limit), for your car only.
- **A wrecked engine runs hot:** from 75% car damage the engine heats up a third faster (with
  engine wear on); cooling is unchanged.
- **Drag races playable with the mods:** a drag race gave your car the drag driver style, which
  swaps in the game's drag parts: steering became automatic lane changes, and the drag engine and
  damage (totaled by a hit) replaced the normal ones, so the wheel, shifter, engine wear and damage
  mods did nothing. Your car now keeps the normal style: you steer, with the normal engine,
  shifting and damage. Opponents and the race itself (staging, drag HUD, finish) are unchanged.
  F4 > Traffic and HUD > Drive drag races normally, `[Races] NormalDragRaces`.
- **Damage meter car icon back to the game's look:** it was drawn opaque with a black outline,
  which looked harsh. It now uses the HUD's own colours: gray with no damage, the HUD's beige when
  damaged, pulsing red like the damage arc from 75%. The spike strip icon shows only while a tyre
  is popped, flashing red, without the outline.
- **VR: no more occasional cutscene camera at pursuit breakers:** the camera director had other
  ways off the driving camera (the cinematic camera when a moment or the game asks for it). In VR
  the player's camera is now turned straight back to the driving camera whatever picks another,
  except during the game's own cutscenes (race intros, busted).
- **Force feedback status:** F4 > Force feedback starts with a status line: "Running on '<wheel>'"
  in green, or what is holding the force up (game window not in front, no device opened yet,
  device not acquired, ...). Status changes are written to `nfsmw_ffb.log`.
- **Force feedback on the Fanatec CSL DD:** the wheel reports nonsense force feedback states
  (random flags, "power off" and "safety switch off" among them), which made the mod think the
  safety switch was off and send nothing. Impossible states are now ignored.
- **F4 menu lag on the force feedback page fixed:** the device list was looked up (opening every
  controller) on every frame; it is cached now.
- **VR: no raindrops on the screen** in the headset (they stuck to your eyes); the rain itself
  stays. `[VR] ScreenRain=1` brings them back.
- **Force feedback rebuilt from scratch for direct drive wheels:** a new modular core
  (telemetry, force model, safety filters, device). Steering weight comes from the front tyres'
  slip angle and load (light past the grip limit, pulling into the countersteer in a slide), with
  a speed-sensitive centring spring, damping and friction computed by the mod. Everything goes out
  as one constant force through a safety chain: smoothing of the game's 60 Hz physics, a soft
  limiter instead of clipping, a slew-rate limit, an oscillation guard, a force ceiling
  (`MaxForce`) and a fade-in. Every change restarts the effect (`RestartEffects`, needed by
  Fanatec bases). Old FFB settings are replaced; road effects, jumps, crashes and wall scrapes
  return in the next steps.
- **ffbtool:** no longer loads the game's dinput8.dll (the ASI loader, which pulled the mods in),
  opens its own window (works from any terminal), and tests each effect type on its own.
- **Steering sensitivity:** F4 > Controls > Steering sensitivity scales how far the car steers for
  a given wheel angle (`[Controls] SteeringSensitivity`).
- **FFB device:** wheels that show up as two FFB devices of the same name (Fanatec) now use the
  first one in Windows' order (as joy.cpl lists them). F4 > Force feedback > Wheel device lists
  them by name, axes and buttons and switches live (`[FFB] Device`). Test force pushes the wheel
  right for half a second.
- **Pursuit breaker camera off in VR:** pursuit breakers no longer cut to the camera watching the
  cops; the view stays in the cockpit.
- **VR seat bindings:** F4 > Controls > VR seat binds wheel or button-box buttons to move the
  cockpit camera forward/back, left/right, up/down (held) and back to the driver's head, like the
  numpad keys. Saved per car.

**Upgrading from v0.12.1:** replace all three plugins and `ffbtool.exe`. Keep your `.ini` files.

### v0.12.1: cockpit crash and garbage polygons fixed, settings kept

- **VR cockpit crash:** the VR mod's driver-model cache was never refreshed. After the game
  streamed a car model out and loaded another at the same address (traffic, car resets, race
  restarts), the cockpit kept drawing with the freed model's vertex buffer: big dark garbage
  polygons in the cockpit, then a crash inside `d3d9.dll` when the game released its model
  buffers (seen right after opening F4). Changed or reloaded models are now prepared again, and
  the head-less copy of a draw entry always uses the live vertex buffer.
- **Near plane default:** `[VR] NearZ` now defaults to 0.05 m (the game's own is 0.5 m); 0.01 m
  cost too much depth precision. Existing ini values are kept.

**Upgrading from v0.12.0:** replace `scripts
fsmw_vr.asi` and `scripts
fsmw_gameplay.asi`.

### v0.12.0: cockpit near plane, VR recenter binding, misshift damage

- **VR near clip plane:** the player view's near plane is now `[VR] NearZ` (default 0.01 m, was
  the game's own 0.5 m), so the dashboard, wheel and pillars right in front of the eyes are no longer
  cut off. Adjustable live in F4 > VR > Near clip plane. 0 in the ini restores the game's value.
  Very low values can make distant surfaces flicker (depth precision).
- **Recenter VR view binding:** F4 > Controls > Recenter VR view binds any key, wheel or button
  to the VR mod's recentre (same as F8). Works while driving and in the game's menus.
- **Misshift damage:** a shift refused for lack of clutch has a 50% chance of doing 5% car damage
  (`[Shifter] MisshiftChance`, `MisshiftDamage`, also in F4 > Shifter and clutch). Needs car damage
  on; `MisshiftChance=0` turns it off.

**Upgrading from v0.11.2:** replace `scripts
fsmw_vr.asi` and `scripts
fsmw_gameplay.asi`. Keep
your existing `.ini` files and bindings; the new settings use their defaults when missing, so the
lower near plane and misshift damage are on after the update. The force feedback plugin,
`nfsmw_vr_host.exe` and `nfsmw_crashdump.exe` are unchanged. The near-plane slider needs both
updated plugins.

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
