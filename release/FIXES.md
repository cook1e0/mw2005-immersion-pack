### Unreleased

- **Challenge Series: the car is handed back at the start:** time trials (toll booths) give the
  player's car to the AI at the start (`Game.ForceAIControl`) in the same frame as the race timers
  start, and nothing ever hands it back, so the AI drove the whole race. Rolling starts with a
  countdown are meant to hand it back at GO (`EBecomePlayerCar`). The gameplay mod now runs the
  game's own `EBecomePlayerCar` when the AI took the car within 2 s of the start: as soon as you
  press a pedal or steer, and 3 s after the start at the latest. Player input left blocked is
  unblocked with it. A car the AI takes over later (a finished race) is left alone. Every step is
  logged as `race start:` lines in `nfsmw_gameplay.log`.

- **VR: no more z-fighting far away with a low near plane:** the cockpit needs a near plane of
  ~0.05-0.1 m (`NearZ`), and with one that low a 24-bit depth buffer runs out of precision a few
  hundred metres out, so distant surfaces flickered through each other. In the driver's seat the
  depth buffer is now split: the player view's own matrices get `[VR] WorldNearZ` (0.3 m) and the
  far 98% of the depth range, which every game drawing path reads; your own car's objects (body,
  interior, driver, steering wheel, recognised by the matrices they are queued with) get a
  WorldViewProj of their own with `NearZ` and a 20 m far plane in the near 2% (hook on the
  per-object WorldViewProj multiply at 0x6C81CA). The mod's dashboard, wheel and mirror drawing use
  the car's projection. Your car is always drawn in front of the world, which only shows when
  something gets inside it. If the car's draws are not recognised the split turns itself off and
  says so in the log. `[VR] DepthSplit=0` restores one near plane.

- **VR: fewer draws per frame from the reflections' environment map:** the last session's log was
  CPU-bound on draw calls - up to about 6,000 for the left eye and 4,000 for the right in traffic and
  pursuits, 12-17 ms of eye rendering against a 13.9 ms frame, so the headset fell to 36 fps. The
  environment map's six cube faces (scene renders around the car, the same for both eyes) were all
  redrawn every frame; now 2 a frame are, in turn, so each face refreshes about 24 times a second
  (`[VR] EnvMapFacesPerFrame`, 6 = as before). Both eyes always sample the same map, so stereo is
  unaffected. `[VR] RearViewEvery=2` optionally renders the mirrors' rear view every other frame (off
  by default). With `LogPasses=1` the `passes per frame` line now gives each pass's draws.

- **VR: reflections sit still when you move your head, again:** a debug key added with the eye-rule
  checks (`KeyToggleRefl`, F12 in the installed ini - also Steam's screenshot key) switched the
  seat-view reflections (`[VR] StableReflections`) off for the rest of the session; the log showed
  `seat-view reflections off` two seconds after start, so car paint and glass reflections turned
  with the head. The key is gone: StableReflections is only set in the ini, on by default.

- **VR: no more stutter when the HUD, the in-car dashboard or the mirrors first appear:** the mirror
  shaders (which loaded d3dcompiler_47 and compiled three pixel shaders on first use), the dashboard's
  textures and the reused state block were all built inside a race frame, at the exact moment the mirrors
  or the dashboard showed up - the frame they landed in took over a second (a `game` spike with nothing
  in the measured phases to explain it). They are built when the device is created (and again after a
  reset), while the game is still loading; the log's `warm-up` line says what that cost.
- **VR: the dashboard's HUD probe no longer waits for the GPU:** finding which dashboard elements are
  showing means copying the HUD into a system-memory surface and reading it back, and both the copy and
  the lock waited for the frame's eye renders to finish - again at the moment the HUD pops up. The copy
  is issued a few times a second and locked with D3DLOCK_DONOTWAIT, retried on the next frame when it
  isn't ready yet (so an element is still picked up a frame or two after it appears); if a driver never
  reports it ready the probe falls back to waiting, once, with a line in the log.
- **VR: less work per frame:** the rear view is copied and graded once a frame instead of once per eye
  (it is the same picture for both), one D3D state block is captured and applied instead of a new one
  created per draw (twice an eye with the dashboard, plus once per coloured-triangle batch), the
  player-car anchor is checked once per car render and reused by its draw entries, and the glass
  vertex shader dump (a shader disassembly and twenty log lines at the first mirror) is off unless
  `[VR] LogGlassShader=1`.
- **VR: hitches are named in the log:** the `perf:` lines now give the frame's time outside the measured
  phases as `game avg/max ms` (it was an average only, which is where a spike without a matching phase
  hid), a `perf worst frame` line breaks the window's longest frame into its phases, and a `hitch:` line
  is written - at most once a second - for a frame more than half again as long as the average.
- **VR: each eye now collects its own light flares (fixes lights that differed between the eyes):**
  the world's light flares - street lamps, warning lights, the sun - are collected while each view's
  scenery is culled and drawn from that pool per view, but the pool was emptied once a frame, in the
  game's post-display step after both eyes, so the second eye found the first eye's flares still in
  it and drew them again: doubled where the pool had room and dropped where it ran into its fifty.
  Each eye now collects its own; `flares: pool N entries after eye 1, M after eye 2` in
  nfsmw_vr.log shows the two counts.
- **VR: only known eye-independent views are reused between the eyes:** the rear view and the six
  environment-map faces are rendered for the first eye only. Other offscreen views, including
  shadow maps, remain active for both eyes; using a camera other than the player's does not by itself
  prove that a view's output can be shared. Views using the player's camera (such as bloom and
  specular) are rebuilt for each eye. The PC build's view table is written to the log once per
  session (`view NN: id ... active ... playerCam ... rt WxH`) to check the view setup.
- **VR: per-car dashboard/HUD defaults:** each car's HUD size, distance, height and side are kept
  per car model; a car whose line is missing or unreadable falls back to the mod's own defaults
  instead of keeping the previous car's values.
- **VR: one dashboard layout for every car:** Adjust dashboard element positions and sizes are
  shared in `[DashLayout] Default`; an existing `[DashLayout] SUPRA` entry is used as the initial
  shared layout. Panel placement, depth and size remain independently saved per car in `[Dash]`.
- **VR: less work per draw entry:** the check that tells a streamed-in model from the one last seen
  at the same address hashed the model's whole index buffer for every draw entry of every car; it
  is taken once a frame per model now (the pointers are still compared every time). The D3D9 hooks
  also look up mirror-glass and dashboard overlay jobs by index buffer instead of scanning every
  registered job on every draw.
- **VR: no repeated steering-wheel discovery on streamed car models:** once an interior model's
  wheel has been found, its index ordering and wheel split are reused when that model is streamed
  back in. Triangle-editor changes to wheel membership explicitly rebuild the cache. The eye draw
  hook also takes a direct pass-through when there are no per-eye mirror, wheel or dashboard jobs;
  active wheel stencil draws are looked up by index buffer and start index.
- **VR: faster first-time steering-wheel discovery:** the wheel search scans a coarse set of wheel
  heights, then refines around the best candidate instead of testing every height at full precision.
  This reduces the CPU hitch when a race first streams multiple car interiors without skipping
  either eye or any shadow views.
- **Gameplay: HUD text diagnostics log once per object/label:** changing timer and score text no
  longer generates a new synchronous log entry for each value while a HUD object is rendered.
- **Force feedback: car hits come through clearly:** while the kick and shake of hitting another
  vehicle plays, the other short effects (engine, kerbs, surface, crashes, scrapes, breakables,
  landings) are ducked in proportion to the hit's strength, so the impact isn't buried under them.
  The telemetry log line (`[FFB] LogTelemetry`) shows the hit (`car=`) and how much it ducks the rest
  (`priority=`).
- **Gameplay: steering never reads past full lock:** a wheel axis reporting values outside its range
  is clamped, so the wheel angle handed to the game and the VR steering wheel stop at half the
  wheel's rotation instead of turning further. Covered by a new regression test.
- **Gameplay: free roam totaled message shortened:** a car totaled in free roam now shows "TOTALED"
  (it said "TOTALED - JUMP TO SAFE HOUSE").
- **VR: debug key for the shared views:** `[VR] KeyToggleShared` (e.g. 122 = F11, off by default)
  flips live which views the second eye reuses instead of rendering, to tell a view that differs
  between the eyes from one that doesn't.

### v0.20.0: car mirrors in VR, grip sliders, spin limit, new force feedback jolts

- **VR: the car's own mirrors show what's behind you:** the game's rear view is drawn on the
  interior rear view mirror and on both side mirrors' glass in the player car (static slices of the
  rear view, `[VR] SideMirrorWidth`), with a dark rim around the side mirror glass. The game's
  MIRROR material no longer tints the picture; F4 > VR > Mirror brightness / Mirror contrast.
  Works on cars whose mirror glass is part of the body (BMW M3 GTR). `[VR] GameMirrorsSkip` lists
  cars to leave with the game's own glass. The HUD's rear view mirror is hidden in VR.
- **VR: Fiat Punto rear view mirror:** its own glass was drawn after the rear view picture and
  covered it with a reflective surface; the picture is now drawn after every draw of the model.
- **VR: faster frames:** the rear view and the environment map are rendered once a frame instead
  of once per eye, and the game's pre-display pass (lights, flares, fades) runs once, so both eyes
  also see the same lights. Frame timing is in nfsmw_vr.log ("perf:" lines).
- **VR: no cutaways:** no camera flip when the car rolls, no pursuit breaker cutscene, cutaway or
  cop camera, no busted viewfinder frame while the headset runs.
- **VR: HUD:** F4 > VR > HUD left / right; "Each car remembers HUD positioning" keeps size,
  distance, height and side per car; "Steering wheel rendered in front of HUD" draws the car's
  wheel over the HUD panel while driving (not in menus).
- **VR: driver's hand leftovers hidden:** a hand split into its own draw entry was left floating on
  some cars; it is hidden with the arms.
- **Physics: spin limit (fixes the pinball):** a car spinning fast (launched by props, landing while
  rotating) could get caught in a collision loop and be thrown across the map. The player's car
  never spins faster than F4 > Physics > Spin limit (`[Physics] MaxSpin`, 8 rad/s), checked before
  and after each physics step, with the torque limited too.
- **Physics: new grip sliders,** all per car with "Settings for this car only":
  - Rear grip (oversteer): the rear tyres' grip relative to the front.
  - Offroad grip: grip on grass, dirt, mud, gravel and sand.
  - Slide grip: a tyre that has broken loose slides on less grip than its peak.
  - Slide oversteer: the rear tyres' side grip under throttle, as on grass (front drive cars:
    lifting off), fading as the slide passes 20-45 degrees, so it stays catchable.
  - Rain grip: grip in the rain (90% = 10% less).
  - Grip loss at speed: cornering grip falls with the square of the speed; launching and braking
    keep their grip (`[Physics] SpeedGripLoss`, the share gone at 300 km/h).
- **Per-car physics covers every physics slider.**
- **Testing: Drop from air** (F4 > Menu and testing): lifts your car and lets it fall.
- **Sequential shifting has neutral and reverse:** shift down from 1st to N, then R; the
  automatic reverse no longer kicks in with a manual gearbox.
- **Engine heat:** nitrous on the rev limiter heats the engine twice as fast (a third faster on a
  car over 75% damage); no heat for 8 s after a race start, restart or respawn reset, or in a
  cutscene.
- **Smoke:** engine smoke no longer hangs in the air where it stopped; a missed shift no longer
  leaves a smoke trail.
- **Force feedback:** off-road vibration (replaces the grass pull), the wheel goes lighter with
  nitrous (F4 > Force feedback > NOS), a hard jolt on landing, and the same jolt when you hit
  another vehicle (traffic, cops, racers, told apart by the game's own object type). Vehicle hits
  and breakable objects are at full strength from 150 km/h.

Known issue: in a new career, entering the safehouse after buying the first car can skip the
first-visit scene with Mia and go to the safehouse menu. Being investigated.

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
- **Sequential shifting has neutral and reverse:** with manual shifting (sequential mode or the
  game's manual gearbox), shifting down from first goes to neutral, then reverse, and up again the
  same way; in reverse the gas pedal drives backwards. Holding the brake no longer reverses.
- **VR: side mirrors look like mirrors:** the glass is drawn with a plain shader showing the rear
  view as it is, without the car paint's gloss and reflections that washed it out (the brightness
  and contrast sliders may want lowering now).
- **Physics: no more pinballing around the map:** the game lets a car spin at up to 30 rad/s; once
  the player's car spins that fast (props launching it, landing while rotating, with low aids),
  every step turns it deep into walls and the ground and the push-out adds speed, so it tumbles
  around the map gaining speed with nobody driving (logged at 15-23 rad/s, hits up to 76 m/s). The
  player's spin is now limited to 8 rad/s (normal driving stays under 5). F4 > Physics > Spin
  limit, `[Physics] MaxSpin` (0 = the game's).
- **VR: pursuit breakers no longer freeze the game and respawn the car:** besides the camera, a
  pursuit breaker starts an NIS (ETriggerMomentNIS), which paused the world for ~10 s and then
  placed the car again; in VR that trigger is now dropped.
- **Physics: rear grip (oversteer) and off-road grip:** F4 > Physics > Rear grip scales the rear
  tyres' grip against the front (below 100% the rear steps out, front drive too, `[Physics] RearGrip`);
  Offroad grip scales grip on grass, dirt, mud, gravel and sand by each tyre's surface
  (`[Physics] OffroadGrip`, 75%).
- **Physics: slide grip:** a tyre that breaks loose now slides with less grip than its peak
  (F4 > Physics > Slide grip, `[Physics] SlideGrip`, 85%), so with a lower Rear grip the
  car goes into a slide you can catch instead of snapping into the turn.
- **VR: vanilla side mirrors:** the side mirrors are the game's own again (`[VR] GameMirrors=0`); the
  interior rear view mirror stays.
- **Force feedback: landing:** a hard kick and a short shake in the wheel when the car lands after a
  jump, stronger the longer it flew (F4 > Force feedback > Landing, `[FFB] Landing`).
- **Force feedback: vehicle hits:** the landing's kick and shake when you hit another vehicle
  (traffic, cops, racers), toward the side you were pushed; recognised by the other object's own
  type (SIMABLE_VEHICLE), and no longer counted as a generic crash (F4 > Force feedback > Vehicle
  hits, `[FFB] CarHits`).
- **Physics: "Settings for this car" covers every physics slider:** Rear grip, Offroad grip, Slide
  grip, Slide oversteer, Rain grip and Spin limit are kept per car too (a car's section without them
  uses the [Physics] values).
- **VR: side mirrors show a wider, fixed view:** each shows `[VR] SideMirrorWidth` (60%) of the rear
  view from its side, full height.
- **VR: side mirrors per car:** with `[VR] GameMirrors=1`, cars listed in `GameMirrorsSkip` (e.g.
  `BMWM3GTRE46,SLR`) keep the game's own side mirrors.
- **VR: no stray hand pieces with Hide arms on:** some drivers have their hands in a second draw entry
  (a second DRIVER_PLAYER material); an entry that is mostly arms is now hidden whole, and hands
  low on the wheel in low cars (Gallardo) count as arms.
- **Vehicle hit and breakable jolts scale with speed:** full slider strength from 150 km/h, less
  below it (a tenth at least).
- **Challenge races no longer end with a blown engine at the start:** a rolling start put the car at
  speed in the shifter's gear (2nd at 170 km/h blew the engine in 4 s and knocked the car out of the
  race). Engine heat now waits 8 s after the game resets the car (race start, restart, respawn) and
  doesn't rise during cutscenes.
- **No smoke left behind by a missed shift:** the drag race "missed shift" event isn't sent any more
  (its exhaust smoke stayed where the shift was missed); the grind sound stays.
- **Physics: slide oversteer you can catch:** the rear's lost side grip now follows the throttle
  (lifting off in front drive cars) and comes back as the slide passes 20-45 degrees, so lifting
  or countersteering recovers it instead of the car spinning.
- **Engine heat: nitrous on the limiter:** with engine wear on, the nitrous at maximum revs heats the
  engine twice as fast; on a car over 75% damaged (whose engine already heats a third faster) it
  adds only a third more, so the two don't pile up.
- **Physics: slide oversteer:** the rear tyres lose side grip the way they do on grass while keeping
  their drive grip, so the rear slides out under power on the road too (F4 > Physics > Slide
  oversteer, `[Physics] SlideOversteer`, 80%; 100% = off).
- **Physics: rain grip:** tyres have 10% less grip in the rain (`[Physics] RainGrip`, 90%).
- **Engine smoke no longer hangs in the air:** when the smoke stopped (repair, respawn, engine cooled)
  the game switched the effect off without updating it again, freezing its last puffs in place for
  good; they now drift and fade out.
- **VR: frame timing in the log:** every 5 s `nfsmw_vr.log` gets a "perf:" line: fps, and the
  average and worst milliseconds spent waiting for the headset, rendering the eyes, waiting for the
  GPU, submitting, drawing the spectator window, presenting, and in the game itself.
- **Testing: Drop from air:** F4 > Menu and testing > "Drop from air" lifts the car straight up by
  "Drop height" (`[Tires] DropHeight`, 5 m) and lets it fall, keeping its forward speed, to test
  landings anywhere.
- **Force feedback: NOS FFB:** the steering goes lighter while the nitrous is on (the game's own
  IsNOSEngaged), eased in and out. F4 > Force feedback > NOS FFB, `[FFB] NosLighter` (0..0.9).
- **VR: HUD left / right:** F4 > VR > HUD left / right (`[VR] HudRight`), also kept per car.
- **Force feedback: off-road vibration:** the game's own surface name under each front tyre tells
  grass, dirt, mud, gravel and sand apart from the road: the wheel rumbles unevenly (two rough
  tones and random bumps), half with one wheel off, growing with speed. F4 > Force feedback >
  Offroad vibration, `[FFB] OffroadVibration`.
- **VR: each car remembers its HUD:** F4 > VR > "Each car remembers HUD positioning"
  (`[VR] HudPerCar`): like the seat, the HUD size, distance and height are saved per car
  (`[Hud]`); a car without its own yet keeps the last HUD you used.
- **VR: steering wheel in front of the HUD:** F4 > VR > "Steering wheel rendered in front of HUD"
  (`[VR] WheelOverHud`). While driving, the HUD panel is drawn into the picture behind the car's
  steering wheel; in the pause menu, other menus and while the F4 menu is open it is the usual
  panel on top again.
- **VR: the view no longer turns over with the car:** with the horizon levelled (`LevelRoll`), a car
  on its roof or barrel-rolling kept the view upside down; now "down" always points at the ground.
- **VR: pursuit breakers no longer cut away:** the slow-motion moment camera (CDActionTrackCar) and
  any other camera set directly, not only through the camera director's selection, is refused in
  VR (the game's own NIS cutscenes still play).
- **VR: busted cutscene without the viewfinder frame:** the BUSTED_OVERLAY square over the scene is
  not shown in VR.
- **VR: faster frames:** the rear view mirror and the six faces of the reflections' environment
  map were rendered again for the second eye (seven extra scene renders a frame, none of which
  depend on the eye). They are rendered once a frame now and both eyes use them.
  `[VR] SharedViewsOnce`.
- **VR: both eyes see the same lights:** each eye runs the game's frame drawing, which starts by
  stepping per-frame state (animated world models, the sun flare, light flare parameters, tunnel
  bloom and tinting fades, sound). Done twice, the second eye saw a later step than the first, so
  blinking lights, flares and fades differed between the eyes. It runs once per frame now.
- **VR: reflections sit still when you move your head:** the car paint and window shaders turn
  their reflections with the view's rotation (WorldView), which in the headset is your head's. While
  the eyes render, that one matrix now comes from the seat (the game camera without your head),
  so reflections stay put as you look around; positions are untouched. `[VR] StableReflections`.
- **VR: rear view mirror inside the car:** the interior's rear view mirror is found in front of the
  driver's head on the centre line (about 0.2 x 0.06 m, facing back; sun visors and roof strips
  around it are passed over) and shows the whole rear view. Found offline in every car's interior
  except the GTO's.
- **The game's own side mirrors show the rear view (VR):** the side mirror glass is its own draw
  entry in each car's model, with the "MIRROR" material (in the game: black, a white glow and no
  reflection). The game now draws that glass itself with the rear view as its texture: its
  vertices get texture coordinates across the glass (the left mirror the left half of the rear
  view, the right the right half) and the material is plain white while it does, so it is the
  game's own glass, shaded like the rest of the car, not a picture laid over it. `[VR] GameMirrors`.
- **VR: working side mirrors:** in the headset the game's rear view mirror image is shown on the
  player car's side mirrors instead of at the top of the HUD (hidden in VR whatever the game's
  option). The mirror glass is found in each car's model (checked offline on every car, including
  the few that have their mirrors in the body); the left mirror shows the left half of the rear
  view, the right one the right half. `[VR] Mirrors`, `MirrorSplit`, `MirrorFlip` (if the image
  is the wrong way round), `MirrorBrightness`. The image is taken from the rear view's own render
  target. The game only renders the rear view outside the in-car cameras and only while the race
  HUD allows it, so in the headset it never rendered and the mirrors showed the game's own glass;
  in VR it now always renders, and the HUD's mirror is left out by the d3d9 wrapper instead
  (also the copy the HUD drew from the rear view's own texture, under the cop radar). The image is
  drawn on the glass's own triangles right after the game draws that mirror's model, with the
  scene's depth, so it has the glass's outline and the door, pillars and housing cover it as they
  should (the first version laid a rectangle over the finished picture).
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
