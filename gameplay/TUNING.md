# Tuning guide

How the gameplay mod makes each car drive the way it does, and what to change when one doesn't feel
right. Everything is in the F4 menu while driving: **Physics** for how the car drives, **Force
feedback** for how the wheel feels. Change one thing at a time and drive the same corners again.

## In short

1. **Leave the driving aids off** (Stability 0%, Drift assist 0%, traction control off, ABS on).
   The game's own aids add and take away grip behind your back; with them on, nothing below
   behaves as described.
2. **Set Grip** in slow and medium corners, until the cars' cornering speed there feels right.
   It's one value for every car.
3. **Set Car character** to how different you want the cars to feel from each other (50% to start).
4. **Leave Real weight distribution on.**
5. **Set Downforce** last, in fast corners only, for how planted the cars feel at speed (70% to
   start). Lower is looser at speed; real road cars have almost none. Do it after Grip: the grip
   downforce gives is its extra load times Grip, so changing Grip later changes fast corners again,
   while downforce hardly touches the slow ones.

That's it for most people. Every car then gets its own feel from the game's data for it; there's
nothing to set up per car. The rest of this guide is for when a car still feels wrong.

## How a car gets its feel

Four things, in this order:

**1. The game's own data for the car.** Every car has its own weight, springs, anti-roll bars,
dampers, tyre grip, downforce and engine. The game keeps most of these close together, so out of
the box a Camaro and a Carrera GT feel more alike than they should.

**2. Car character** pushes them apart. Each car is compared with the middle of the 38 player
cars, and whatever is different about it gets more different:

| The car has... | With more character it gets... | For example |
|---|---|---|
| soft springs and anti-roll bars for its weight | softer: more body roll, more dive under braking | GTO, Monaro, Camaro |
| stiff springs and bars | stiffer: flatter, sharper | Carrera GT, Corvette C6.R, Elise |
| less grip than most | a bit less grip | Camaro, Punto |
| a lot of torque for its weight and grip | more step-out under power | C6.R, Camaro, SL65, Viper |
| little torque for its weight and grip | less step-out under power | RX-8, A4, GTI |
| anti-roll bars stiffer at one end | more of that: more push, or a looser rear | Ford GT pushes; RX-7, Viper rotate |

Character never gives a car more grip than the game does: cars only get harder to
drive, never easier.

- **0%**: the game's own values.
- **50%**: the default; clearly different cars.
- **100%**: as far apart as it goes (some race cars hit the limits).

**3. Real weight distribution** puts each car's weight where the real car has it. The game puts
51–55% on the front wheels of every car, even the 911. With this on:

| Car | Front weight | What you notice |
|---|---|---|
| 911 GT2, 911 Turbo, 997 S, Elise, Clio V6 | 38–39% | light nose, tail swings out if you lift mid-corner, great traction out of corners |
| Carrera GT, Murciélago, Gallardo, Ford GT, Cayman | 42–45% | the same, a little milder |
| Viper, RX-7, RX-8, DB9, M3 GTR, C6.R | about 50% | balanced |
| Mustang, GTO, Camaro, SL65 | 53–56% | front-heavy: pushes on corner entry |
| Golf GTI, Punto, Cobalt, Eclipse, Evo, Impreza, A3, A4 | 59–64% | nose-heavy: pushes wide, the tail follows |

The rear-heavy cars also get a little more rear grip, as their wider real rear tyres would give.

The Physics page shows it too: the **This car** row at the top gives the car's drive, weight,
front weight and power, and a slider that Character or real weight changes for this car shows
what the car actually gets after its value (Grip `0.750x   this car 0.720x`). The log has the
same, the first time you drive a car:

```
physics: character 50% for camaro: springs 0.90, dampers 0.95, anti-roll bars 0.94, roll balance -0.01,
grip 0.96, throttle takes side grip 1.25; weight 56% front, grip balance +0.000
```

Below 1 is softer, less grip or less step-out than the game's; above 1 is more.

**4. Downforce** is set by the **Downforce** slider alone, not by Character: one share of each
car's own aero for every car, so the race cars keep the most. The game gives every car far more
than a real one (a stock Mustang about 40% of its weight at 150 km/h at 70%, where the real car
has slight lift), so this is the slider for how glued-down the cars feel at speed. In town there's
almost none either way.

## When a car feels wrong

Most problems are one of these. Start with the left column.

| The car... | Try first | Then |
|---|---|---|
| feels too much like the others | Car character higher | |
| is too wild to enjoy | Car character lower | Grip when sliding higher |
| feels on rails, never slides | Grip lower | Grip when sliding lower |
| pushes wide in slow corners | Roll balance below 0 | Throttle takes side grip lower (front drive) |
| pushes wide in fast corners | Roll balance below 0 | Downforce higher |
| snaps sideways when it lets go | Grip when sliding higher | Tyre grip peaks at higher |
| slides too far, hard to catch | Grip when sliding higher | Roll balance above 0 |
| spins under power out of slow corners | Wheelspin in slides lower | Throttle takes side grip lower |
| won't rotate with the throttle | Wheelspin in slides higher | Roll balance below 0 |
| revs stay flat in a drift | Wheelspin in slides higher | |
| spins under hard braking | Grip balance back to 0 | Roll balance above 0 |
| glued down at high speed | Downforce lower | |
| nervous on fast sweepers | Downforce higher | Suspension firmness lower |
| feels lazy, rolls a lot | Suspension firmness higher | Car character lower |
| a 911 or mid-engined car is undriveable | Real weight distribution off, to compare | Car character lower |

**Every car, a group of cars, or one car?** The sliders marked "per car" can come from three
places. **Settings from**, near the top of the Physics page, says which one the car you're driving
uses; pick another from its drop-down (or step through them with left / right):

- **Shared**: the same values as every other car on Shared. Where every car starts.
- **A group**: a named set of values shared by the cars in it, for example all the muscle cars.
  Changing a slider in any car of the group changes it for all of them.
- **This car's own**: values for this car alone.

Car character and Real weight distribution are per-profile values, and act on each car's own data,
so two cars in the same group still drive differently.
Real weight distribution is saved as `RealWeight=0` or `1` in `[Group.<name>]` or `[Car.<name>]`.
Older setups without that key inherit the Shared `[Physics] RealWeight` value.

Six groups come premade, with every player car in one, by class and drive:

| Group | Cars |
|---|---|
| Muscle | Camaro SS, Corvette C6, Cadillac CTS, GTO, Monaro VXR, Mustang GT, Viper SRT-10 |
| Tuner FWD | Cobalt SS, Eclipse GT, Golf GTI, Punto |
| Tuner AWD | A3, A4, Impreza WRX STi, Lancer Evolution VIII, TT |
| Tuner RWD | Clio V6, IS 300, RX-7, RX-8, Supra |
| Exotic RWD | the 911 GT2 and Carrera S, both M3 GTRs, Carrera GT, Cayman S, CLK 500, Corvette C6.R, DB9, Elise, Ford GT, SL 500, SL 65, SLR |
| Exotic AWD | 911 Turbo S, Gallardo, Murcielago |
| FWD, RWD, AWD | empty: for tuning by drive alone, tick cars into them (each leaves its class group) |

They start without values of their own, so they drive exactly as Shared until you change a value
in one: then it applies to that group's cars only. Move cars between them, rename or delete them
as you like.

Groups are made and edited on the **Groups** page (Physics > Groups):

- **On the left, the groups.** Pick one to see it. **New group** starts one from the values of the
  car you're driving, with no cars in it, and lets you type its name (Enter keeps it; Escape keeps
  "Group 1"). **Rename** and **Delete** (press twice; its cars go back to Shared) act on the
  picked group.
- **On the right, its cars**: every player car with a checkbox. Tick one to add it, untick to take
  it out. A car is in one group at most: a car marked with another group's name moves when you
  tick it. A car marked **own** has its own settings, which go before any group.
- **Below them, its values**: the group's per-car sliders. You can tune a group from any car,
  without driving one of its cars; a car in it gets the change straight away.

With the mouse, click the controls. With the menu keys or a wheel's d-pad (in VR): Up/Down moves
between them, Right ticks, presses or raises, Left lowers or goes back.

**Sharing a group or a car's setup.** Under **Share** on the Groups page:

- **Export group** saves the picked group (all its values and its cars) as
  `scripts\profiles\Group - <name>.ini`; **Export this car** saves the car you're driving's own
  settings as `scripts\profiles\Car - <car>.ini`. Send the file as it is.
- Whoever gets it puts it in their own `scripts\profiles` folder and picks **Import...**: the list
  shows every profile there and what importing it does. A group becomes a new group, or replaces
  theirs of the same name (press twice); its cars move into it. A car's file becomes that car's own
  settings.
- A profile includes Real weight distribution as well as the other per-car values. ABS, wall bounce
  and the steering feel are each player's own (they're for every car).

In the ini a group is a `[Group.<name>]` section with the values and `Cars=` its cars
(`Cars=camaro,gto,mustanggt`), and a car's own values are a `[Car.<name>]` section.

## What each setting does

### Grip and balance

- **Grip**: how hard every car can corner. It doesn't change which end lets go first.
- **Roll balance**: which end lets go first in a corner, by moving anti-roll bar stiffness between
  the axles. Below 0 the front grips better (less pushing wide), above 0 the rear does (less
  oversteer). Steps of 10%. It only acts while cornering, so braking and straights stay stable.
  This is the setting to balance a car with.
- **Grip balance**: leave at 0. It moves grip between the axles all the time, under braking too:
  above 0 the car spins when you brake hard, below 0 the steering goes dead. Use Roll balance.
- **Suspension firmness**: softer rolls more, dives under braking and squats under power, a lazier
  car with more warning; firmer is flatter and sharper, a little less grip at the limit.

### Tyres

- **Grip when sliding**: how much grip a tyre keeps once it's well past its limit.
  - 75–80%: a slide grows unless you catch it.
  - 85% (default): a real road tyre.
  - 90–95%: forgiving; slides stay small.
- **Tyre grip peaks at**: how much warning the tyres give.
  - 5–6°: sharp, but they let go suddenly.
  - 8° (default): road tyres.
  - 10–12°: softer, more warning.

### Throttle

- **Throttle takes side grip** (100%): a tyre's grip is shared between driving and cornering, so
  flooring it in a low gear makes a rear-drive car step out and a front-drive car push wide;
  easing off tightens the line. Car character makes high-torque cars do this more.
- **Wheelspin in slides** (50%): once the rear slides under power the wheels spin up and the revs
  climb, so more throttle means more angle. Lower it if power slides turn into spins too easily.

### Downforce

- **Downforce** (70%): a share of each car's own aero, so the race cars keep more than the road
  cars. It only matters in fast corners: it grows with the square of the speed, as real aero
  does, so there's almost none in town. Lower it if the car feels glued down at speed.

## Steering feel (Force feedback page)

What you feel in the wheel comes from the front tyres:

- **Tyre feel**: the tyres' pull back toward where the car is going. Heavy while they grip, light
  once the front slides (that's how you feel understeer), and it pulls into the countersteer in a
  slide. The main strength of the steering.
- **Weight transfer**: how much the load on the front tyres changes the weight. Braking moves
  weight forward and the wheel gets heavier; full throttle lifts the nose and it gets lighter;
  crests go light, dips heavy. 100% follows the load fully; 80% is a little calmer.
- **Steering geometry (caster)**: the steering's own pull back to straight ahead, on top of the
  tyres'. It grows with front load too, and unlike the tyres' pull it only eases off a little
  when the front slides. Lower it if the steering is too heavy overall.
- **Light when the front loses grip** (60%): once the front tyres are past their limit, the wheel
  loses this much of its weight. You feel a front drive car spinning its wheels out of a corner,
  the front pushing wide, and locked front wheels under braking as a light, vague wheel. Its weight
  comes back over about 0.3 s as the tyres grip again, so it doesn't snap to the centre. Raise it for a clearer warning, lower it if the wheel goes too
  limp in slides. It reacts once the tyres actually spin or slide, not to the throttle alone.
- **Car's front weight**: how much the car's own weight on its front wheels sets the steering
  weight. Front-heavy and big cars steer heavier (SL65), rear-engined and light ones lighter (911,
  Elise). 0 = every car the same.

Too heavy overall: lower Steering geometry first, then Tyre feel, then Strength. Too vague or
numb: raise Tyre feel. Downforce adds little steering weight at speed: the wheel's weight follows
quick changes in load (braking, throttle, bumps), not the slow build-up of downforce, as the game
has far more of it than a road car.

## All settings

**Physics page** (`[Physics]` in `nfsmw_gameplay.ini`)

| Setting | ini key | Range | Default | Per car | The game's |
|---|---|---|---|---|---|
| Traction control | `TractionControl`, `TC` | off / game's / + launch control | off | strength | game's |
| ABS | `ABS` | on / off | on | | off |
| Stability control | `Stability` | 0–100% | 0% | yes | 100% |
| Drift assist | `DriftAssist` | 0–100% | 0% | yes | 100% |
| Car character | `Character` | 0–100% | 50% | yes | 0% |
| Real weight distribution | `RealWeight` | on / off | on | | off |
| Settings from | `[Group.<name>]` `Cars=`, or a `[Car.<name>]` section | Shared / a group / this car's own | Shared | | |
| Grip | `Grip` | 0.3–2.0x | 0.75x | yes | 1.0x |
| Rain grip | `RainGrip` | 50–100% | 90% | yes | 100% |
| Offroad grip | `OffroadGrip` | 20–100% | 95% | yes | 100% |
| Roll balance | `RollBalance` | −50–+50% | 0% | yes | 0% |
| Suspension firmness | `SuspensionFirmness` | 50–200% | 100% | yes | 100% |
| Grip balance | `GripBalance` | −50–+50% | 0% | yes | 0% |
| Grip when sliding | `TyreSlideGrip` | 50–100% | 85% | yes | 100% |
| Tyre grip peaks at | `TyrePeakAngle` | 3–20° | 8° | yes | (none) |
| Wheelspin in slides | `WheelspinInSlides` | 0–100% | 50% | yes | 0% |
| Throttle takes side grip | `FrictionCircle` | 0–300% | 100% | yes | 0% |
| Downforce | `Downforce` | −25–200% | 70% | yes | 100% |
| Wall bounce | `WallBounce` | 0–100% | 0% | | 100% |
| Gravity | `Gravity` | 0.1–3 g | 1 g | yes | 1 g |

In the ini only: `LoadSensitivity` (0.2: a more loaded tyre gains less than its share of grip, so
weight transfer, roll balance and downforce act as in a real car; 0 = the game's),
`DownforceMatchSpeed` (250 km/h: where the downforce, which grows with the square of the speed
while the physics sliders are on, equals the game's) and `MaxSpin`.

**Reset page to game defaults** sets everything on the page to the unmodded game's values, the last
column above.

**Force feedback page** (`[FFB]` in `nfsmw_ffb.ini`, steering part)

| Setting | ini key | Range | Default |
|---|---|---|---|
| Tyre feel | `Aligning` | 0–200% | 100% |
| Weight transfer | `LoadSensitivity` | 0–200% | 100% |
| Steering geometry (caster) | `Caster` | 0–200% | 20% |
| Car's front weight | `CarWeight` | 0–100% | 50% |
| Light when the front loses grip | `FrontGripLoss` | 0–100% | 60% |

## Reading the log

With `[Physics] LogPhysics=1` the mod writes a line a second to `nfsmw_gameplay.log` while you
drive. To see what a setting did, drive the corner, then look at that second:

- **character** (once per car) — the factors Car character and real weight gave it.
- **downforce** — how much of the car's weight the air adds now, and the game's own for comparison.
- **front / rear tyre curve** — the tyres' slip angle (past the peak angle the curve starts taking grip
  away) and how much it took.
- **rear driven** — how much of the tyres' grip the engine's torque used, and how often the friction
  circle held the side force back.
- **past the limit under power** — how loosely the driven wheels were tied to the road, and how much
  they spun.

For the steering, `[FFB] LogTelemetry=1` in `nfsmw_ffb.ini` writes `tel:` lines to `nfsmw_ffb.log`
ten times a second: the front load (`loadF`), the car's learned static front load (`loadRef`), the
front tyres' traction (`gripF`, 1 = gripping) and the weight kept for it (`light`), and each force
(`align`, `caster`, ...).
