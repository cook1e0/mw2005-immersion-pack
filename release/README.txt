NFS Most Wanted (2005) mods - VR, force feedback, gameplay (H-pattern shifter + clutch and more)
https://github.com/cook1e0/nfsmw05-mod

Needs speed.exe v1.3. The mods are .asi plugins loaded by Ultimate ASI Loader (dinput8.dll),
the same loader the widescreen fix and most other NFS mods use.

1. Copy dinput8.dll next to speed.exe (skip it if you already have it, e.g. from the
   widescreen fix), and create a "scripts" folder there if there is none.
2. Copy the mods you want:

  VR               scripts\nfsmw_vr.asi, and nfsmw_vr_host.exe + nfsmw_vr.ini next to speed.exe
                   (Virtual Desktop, SteamVR or Meta Quest Link; for SteamVR set it as the
                   OpenXR runtime in SteamVR's settings and run Room Setup once)
  Force feedback   scripts\nfsmw_ffb.asi, and nfsmw_ffb.ini next to speed.exe
                   (ffbtool.exe lists / tests wheels)
  Gameplay         scripts\nfsmw_gameplay.asi, and nfsmw_gameplay.ini next to speed.exe
                   F4 while driving: bind wheel and pedals under Controls, an H-shifter
                   under Gear bindings

Each mod works on its own or together with the others. To uninstall, delete its .asi.
Settings are in the .ini files; each mod writes a .log next to speed.exe.

Upgrading from v0.9.1 or older: delete the old d3d9.dll and dsound.dll next to speed.exe (if
both copies are there, the old one runs). The old dinput8.dll is replaced by the loader. Your
.ini files stay as they are.

With the widescreen fix: all three mods run alongside it. If the game crashes right at start,
set SkipIntro = 0 in scripts\NFSMostWanted.WidescreenFix.ini. The fix keeps the game's settings
in its own registry key starting from defaults (640x480, low detail): set them again in the
game's options.

Without an ASI loader: each .asi also works renamed to the DLL it used to be, next to speed.exe:
nfsmw_vr.asi -> d3d9.dll, nfsmw_ffb.asi -> dinput8.dll, nfsmw_gameplay.asi -> dsound.dll.

Upgrading from v0.6.0 or older: the H-shifter mod is now the gameplay mod. On the first start
nfsmw_hshifter.ini is renamed to nfsmw_gameplay.ini with your settings. hshiftertool.exe and
nfsmw_hshifter.log can be deleted.

Gameplay: wheel, pedal and button bindings with a steering lock (the game's own controller
settings aren't needed), H-pattern shifter and clutch, engine heat / over-rev / blow-ups (manual
shifting only), car damage and engine smoke, drivable popped tires, physics sliders, no SUV
cops, a damage / heat meter in place of the speedbreaker meter, and an in-game tuning menu on F4
while driving (categories, sliders, mouse on a flat screen, reset to mod or game defaults, and
the force feedback settings when that mod is installed).

Force feedback: steering weight from the game's tyre model, a centring spring, and a soft lock
at the gameplay mod's steering lock.

VR extras: in the bumper and hood cameras you sit in the driver's seat (also on a flat screen).

Licences: the mods are MIT (LICENSE). Ultimate ASI Loader by ThirteenAG
(https://github.com/ThirteenAG/Ultimate-ASI-Loader) is MIT: LICENSE-asi-loader.txt.
Dear ImGui (used by the gameplay mod's menu) is MIT: LICENSE-imgui.txt.
