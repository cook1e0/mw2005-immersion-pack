### Unreleased: car mirrors and in-car dashboard in VR, grip sliders, spin limit, new force feedback jolts

**VR**

- **The car's own mirrors show what's behind you:** the game's rear view is drawn on the interior
  rear view mirror (found in front of the driver's head in every car's interior but the GTO's),
  inside a dark rim as if set into its housing. The HUD's rear view mirror is hidden in the
  headset. `[VR] Mirrors`, `MirrorFlip`, `MirrorScale`; F4 > VR > Mirror brightness / Mirror
  contrast; F4 > Menu and testing > Rear view mirror forward / back moves the picture per car.
  With `[VR] GameMirrors=1` the side mirrors' glass shows the rear view too (a fixed slice each,
  `SideMirrorWidth`), also on cars whose glass is part of the body (BMW M3 GTR); cars listed in
  `GameMirrorsSkip` keep the game's glass. Off by default: the side mirrors are the game's own.
- **In-car dashboard:** from the driver's seat the HUD's minimap, speedometer, cop radar, pursuit
  info, leaderboard and messages move onto a display over the car's gauge cluster, and the floating
  HUD is hidden (back in the pause menu and the F4 menu). F4 > VR > In-car dashboard (`[VR] Dash`):
  the display's size, position, angle and depth are kept per car model; its colour, a glass sheen
  and a reflection of the world behind you are adjustable. F4 > VR > Adjust dashboard places each
  element live (the layout is shared by all cars, `[DashLayout] Default`) and sets which part of
  the HUD it shows.
- **Triangle editor** (F4 > VR > Triangle editor): look at a part of your car and hide it, draw it
  black (the car's own gauges under the display), or put it on or take it off the steering wheel;
  restore and undo. Saved per car part in `nfsmw_vr_edits.ini`. F4 > Menu and testing > Center
  steering wheel animation fits the wheel's turning centre, adjustable per car.
- **No more z-fighting far away with a low near plane:** in the driver's seat the depth buffer is
  split: your own car is drawn with `NearZ` in its own small part of the depth range, the world with
  `[VR] WorldNearZ` (0.3 m) in the rest. `[VR] DepthSplit=0` restores one near plane.
- **HUD:** F4 > VR > HUD left / right (`[VR] HudRight`); "Each car remembers HUD positioning"
  (`HudPerCar`) keeps size, distance, height and side per car model (a model you haven't set up
  starts from the defaults); "Steering wheel rendered in front of HUD" (`WheelOverHud`) draws the
  car's wheel over the HUD panel while driving.
- **Reflections sit still when you move your head:** car paint and window reflections use the
  seat's view, not the head's (`[VR] StableReflections`).
- **Both eyes see the same lights:** the game's per-frame display step (blinking lights, flares,
  fades) runs once per frame, and each eye collects its own light flares instead of drawing the
  first eye's again.
- **Faster frames:** the rear view and the reflections' environment map are rendered once a frame
  for both eyes; two of the environment map's six faces are refreshed per frame
  (`[VR] EnvMapFacesPerFrame`, 6 = all), the rear view optionally every other frame
  (`RearViewEvery`). Mirror shaders, dashboard textures and a reusable state block are built while
  the game loads instead of in the frame they first appear; the dashboard's HUD probe doesn't wait
  for the GPU; streamed-in car models reuse their steering wheel and are checked once a frame.
- **Frame timing in the log:** every 5 s `nfsmw_vr.log` gets `perf:` lines (fps, time spent waiting
  for the headset, rendering the eyes, waiting for the GPU, submitting, the spectator window,
  presenting, and in the game itself, with the window's worst frame), and a `hitch:` line for a
  frame half again as long as the average. `[VR] LogPasses=1` adds render target statistics.
- **No cutaways from the cockpit:** the camera director's other cameras, the slow-motion moment
  camera and the pursuit breaker cutscene (which froze the world and placed the car again) are
  refused in the headset; the busted cutscene comes without its viewfinder frame; a career race's
  finish no longer stays in slow motion. Story cutscenes still play.
- **Less in your face:** no spinning direction arrow above the car (`[VR] DirectionArrow`), no sun
  glare blob or sun lens flare on your eyes (`SunGlare`). A flat screen shows both as before.
- **The view no longer turns over with the car:** with the horizon levelled, a car on its roof
  kept the view upside down; "down" now always points at the ground.
- **Your car stays drawn while seated:** looking down or leaning in a low car (IS300) made the game
  think the camera was outside it, and the whole car vanished.
- **Hide arms:** hands split into a draw entry of their own, and hands low on the wheel in low cars
  (Gallardo), are hidden too.

**Gameplay**

- **Physics: spin limit (fixes the pinball):** a car spinning fast (launched by props, landing while
  rotating) could get caught in a collision loop and be thrown across the map. Your car never spins
  faster than F4 > Physics > Spin limit (`[Physics] MaxSpin`, 8 rad/s), with the torque limited too.
- **Physics: new grip sliders,** all per car with "Settings for this car only":
  - Rear grip (oversteer): the rear tyres' grip relative to the front.
  - Offroad grip: grip on grass, dirt, mud, gravel and sand (75%).
  - Slide grip: a tyre that has broken loose slides on less grip than its peak (85%).
  - Slide oversteer: the rear tyres' side grip under throttle, as on grass (front drive cars:
    lifting off), coming back as the slide passes 20-45 degrees, so it stays catchable (80%).
  - Rain grip: 10% less grip in the rain.
  - Grip loss at speed: cornering grip falls with the square of the speed; launching and braking
    keep their grip (`[Physics] SpeedGripLoss`, the share gone at 300 km/h, off by default).
- **Sequential shifting has neutral and reverse:** with manual shifting, down from first is neutral,
  then reverse; gas drives in reverse, and holding the brake no longer reverses.
- **Engine heat:** nitrous on the rev limiter heats the engine twice as fast (a third faster on a
  car over 75% damaged). No heat for 8 s after a race start, restart or respawn (a rolling start in
  the shifter's gear blew the engine at challenge race starts), nor in cutscenes.
- **Challenge Series time trials hand the car back:** the race script gave your car to the AI at
  the start and never took it back. The game's own hand-back now runs as soon as you press a pedal
  or steer, and 3 s after the start at the latest (`race start:` lines in the log).
- **First safehouse visit:** a career whose "intro done" mark was set at the prologue's bin (a
  prepared profile) skipped Mia's intro and could not go on; the mark is cleared, and the safehouse
  door starts the intro if the career script didn't. Story movies play even when the widescreen
  fix's intro skip left movie skipping on.
- **HUD:** an engine warning icon flashes above the car icon from `[HUD] HeatIconFrom` (50%) heat or
  with a blown engine (the spike strip icon moves to its left and shows only with a popped tyre);
  the meter's arrow glows with the car icon; the meter fills all the way to the arrow; no
  infractions, cost to state or area name popups (`[HUD] Infractions`, `CostToState`,
  `LocationNames`); free roam's totaled message is just "TOTALED".
- **Drag races with the normal race HUD** (`[Races] NormalDragHud`, on), the turbo dial's needle in
  the right place on it.
- **No head-on ramming SUVs** (`[Traffic] NoHeavySupport`, replaces `NoSuvCops`): only heavy
  support is dropped; SUV patrols and roadblock SUVs come as the game sends them.
- **Clutter gone:** the EA Trax "now playing" banner and the loading screen's red "EVENT HANDLER"
  developer text.
- **Smoke:** engine smoke no longer hangs in the air where it stopped; a missed shift no longer
  leaves a smoke trail (the grind sound stays).
- **Steering never reads past full lock:** a wheel axis reporting values outside its range is
  clamped.
- **Testing: Drop from air** (F4 > Menu and testing) lifts your car `[Tires] DropHeight` (5 m) and
  lets it fall.

**Force feedback**

- **New jolts:** breakable props (benches, sign posts, fences, bins: the game's own breakable
  objects, `Breakables`), landing after a jump (`Landing`), and hitting another vehicle (traffic,
  cops, racers, told apart by the game's object type, `CarHits`), which ducks the other short
  effects while it plays. Vehicle hits and breakables are at full strength from 150 km/h.
- **Off-road vibration** on grass, dirt, mud, gravel and sand, by the surface's own name under each
  front tyre (`OffroadVibration`).
- **Lighter steering with nitrous** (`NosLighter`, 30%).

### v0.13.0: direct wheel steering, turning steering wheel, new force feedback

- **Wheel steering:** a bound wheel used to reach the game as a gamepad: a few degrees of front
  wheel lock at speed (40 parked, ~3 at top speed) and a slow steering ramp, so half the wheel's
  rotation barely steered. The game's own real-wheel mode is now used: the front wheels follow
  the wheel directly, full lock (45 degrees) at `[Controls] SteeringLock`, at any speed. F4 >
  Controls > Speed-sensitive steering restores a lock that shrinks with speed.
- **Turning steering wheel:** the player car's steering wheel turns with yours, by the same angle,
  in every car with a modelled wheel (found automatically in the interior model; 36 of 38 cars).
  `[VR] SteeringWheel=0` turns it off. Works in VR and on a flat screen.
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
  damaged, pulsing red like the damage arc from 75%. The spike strip icon matches it: always
  shown, gray, flashing red while a tyre is popped, no outline.
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
  Fanatec bases). Old FFB settings are replaced. Jump lightening and landings are not in yet.
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

**Upgrading from v0.12.0:** replace `scripts\nfsmw_vr.asi` and `scripts\nfsmw_gameplay.asi`.

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

**Upgrading from v0.11.2:** replace `scripts\nfsmw_vr.asi` and `scripts\nfsmw_gameplay.asi`. Keep
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
