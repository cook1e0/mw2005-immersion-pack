### Changes since v0.10.1

- **Steering linearity**: a new slider in F4 > Controls adjusts the steering response. 1 is linear; higher values give finer control around the centre.
- **VR controls in F4**: recenter the view and adjust the HUD panel's size, distance and height under VR. Changes take effect immediately and are saved when the menu closes, including with SteamVR.
- **Centring spring**: self-centring now grows with speed. Its strength when parked and the speed where it reaches full strength are adjustable in F4 > Force feedback.
- **Cockpit camera**: the seat follows the car body properly, including on a flat screen, and stays fixed through model-detail changes. Horizon levelling remains on by default; F7 toggles it.
- **VR rendering**: more temporary render memory for busy stereo scenes, addressing a possible cause of cars disappearing in the right eye and the game crashing. Tested in desktop stereo; the reported SteamVR crash still needs confirmation in a real headset.

**Upgrading:** replace all three `scripts\nfsmw_*.asi` plugins and `nfsmw_vr_host.exe` next to `speed.exe`. Keep your existing `.ini` files and bindings. The VR plugin and helper must be from the same build.
