Most Wanted 2005 Immersion Pack Mod - VR, force feedback, gameplay (H-pattern shifter + clutch and more)
https://github.com/cook1e0/mw2005-immersion-pack

Needs speed.exe v1.3. The mods are .asi plugins loaded by Ultimate ASI Loader (dinput8.dll),
the same loader the widescreen fix and most other NFS mods use.

Quick install: extract the whole zip into the folder with speed.exe (all three mods, the
loader, ThirteenAG's widescreen fix and the VR helper), start the game, press F4 while driving
to set things up. Already have the widescreen fix set up? Skip
scripts\NFSMostWanted.WidescreenFix.ini when extracting, to keep its settings.

Or pick and choose:

1. Copy dinput8.dll next to speed.exe (skip it if you already have it, e.g. from the
   widescreen fix), and create a "scripts" folder there if there is none.
2. Copy the widescreen fix: scripts\NFSMostWanted.WidescreenFix.asi, .tpk and .ini (skip the
   .ini if you already have one). The VR mod needs it: it sets the game's physics rate to suit
   the headset through the fix's SimRate setting.
3. Copy the mods you want:

  VR               scripts\nfsmw_vr.asi + scripts\nfsmw_vr.ini, and nfsmw_vr_host.exe plus nfsmw_crashdump.exe next to
                   speed.exe
                   (Virtual Desktop, SteamVR or Meta Quest Link; for SteamVR set it as the
                   OpenXR runtime in SteamVR's settings and run Room Setup once)
  Force feedback   scripts\nfsmw_ffb.asi + scripts\nfsmw_ffb.ini
                   (ffbtool.exe, next to speed.exe, lists / tests wheels)
  Gameplay         scripts\nfsmw_gameplay.asi + scripts\nfsmw_gameplay.ini + scripts\nfsmw_physics_presets.ini
                   F4 while driving: bind wheel and pedals under Controls, an H-shifter
                   under Gear bindings

Each mod works on its own or together with the others. To uninstall, delete its .asi.
Settings are in the .ini files in scripts; each mod writes a .log next to speed.exe.

Crash capture: install nfsmw_crashdump.exe next to speed.exe. After an unhandled game crash,
send nfsmw_crash.dmp + nfsmw_crash.txt and the nfsmw_*.log files from the game folder.
Capture is automatic with existing ini files, requires no administrator setup and uploads
nothing. The latest crash replaces the previous dump/report; copy logs before restarting.
Dumps contain portions of game memory. [VR] CrashDumps=0 disables capture.
If the folder cannot be written, the helper tries %TEMP%\nfsmw_crash_<pid>.dmp and .txt;
nfsmw_crashdump.log records the path. Forced exits and hangs are not captured.

Upgrading from v0.25.0: replace scripts\nfsmw_gameplay.asi, nfsmw_ffb.asi and nfsmw_vr.asi (F4
now shows each mod's release, and flags a half-updated install). Your ini files keep working: the
new settings start at their defaults (FrictionCircle=1 under [Physics], EngineBrakingFade=0.1 under
[Engine]). PowerOversteer, SlideOversteer and UsePowerOversteer are
gone: the friction circle replaces them, and the old keys are ignored. ABS and traction control now
act under hard braking and wheelspin, and RainGrip only in the rain, so cars grip a little more in
the dry. See FIXES.md.

Upgrading from v0.24.0: replace scripts\nfsmw_gameplay.asi. Your ini files keep working. The new
tyre model is on for every car (LoadSensitivity=0.2, PastPeakGrip=0.85 under [Physics]; 0 and 1
give the game's tyres back). Suspension firmness and Roll balance are in F4 > Physics, per car.
See FIXES.md.

Upgrading from v0.23.0: replace scripts\nfsmw_gameplay.asi. Your ini files keep working; the new
[Physics] WallBounce starts at 0 (no bounce off walls; F4 > Physics). Your ini keeps its spin limit:
for the new 8 rad/s, set MaxSpin=8 in scripts\nfsmw_gameplay.ini and nfsmw_physics_presets.ini.
See FIXES.md.

Upgrading from v0.22.1: replace scripts\nfsmw_gameplay.asi. Your ini files keep working; the new
[Physics] DownforceSquared starts off (F4 > Physics). See FIXES.md.

Upgrading from v0.22.0: replace scripts\nfsmw_vr.asi. For side mirrors that show the rear view,
set GameMirrors=1 and MirrorScale=0.88 under [VR] in scripts\nfsmw_vr.ini (the new defaults).

Upgrading from v0.21.0 or v0.13.0: replace the three scripts\nfsmw_*.asi plugins,
nfsmw_vr_host.exe and nfsmw_crashdump.exe, and add scripts\nfsmw_vr_edits.ini. Your
scripts\nfsmw_*.ini files keep working (Front / Rear grip are read as Grip and Grip balance;
new settings start at their defaults). For spinesplitter's physics presets on every
car, also take the new scripts\nfsmw_physics_presets.ini. See FIXES.md.

Upgrading from v0.20.0: replace the three scripts\nfsmw_*.asi plugins, nfsmw_vr_host.exe and
nfsmw_crashdump.exe. Keep your scripts\nfsmw_*.ini files (new settings start at their defaults).
F4 opens one menu with a page per topic (each setting on one page). A car
with its own physics settings keeps them; give it a preset with Load into this car. See FIXES.md.

Upgrading from v0.13.0: replace the three scripts\nfsmw_*.asi plugins and nfsmw_vr_host.exe.
Keep your scripts\nfsmw_*.ini files: new settings start at their defaults. See FIXES.md.

Upgrading from v0.12.1: replace the three scripts\nfsmw_*.asi plugins and ffbtool.exe. A bound
wheel now steers the car directly (full lock at SteeringLock), the car's steering wheel turns with
yours, and the force feedback is rebuilt with new effects (F4 > Force feedback); its old [FFB]
settings are replaced, so check that page. See FIXES.md.

Upgrading from v0.12.0: replace scripts\nfsmw_vr.asi and scripts\nfsmw_gameplay.asi. Fixes
garbage polygons in the VR cockpit and a d3d9.dll crash after cars were streamed in and out.

Upgrading from v0.11.2: replace scripts\nfsmw_vr.asi and scripts\nfsmw_gameplay.asi. Keep
your existing .ini files and bindings. New: VR near clip plane (F4 > VR, [VR] NearZ) so the
cockpit close to your eyes isn't cut off, a bindable Recenter VR view (F4 > Controls), and a 50%
chance of 5% car damage when a shift is refused for lack of clutch (F4 > Shifter and clutch,
[Shifter] MisshiftChance=0 turns it off). See FIXES.md for this release.

Upgrading from v0.11.1: replace the three scripts\nfsmw_*.asi plugins and nfsmw_vr_host.exe,
and add nfsmw_crashdump.exe next to speed.exe. Keep your existing .ini files and bindings.
F10 (or F4 > Force Feedback) restarts the wheel's force feedback. See FIXES.md for this release.

Upgrading from v0.11.0: replace scripts\nfsmw_ffb.asi and scripts\nfsmw_gameplay.asi. Keep
your existing .ini files and bindings. Analog axes are now ignored in game menus by default,
preventing pedals and steering from scrolling them. F4 > Controls has settings for analog
axes and all controller input in game menus. Driving input is unaffected. The VR plugin
and helper are unchanged. See FIXES.md for this release.

Upgrading from v0.10.x: replace the three scripts\nfsmw_*.asi plugins and nfsmw_vr_host.exe
next to speed.exe. Keep your existing .ini files and bindings. New settings use defaults when
missing. The VR plugin and its helper must be from the same build. See FIXES.md for this release.

Upgrading from v0.9.1 or older: delete the old d3d9.dll and dsound.dll next to speed.exe (if
both copies are there, the old one runs). The old dinput8.dll is replaced by the loader. Leave your
.ini files where they are: on the first start each mod moves its own into scripts, replacing
the default one from the zip.

The widescreen fix (ThirteenAG; MIT licence in LICENSE-widescreen-fix.txt): the ini in this zip
is the fix's own with SkipIntro = 0, since the intro videos crashed the game in the boot screens
on our v1.3 exe. If the game crashes right at start with your own ini, set SkipIntro = 0 there.
SimRate = -1 runs the game's physics at the monitor's refresh rate; after a second in the
headset, the VR mod sets it to suit the headset (72 Hz -> 144) from the next start. The fix keeps the game's settings
in its own registry key starting from defaults (640x480, low detail): set them again in the
game's options. In VR set FixHUD = 0 there too: the headset shows the HUD and menus on a 4:3
panel, the game's own layout; the fix's correction for the nearly square eye picture squeezes the
HUD so its parts run into each other (and adds bars the in-car dashboard picks up as black).

Without an ASI loader: each .asi also works renamed to the DLL it used to be, next to speed.exe:
nfsmw_vr.asi -> d3d9.dll, nfsmw_ffb.asi -> dinput8.dll, nfsmw_gameplay.asi -> dsound.dll.
Its .ini then goes next to speed.exe too.

Upgrading from v0.6.0 or older: the H-shifter mod is now the gameplay mod. On the first start
nfsmw_hshifter.ini is renamed to nfsmw_gameplay.ini with your settings. hshiftertool.exe and
nfsmw_hshifter.log can be deleted.

Gameplay: wheel, pedal and button bindings with a steering lock (the game's own controller
settings aren't needed), H-pattern shifter and clutch, engine heat / over-rev / blow-ups (manual
shifting only), car damage and engine smoke, drivable popped tires, physics sliders, no SUV
cops, a damage / heat meter in place of the speedbreaker meter, and an in-game tuning menu on F4
while driving (categories, sliders, mouse on a flat screen, reset to mod or game defaults, and
the force feedback settings when that mod is installed). Controls includes steering linearity.

Force feedback: steering weight from the game's tyre model, a centring spring that grows with
speed (parked strength and full-strength speed adjustable in F4), and a soft lock at the gameplay
mod's steering lock.

VR extras: in the bumper and hood cameras you sit in the driver's seat (also on a flat screen).
F4 > VR: recenter view and adjust HUD size, distance and height. For headset play use FakeHmd=0.
F7 toggles horizon levelling; it is enabled by default for comfort.

Licences: the mods are MIT (LICENSE). Ultimate ASI Loader by ThirteenAG
(https://github.com/ThirteenAG/Ultimate-ASI-Loader) is MIT: LICENSE-asi-loader.txt.
Dear ImGui (used by the gameplay mod's menu) is MIT: LICENSE-imgui.txt.
