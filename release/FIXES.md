### Unreleased: force feedback from the tyres, ABS and traction control, physics presets, car mirrors and in-car dashboard in VR, grip sliders, spin limit, new force feedback jolts

- **The pack is now called Most Wanted 2005 Immersion Pack Mod:** the README and the release zip
  (`mw2005-immersion-pack-<version>.zip`) carry the new name. The files inside keep theirs
  (`scripts\nfsmw_*.asi`, `.ini`, logs), so unpacking over an older version still replaces it.
- **Logs: debug chatter removed:** the VR perf / pass / eye scene reports, view table and
  shared view dumps, flare pool counts, texture filter statistics, frame memory peaks and the
  host's XR timing lines; the gameplay mod's shift, engine wear and heat, input block, menu, save,
  picking, icon and physics step lines. Setup results, warnings, errors and the VR `hitch` lines
  stay. The frame trace key (`[VR] KeyTrace`) is off by default; `[FFB] LogTelemetry` and
  `[VR] LogPasses` stay opt-in.

**VR**

- **Per-car cockpit placement and triangle picks (by spinesplitter):** seat offsets, HUD and
  in-car dashboard placement, dashboard layouts, steering wheel turning centres and the rear view
  mirror offset for the cars he set up, and his triangle editor picks (`nfsmw_vr_edits.ini`, now in
  the zip: which triangles are the steering wheel, hidden or drawn black, 53 car models).
- **VR starts with RivaTuner running:** RivaTuner's `RTSSHooks.dll` can hook the game's Direct3D
  import before the VR mod loads; the VR mod then took the game for another version and switched
  itself off (no cockpit camera, "VR inactive" in the log). It now recognises the game's own import
  and creates the device from Windows' `d3d9.dll` past that hook.
- **The Clio's steering wheel is found:** its low-poly, thick rim spread over the wheel search's
  2 cm slices, so no wheel was found (no turning wheel, no in-car dashboard, which is placed from
  the wheel). A wheel the usual search misses is looked for again with 4 cm slices; every other car
  in the game is found by the usual search, as before.
- **Rear view mirrors on the CTS and the Eclipse:** the search region now starts 15 cm ahead of
  the head (the CTS's driver sits close to the mirror). The Eclipse's glass is a shallow faceted
  shell whose triangles don't share edges, and the back of its housing is a few millimetres behind
  it: growing a patch from shared edges found only a single facet, or a patch folded together with
  the housing. Now the largest triangle facing the driver seeds a flat patch of everything lying in
  its plane, either winding, and the patch must still look like the mirror. A piece elsewhere in
  that plane (a scrap of the Punto's dashboard) leaves the glass to the connected search, which
  stays as the fallback for every other car. The glass is drawn as its outline, fanned from its
  convex hull, so overlapping or folded triangles can't make a broken shape.
- **The road is back in the mirrors:** skipping the sun glare in the headset skipped the whole
  pass that draws it, and that pass ends by turning depth testing back on (it turns it off for its
  quad). Without it, what rendered next - the rear view among it - drew with no depth test, and the
  sky covered the road in the mirrors. The skip now turns depth testing back on itself whenever the
  pass would have drawn.
- **Anisotropic filtering:** textures at an angle (the road ahead, walls) stay sharp. Samplers the
  game sets to linear minification are switched to anisotropic, except with a depth texture bound (the
  shadow map, whose hardware comparison needs linear): `[VR] Anisotropic` 0 off, 1 2x, 2 4x, 3 8x,
  4 16x (default), also in F4. Every 30 s the log counts the filters the game set and how many were
  made anisotropic.
- **Shadow resolution in F4:** F4 > Menu and testing > Shadow resolution sets the widescreen fix's
  `ShadowsRes` (x1 = the game's 1024 .. x16), written into its ini for the next start.
- **Hidden driver parts can be brought back with the triangle editor:** with Hide arms / Hide
  head the seated driver's arms and head are left out of the draw (their index ranges are skipped),
  so the editor's Restore had nothing to undo for them and pieces wrongly counted as arms stayed
  hidden. Restore on the driver's triangles now puts them on a per-model `Show` list in
  nfsmw_vr_edits.ini, and they are drawn again inside the hidden parts (Hide takes them off it).
- **The shared shadow map now actually stays on:** its per-frame check switched it off at every
  race start (`shadow matrices differ ... by 0.97`), so the map was still drawn for each eye. Only the
  camera's position had been made the same for both eyes; the pass now gets the whole head-free seat
  camera (matrix, direction, target too). The check compares only what the pass writes (view 15's
  matrices, the shadow projection, the light), names the value that differed, and a difference
  pauses sharing for a second instead of ending it; five within a minute end it for the session.
- **Hitch diagnosis in the logs:** the `perf` and `hitch` lines have an `update` phase (the game's
  own update, from Present to the next scene render), and the 64-bit host logs `XR timing` every 5 s
  (display slots the headset skipped, and how long xrWaitFrame and xrEndFrame took).
- **The sun shadow map is drawn once a frame:** it was the biggest pass by far - up to 6,800 of
  10,200 draws a frame in traffic, because the game draws every caster in a ~350 m square and the
  mod ran it for each eye. Shadows are the same for both eyes, so both eyes now run the shadow pass
  (`sub_6E44C0`, through `sub_6E54E0` at 0x6DEC72) with the seat's position instead of the eye's, which
  makes its matrices identical, and the second eye's pass keeps all its game state but has its
  draws and clears dropped, so it reuses the first eye's map. Every frame the second eye's shadow
  matrices (views 15/16 and the shadow texture matrix) are compared with the first eye's; any
  difference turns sharing off with a line in the log. `[VR] SharedShadows`.
- **Smaller shadow map area:** `[VR] ShadowArea` scales the shadow camera's 10 degree FOV
  (0x8FAEE0); 0.75 by default (~260 m), so fewer objects cast shadows into it and they are sharper.
- **Frames handed to the headset without waiting:** with the 64-bit bridge (SteamVR) the
  EndFrame is posted and the next frame's BeginFrame takes its reply, so the game no longer waits
  about a millisecond a frame for the hand-over. `[VR] AsyncSubmit`.
- **The shared shadow map stays on:** after a frame where the eyes' shadow matrices differed,
  sharing pauses for a second while each eye draws its own map. The check went on counting
  differences during that pause, when nothing is shared and per-eye maps may differ, so the first
  frames after every spawn turned sharing off for the whole session (seen in every log today:
  "5 mismatches in 3600 frames"). Only frames that actually reused the first eye's map count now;
  a difference while paused only keeps the pause going. Both eyes still see the same map.
- **Per-frame trace (debug):** Delete (`[VR] KeyTrace`, 0 = off) records 30 s of frames,
  one CSV line each, to `nfsmw_frametrace_NNN.csv` next to speed.exe: every phase's time, each eye's
  CPU time, draws and GPU time (timestamp queries), the frame's GPU span, the game time stepped
  (physics steps), the reflection faces and rear view drawn, the shadow map (per eye / shared /
  paused) and the HUD probe. Measurement only: nothing is drawn differently, and the GPU queries
  exist only while a trace runs. The file is written by a thread of its own when the 30 s are up.
- **Shared shadow map stays on all race, with one set of matrices for both eyes:** the sun's
  direction drifts a little between the two eye passes (~1e-4; the shadow camera sits 2000 m up it),
  so every 10-20 s the second eye's shadow matrices came out a hair off the first's, and five of
  those ended sharing after about a minute. On a shared frame the second eye now takes the first
  eye's shadow matrices and light outright (view 15's matrices, the shadow projection, the light),
  so both eyes draw with one map and one set of matrices. The second eye's own pass came out 1-25%
  off in the next session too, so there is no comparison fallback any more: every stereo frame the
  second eye uses the first eye's map, matrices and light, always the same shadow in both eyes (only
  a first-eye value that is not a number makes the second eye draw its own map, for that frame).
  Differences are still logged (up to 10 lines) for diagnosis.
- **Render thread scheduled on time:** the game's render thread (eyes and the hand-off to the
  headset) registers as a "Games" multimedia task and sets the system timer to 1 ms, once
  (`[VR] RenderPriority=1`, 0 = off). The missed headset slots came with ~5 ms of headroom, which
  points at the thread being run late. The eyes are drawn exactly as before.
- **The GPU wait no longer burns a core:** it spun on the fence at full speed every frame before
  the hand-off. It now spins for 1.5 ms (the usual wait is ~0.5 ms), then hands its core to any other
  ready thread between checks (the game's, SteamVR's, Link's), with no added delay when none waits.
- **Physics rate follows the headset:** the widescreen fix sets the game's physics rate from its
  [MISC] SimRate at start, and -1 (its setting here) means the desktop monitor's refresh rate: ~103 Hz
  against the headset's 72, so each frame got one or two physics steps in an uneven pattern and
  motion moved in steps (climbing a slope felt like stairs). The VR plugin now reads the headset's
  refresh rate (the 64-bit bridge passes it on) and sets SimRate to a whole multiple of it near
  144 Hz (72 -> 144, two steps every frame; 90 -> 180; 120 -> 120), from the next start, logged.
  `[VR] SimRateFromHeadset=0` leaves SimRate alone. Needs the updated nfsmw_vr_host.exe.
- **Mirrors on more cars:** the glass search groups only connected triangles in one plane, so a
  piece elsewhere in the same plane can't stretch the glass past a mirror's size. The Punto's and the
  GTO's interior mirrors are found now; the BMW M3's and Elise's right side mirrors match their left.
  The Eclipse's interior mirror is laid flat on its glass (its glass and housing back came out as one
  folded patch). The Supra's right side mirror shows the rear view (its glass is matched 2.5 cm wider,
  and the picture is spread over the glass's whole draw entry). Checked offline against every car.
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

- **New physics presets (by spinesplitter):** AWD, RWD MUSCLE, RWD TUNER, RWD SUPER and FWD
  replace the earlier six, and every drivable car (41) loads one of them. His values are in
  the current format: Grip and Grip balance in place of Front / Rear grip, no Slide oversteer, the
  15 rad/s spin limit.
- **Wheel buttons in the game's menus with `GameController=0`:** turning the game's own controller
  input off (so a wheel's pedals don't also move the camera) used to hide the wheel in the menus
  too. Now it only applies while driving; in the menus `MenuController` and `MenuAxes` decide.
- **"Reset page to mod defaults" gives the shipped settings:** the mod's built-in defaults
  differed from the shipped `nfsmw_gameplay.ini` in 21 settings (GameController and MenuController
  off, steering lock 900, engine heating 0.11, ABS off, ...), so a page reset changed them. They
  now match.
- **F4 menu: fewer, clearer settings.** Settings that only doubled another or that you set once
  are merged, removed or kept in the ini only:
  - Grip balance replaces Front grip and Rear grip (below).
  - Traction control is one row: Off / Game's / Game's + launch control (below).
  - Speed-sensitive steering is one slider, 0 = off (`[Controls] SpeedSensitivity`; an older
    ini's `SpeedSensitiveSteering=0` reads as 0).
  - Engine smoke in the back is automatic (below).
  - In the ini only: `[Physics] MaxSpin`, `[Engine] HeatUpMaxRPM` / `CoolDown` / `SmokeFrom`,
    `[Damage] MinImpact` / `SmokeFrom` / `SmokeTrail`.
- **Downforce per car** (`[Physics] Downforce`, per car and in presets, F4 > Physics): the game's
  own aerodynamic downforce, scaled. It grows with speed and is large (the Mustang's is 2.4 times
  its weight at 300 km/h), and since a tyre's grip limit is its load times its grip, it is most of
  the game's grip at speed. Below 100% a car gets lighter and looser the faster it goes; below 0
  it turns into lift. It goes through the game's own aero call, at the car's own aero point
  (AERO_CG), so suspension, grip and force feedback all follow. The log shows each car's own
  downforce at 300 km/h as a share of its weight.
- **Grip loss at speed is back for now** (`[Physics] SpeedGripLoss`, F4 > Physics > Grip loss
  at speed (old)), to compare with Downforce. F4 > Physics > Downforce at speed (`[Physics]
  UseAero`, on by default) picks which one applies, for every car. spinesplitter's presets hold
  both: his grip loss values and the Downforce giving the same grip at 250 km/h on a car with the
  Mustang's aero (AWD 0.45 / 53%, RWD MUSCLE 0.333 / 66%, FWD 0.285 / 70%, RWD TUNER 0.225 / 77%,
  RWD SUPER 0.125 / 87%).
- **Physics diagnostics:** `[Physics] LogPhysics=1` logs a line a second while driving (the game's
  downforce and ours, side grip factors, the tyre force ellipse), and the log notes a car whose
  suspension steps don't reach the mod (it would get none of the physics settings).
- **Wheel steering keeps one ratio in a slide:** speed-sensitive wheel steering widened the lock
  to the rear tyres' slip angle while sliding, so the same turn of the wheel steered the front
  wheels further mid-slide. The lock now follows the speed only.
- **ABS and TC icons on a retail install:** the retail game's `FRONTEND\FrontB.lzc` is compressed
  (JDLZ), and the icon pack was only made from an unpacked one ("FrontEndTextures.tpk not found",
  no icons). It is now unpacked with the game's own decompressor (LZDecompress). The Python tool
  that made the same pack is gone.
- **No more crash when the engine blows at the end of a race:** engine wear destroys the car when the
  engine blows (the drag race crash path, IDamageable::Destroy), and the game's handling of a
  destroyed racer reads the race's parameters without checking them; with none there (free roam,
  or a race just over) the game crashed (speed.exe+0x1FDEB5). The car is now destroyed only in a
  race that has them; otherwise the engine blows the game's own way, and full damage leaves the car
  totaled in free roam without destroying it.
- **A car left with the AI in free roam is handed back:** after beating a Blacklist rival the car
  stayed still with a revving engine until the next race (even in another car from the safehouse):
  the game had handed it to the AI at the race's end and never back. In free roam, a car the AI has
  had for 10 s, standing still while you hold the throttle for 2 s, gets the game's EBecomePlayerCar
  and its controls unblocked. Every block and unblock of your controls is now logged (`input:`).
- **HUD: the car icon's colours and place:** gray with no damage, the speedbreaker clock
  icon's own beige from 1% to 74%, and from 75% red, blinking by brightness in the same rhythm as the
  spike strip and engine warning icons (it pulsed by transparency before, and was beige from 0.5% in
  the arc's colour). The car icon is moved a little right, clear of the small arrow at the meter's
  end, and the other two icons with it, keeping their spacing (`[HUD] IconShift`, 2 HUD units).
- **Damage / heat meter fills to 100%:** at full damage or heat the bar fills the whole arc, up to the
  small arrow at its end (it stopped 5 degrees short), and the arrow keeps its own colour instead of
  glowing with the car icon.
- **No freeze when F4 settings are saved:** the save wrote each of ~300 settings with its
  own WritePrivateProfileString, each rewriting the whole file, on the game thread: ~475 ms every
  time. The values are collected on the game thread and written by a background thread, in order,
  logged as "settings written in the background".
- **No freeze while the F4 menu is open:** the FFB device names were looked up on the game
  thread every 3 s, each name a DirectInput enumeration of the force feedback devices holding the
  FFB mod's device lock: ~200 ms. They are fetched on a thread of their own when the menu opens and
  when the device setting changes; the menu shows the last names it has.
- **No freeze at F4 menu open (FFB device names):** listing the force feedback devices opens each one
  (~100-200 ms) and was done on the game's DirectInput object holding the FFB mod's device lock,
  which the game's own input reads also take, so the game still froze at every menu open even with
  the lookup on a background thread. The FFB mod now lists them on a DirectInput object of its own,
  without that lock, once per refresh (the menu asks for index 0 first); the names are answered from
  that list.
- **Front torque share and Drivetrain multiplication removed** (sliders and what they
  did): the game's own torque split and tyre force ellipse apply again. Old ini values are ignored.
- **Sim pedals:** the game clamps gas and brake to a deadzone at each end (0x8F876C, 5% in
  the exe, used only by those clamps), so a light brake did nothing and 95% was already full. With
  pedals bound it is set to 0 (any small runtime value accepted, logged): the whole travel counts,
  and the mod's own [Controls] PedalDeadzone is the only one.
- **Analogue handbrake:** bind Handbrake to an axis (pedal or lever) and its value goes to
  the game as it is; the game passes the handbrake action's value on to the car (FetchInput
  0x6A8BE5). A button or key still gives 0 / 1.
- **Grip balance** (`[Physics] GripBalance`, per car, F4 > Physics): grip moved between the
  axles, front tyres times 1 + the balance, rear times 1 - it; above 0 the rear lets go first
  (oversteer), below 0 the front (understeer). With Grip it replaces the earlier Front grip and Rear
  grip, which were three sliders for two things; `FrontGrip` / `RearGrip` in an older ini or
  preset are read as the same front and rear grip (Grip scaled, GripBalance set) and taken out when
  that section is saved.
- **ABS** (`[Physics] ABS=1`, on by default; F4 > Physics; `[Controls] ToggleAbs`): from each
  tyre's own wheel speed (UpdateLoaded keeps the radius +0xC4, angular speed +0xD0 and slip speed
  +0xE8, set at 0x69F209), a wheel turning more than 12% slower than the road under braking has its
  brake (+0xC8, after TuneWheelParams) eased off and reapplied as it rolls: full braking stops the car
  straight instead of spinning it. The handbrake is untouched.
- **Traction control with launch control** (`[Physics] TC`, off by default; F4 > Physics;
  `[Controls] ToggleTc`): the game's own traction control at 100%, plus the throttle this mod sends
  trimmed while the driven wheels (by the torque split) slip past 12%; from a standstill the limit is
  6%, so launches hook up without wheelspin. Both toggles are saved to the ini when pressed.
  F4 > Physics > Traction control sets it together with the game's own (`TractionControl`): Off,
  Game's, or Game's + launch control.
- **HUD: ABS and TC icons** (`[HUD] AbsIcon` / `TcIcon`): ABS directly above the engine
  warning icon, TC to its right, copies of the car icon in its object list (shown with it). Like a
  dash light: invisible while that aid is off, gray while on but idle, blinking in the HUD's beige
  while it works. Their textures (PERFORMANCE_BRAKES, MAIN_ICON_QUICK_RACE) only exist in the front
  end pack, which races unload, so `scripts\nfsmw_hud_icons.bin` (a two-texture pack built from
  FRONTEND\FrontB.lzc, laid out like HUDTEXDRAG.BIN) is
  loaded with every race HUD and unloaded with it.
- **Physics presets** (F4 > Physics > Presets): save the values in use as a preset
  (Preset 1, 2, ...), pick one, load it into the current car (turns on its own settings, so other
  cars keep theirs), overwrite or delete it (second press). Stored in `nfsmw_physics_presets.ini`
  next to the gameplay ini, one section per preset. "Rename this preset" takes a new name typed on
  the keyboard (Enter keeps it, Escape cancels; the mod's keyboard bindings, such as the seat keys,
  are muted while typing).
- **Per-car settings fixes:** ABS and TC are now kept per car with the sliders (a car
  section without them uses [Physics]'s), and their bindings save to the car's section while it has
  one instead of always to [Physics]. Turning "Settings for this car only" off now deletes the
  section through the background writer, after any pending save of it (before, a save still queued
  could write the section back), and switching cars waits for pending writes before reading.
- **F4 menu: one page per topic:** F4 (`[UI] Menu`) lists Controls (every binding and controller
  setting, gears and menu keys included), Force feedback, Shifter and clutch, Engine, Damage and
  tires, Physics, Traffic and HUD, VR (with Adjust dashboard and Triangle editor) and Menu and
  testing. Each setting is on one page (a short main menu and a Dev menu had shown 55 of them
  twice). A label too long for its row is drawn smaller instead of being cut off, and the help
  line under the list wraps over up to three lines.
- **Classic inputs:** `[Controls] ClassicInput=1` (F4 > Controls) gives
  steering, throttle and brake back to the game: nothing is injected, the steering type is the
  game's, and the game reads the controllers itself (set them up in its own options). What the mod
  held down is let go once. Shifting, the clutch and everything else stay.
- **HUD: ABS / TC icon position:** `[HUD] AidIconRight` / `AidIconUp` (HUD units, -1000 to
  +1000; sliders in F4 > Menu and testing) move both icons from their place, live.
- **Your settings are the defaults:** every Reset button now goes back to the values in use on
  2026-10-20/21 (the gameplay plugin's built-in values already matched; the FFB and VR settings the
  menu holds, and the VR plugin's built-in HUD crops and display opacity / glass / reflection, were
  changed to match). Per-car values (seat, dashboard size and position, wheel pivot) and the
  dashboard layout are not part of it.
- **ABS and TC are the same for every car:** they were kept per car while a car had its own
  physics settings, so switching them on in one car left them off in the others and looked like a
  lost save. They are now always `[Physics]`'s: never a car's own section or a preset (ABS / TC
  keys left there are ignored).
- **Settings saved in one write per file:** the background save set each of ~300 settings
  with its own WritePrivateProfileString, each rewriting the whole file: ~1.5 s, and quitting in that
  time lost the rest. Each file is now read once, every value set in memory, and written once to a
  temporary copy that replaces it: a few milliseconds, and a quit leaves the old file or the new one,
  never half of one. Comments, order and line endings are kept (checked against
  WritePrivateProfileString on a copy of a real ini: 490 values, all the same). If a file can't be
  replaced (locked), that file falls back to the old way, logged.
- **The pause menu's Performance Tuning can be switched off for your car:** its seven sliders
  (steering, handling, brakes, ride height, aero, nitrous, turbo) are read by the physics through
  PVehicle::GetTunings. With `[Physics] GameTuning=0` (F4 > Physics) that returns stock values (all
  0) for the player's car, whatever the menu holds; the default (1) is the game's behaviour. AI cars
  are unchanged.
- **Engine smoke for rear- and mid-engined cars:** the smoke comes out where the engine is, by
  the car: 3 m behind where the game puts it on the 911s, 2.5 m on the Carrera GT, Cayman S,
  Clio V6, Elise, Ford GT, Gallardo and Murcielago.
- **Keys:** F2 and F5 (Camera Toolkit free camera and pause), F9 (VR image dump), F10 (FFB restart)
  and F11 (VR shared-views debug flip) unbound in the installed inis; the packaged FFB ini no longer
  binds F10 (`KeyReset=0`; the F4 > Force feedback restart button remains). `[VR] SharedViewsOnce=1`
  (rear view and reflections drawn once for both eyes) written explicitly before F11 was unbound.
- **HUD: the ABS / TC icons need no extra step:** their texture pack
  (`scripts\nfsmw_hud_icons.bin`, the game's own art, so not in the zip) is made from the game's
  `FRONTEND\FrontB.lzc` on the first start; before, it took running a Python script.
- **ABS that works:** it judged a wheel's lock from its slip speed, which the game zeroes
  for a locked wheel, so it never saw one. It now compares the wheel's speed with the ground's under
  it, lets the brake off quickly past 15% lock until the wheel turns with the road again, then brakes
  again: the pulsing of a real ABS, the car still steers at full braking, and the ABS icon blinks.
- **Launch control lets the car go:** from a standstill traction control held the throttle
  almost shut at the first sign of wheel movement. It now allows the wheels 10% (at least 1 m/s) more
  than the road, the slip that pulls hardest, and never takes the throttle under 30% at a launch.
- **No perfect launch with the physics mod:** the game's race start boost (4.5 s for revs
  held right at GO) no longer fires; the start is down to the car, the throttle and TC.
- **HUD: the meter and icons go with the pause menu:** the damage / heat bar, its red heat
  fill and the car, spike strip, engine and ABS / TC icons stayed on screen in the pause menu. They
  are hidden while the game time stands still.
- **Cars keep their preset:** *Load into this car* gives the car the preset, and the car
  takes that preset's values again every time it's driven: changing a preset (Overwrite, or by hand
  in `nfsmw_physics_presets.ini`) changes every car that uses it. Renaming keeps the cars linked.
  *Settings for this car only* is gone; a car without a preset uses the shared values.
- **Physics: spin limit (fixes the pinball):** a car spinning fast (launched by props, landing while
  rotating) could get caught in a collision loop and be thrown across the map. Your car never spins
  faster than `[Physics] MaxSpin` (15 rad/s, in the ini), with the torque limited too.
- **Physics: new grip sliders,** all per car:
  - Offroad grip: grip on grass, dirt, mud, gravel and sand (95%).
  - Slide grip: a tyre that has broken loose slides on less grip than its peak.
  - Rain grip: 10% less grip in the rain.
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

- **One set of defaults:** the code, the shipped `nfsmw_ffb.ini` and the F4 menu's Reset values
  disagreed (damping 0.3 / 0.05 / 0.05, gain 1.0 / 0.85 / 0.85, ...). Now all three are: Gain 0.6,
  MaxForce 0.85, SlewRate 20, Aligning 1.0, Centering 0.2 (parked and slow only), Damping 0.15,
  Friction 0.05, ParkingFriction 0.2, SoftLock 1.0; effects: EngineRumble 0.05, Crashes 0.5,
  Scrapes 0.3, Breakables 0.4, Landing 0.6, CarHits 0.6, OffroadVibration 0.4, TyreScrub 0.15. An
  `nfsmw_ffb.ini` you already have keeps its values.
- **Effects that didn't come from the car are gone:** kerbs and bumps (`Kerbs`, fired on road
  seams), the road surface rumble (`SurfaceRumble`, which Off-road vibration covers), lighter
  steering with nitrous (`NosLighter`) and in wheelspin (`SpinLighter`, which lightened the
  steering even when only the rear wheels spun; the tyre force goes light in a slide by itself).
  Their keys left in `nfsmw_ffb.ini` are ignored.
- **F4 > Force feedback is shorter:** Maximum force, Smoothing and Force change limit are set in
  `nfsmw_ffb.ini` only (`MaxForce`, `SmoothingHz`, `SlewRate`); Strength stays.
- **Tyre scrub:** a fine vibration while any tyre slides past its grip limit, read from the
  game's own per-tyre traction, rising with how far past it is and with speed (`TyreScrub`, 0.2;
  F4 > Force feedback).
- **The steering weight comes from the car:** the FFB mod built the car's sideways
  direction assuming the game's physics world is Z-up, like its renderer. It is Y-up (and
  left-handed: `bConvertFromBond` maps render = (z, -x, y)), so "sideways" was vertical: the front
  axle's sideways speed read about 0 even with the rear sliding at 6 degrees, and every collision
  came from "side 0.00". The self-aligning torque only ever saw where the wheel was turned, a
  centring spring that got stiffer with speed. The car's frame now comes from the wheels
  themselves (front left to front right), and the front tyres' slip angle is the game's own (its
  tyre model's, yaw and slide included). The wheel now goes heavy as the front bites, light as it
  washes out, and pulls into the countersteer while the car is still rotating. Crash, scrape,
  vehicle hit, breakable and landing jolts now push the wheel the way the hit came from (they were
  sideways-less; a nose-to-tail vehicle hit, which has no clear side and always kicked right, now
  only shakes).
- **The steering lock is the game's own:** the lock the FFB uses for where the front wheels
  point comes from the game's front wheel angle over the steering input of that physics step. The
  game's reported lock (`GetMaxSteering`) is only written on the gamepad and AI paths, so with a
  wheel it stayed at 45 degrees whatever the speed-sensitive steering did.
- **Tuning that stood in for the missing tyre force is gone:** the speed boost on the aligning
  torque (stiffer with speed), the speed-sensitive centring spring (`CenteringParked`,
  `CenteringSpeed`) and the centre weight (`CentreWeight`, also in F4). `Centering` is now a spring
  toward straight ahead while parked or slow that fades out by 20 km/h, as the tyres take over
  (F4: "Slow-speed centring"). Old keys left in `nfsmw_ffb.ini` are ignored.
- **F4: three FFB rows that had no setting behind them are gone:** "Front grip loss", "Rear grip
  loss" and "Tyre scrub" pointed at FFB settings that did not exist (the gameplay mod did not
  build); the real aligning torque gives that feedback now.
- **`LogTelemetry=2` adds a steering trace:** every physics step, the steering input, front
  wheel angle, where the front axle travels, the lock, speed, yaw, sideways speeds, each tyre's
  slip angle and the game's drift state, in `nfsmw_ffb.log`.
- **Harder jolts when an impact damages the car:** every impact the gameplay mod's car damage
  counts (its speed change over `[Damage] MinImpact`) is passed to the FFB mod
  (`NfsmwFfb_Setting("DamageHits")`). The crash or vehicle-hit jolt of that impact gets harder for
  a harder hit (one jolt, not a second one on top), or a crash jolt starts away from the side of the
  latest contact; the other short effects step back while it plays (`Crashes`).
- **Stationary friction** (`[FFB] ParkingFriction`, now 30% by default, F4 > FFB > Stationary
  friction): the tyre scrub that makes the wheel heavy to turn at a standstill, gone by 18 km/h.
  It existed at 15% with no slider.
- **Stationary friction no longer wobbles:** the friction was a velocity sign (tanh of the
  smoothed wheel speed), which kept pushing after the wheel reversed and set up a wobble that
  damping couldn't stop. It is now a stiction spring to an anchor the wheel drags along once
  turned past ~0.6% of its rotation (about 2.7 degrees on a 900 degree wheel), with a little damping
  inside: position based, so it holds the wheel without lag or flipping.
- **New jolts:** breakable props (benches, sign posts, fences, bins: the game's own breakable
  objects, `Breakables`), landing after a jump (`Landing`), and hitting another vehicle (traffic,
  cops, racers, told apart by the game's object type, `CarHits`), which ducks the other short
  effects while it plays. Vehicle hits and breakables are at full strength from 150 km/h.
- **Off-road vibration** on grass, dirt, mud, gravel and sand, by the surface's own name under each
  front tyre (`OffroadVibration`).

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
