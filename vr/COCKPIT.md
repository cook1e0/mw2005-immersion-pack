# Steering wheel location

Open **F4 > Menu and testing** and adjust **Steering wheel location left / right**
or **Steering wheel location up / down**. Positive values move right or up;
negative values move left or down. Each step is 0.1 cm, with a range of ±50 cm.
These move only the detected/selected steering-wheel geometry, not the dashboard,
seat, or rotation centre, and work even when the steering is straight.

Locations are saved automatically per car in `[WheelPosition]` in
`scripts\nfsmw_vr_wheel.ini`, as `car=right,up` in metres. Cars without a saved
location start at zero. Set both sliders to zero to restore the original position.
The existing animation-centre controls remain separate. Steering-wheel detection
must be enabled; for models needing manual selection, use the triangle editor.

# Cockpit notes

Seat and head removal were tested in Mustang GT free roam with `FakeHmd=1` on the desktop.
On 2026-10-14 the body-locked seat reported zero matrix difference across 50,576 draws
while driving. Stereo frame memory reached 705 KB, above the original 400 KB limit, without
allocation-failure warnings. This remains a desktop stereo test, not real-headset validation.
The x86 regression harness covers body motion, offsets, current POV selection, car unloads,
anchor switches, LOD changes and frame lifecycle.

## What it does

- **Seat:** in the bumper and hood cameras the VR eyes start at the driver model's head, so you sit
  in the driver's seat with the car's own dashboard, wheel and the driver's hands in front of you.
  Numpad 8/2, 4/6, 9/3 move the seat (held), 5 resets; saved per car in `nfsmw_vr_seat.ini`.
- **Head:** the driver model's head is left out when it is within 0.6 m of the eyes (only the
  player's car in practice). `HeadSize` (default 0.25 m) is how far down from the top of the driver
  model is cut.

Code: `src/cockpit.cpp`, hooked into `game.cpp` (`Cockpit_ApplySeat` before levelling in
`hkRenderScene`, `Cockpit_EndFrame` in `Game_EndOfFrame`) and `d3d9proxy.cpp` (`Cockpit_Keys`).

## Open items

- In the first screenshot there is a dark angled shape left of the wheel: A-pillar, or a shoulder
  or neck piece left by the cut? Check in the headset, try a larger `HeadSize`.
- Only the Mustang was seen. Check other cars (the head test assumes the head is the top
  `HeadSize` and the rear 0.4 m of the driver model: x forward, z up).
- The seat follows the car body as it is drawn this frame (see Camera below), so the bumper and
  hood cameras' lag and shake no longer move you inside the car. The head position is measured in
  body space once per car and stays fixed across driver LOD changes. The current render connection
  is resolved by world id each frame, never retained as a pointer across car unloads. The current
  camera anchor selects the body or smoothed matrix; its previous frame's POV mode is not reused.
  Seat is off when the camera is more than 3 m from the head (chase cams). F7 disables horizon
  levelling, which otherwise intentionally changes the camera rotation relative to the body.
- Seat controls are keyboard only. The gameplay mod's F4 menu has a VR page (recenter, HUD panel)
  through the VR mod's `NfsmwVr_Setting` / `NfsmwVr_Recenter` exports; seat offsets could go there too.
- Friend's wishlist still open: wheel and hands turning with steering, working mirrors (the rear
  view is on the flat HUD panel), cockpit EQ.

## Reverse engineering (speed.exe v1.3)

### Car parts

- Each car has `<CAR>_KIT00_INTERIOR_A..D` (dash, A-pillar, steering wheel, side mirror) and
  `<CAR>_KIT00_DRIVER_A..D` (body, arms, hands, head) solids. No separate wheel part: the wheel is
  in the interior mesh, the hands in the driver mesh, and the game never animates them.
- Car slots (decomp `CarPartID.h`): `CARSLOTID_INTERIOR = 28`, `CARSLOTID_DRIVER = 43`.
  `CarRenderInfo::Render` (decomp `CarRender.cpp`) draws the driver only if
  `NISRaceDriverVisible` (0x90330C) / `NISCopCarDriverVisible` allow it, never in the frontend.
- `CarRenderInfo::RenderPart` 0x7387E0 -> `eView::Render(model, local_to_world, light, flags)`.
- Driver textures: `DRIVER_PLAYER`, `DRIVER_CUTOUT`; `REPLACETEX_DRIVER` swaps male / female /
  helmet driver skins.

### Model drawing

- `eViewPlatInterface::Render(eModel*, bMatrix4* local_to_world, eLightContext*, flags, ...)`
  0x6DA9B0 walks the solid's draw entries and queues each visible one with
  `call sub_6E0AB0(entry, ...)` at 0x6DAD17 (cdecl, 9 args; the draw happens later, so a clip
  plane around the call would not work). Hooked with a naked stub that reads the caller's
  `[ebp+8]` (model) and `[ebp+0Ch]` (matrix).
- `eModel`: +0x08 name hash, +0x0C `eSolid*`.
- `eSolid` (decomp `Ecstasy.hpp`): +0x00 plat info, +0x10 name hash, +0x20 AABB min xyz,
  +0x30 AABB max xyz, +0xA0 `char Name[64]`. Model space: x forward, y left, z up.
- Plat info: +0x10 entry count, +0x14 entries, +0x24 index data (system memory copy, uint16),
  +0x28 `IDirect3DIndexBuffer9*`, +0x2C index count.
- Draw entry, 0x68 bytes: +0x00 AABB min, +0x0C AABB max, +0x34 vertex format (+8 = stride),
  +0x38 flags, +0x3C vertex count, +0x40 triangle count, +0x44 first index, +0x4C vertex data
  (first entry only; float xyz, normal xyz, colour, uv), +0x50 vertex data bytes,
  +0x54 `IDirect3DVertexBuffer9*`, +0x58 total vertices, +0x5C index count. Triangle lists.
- BMW M3 GTR driver (LOD A): 942 vertices, stride 0x24. Entry 0 = 1000 triangles (body, arms,
  head), entry 1 = 168 triangles (hands). Head: z 0.85..1.09, x -0.15..0.08; hands at x 0.37..0.58,
  z 0.70..0.85. Mustang: 395 of 1000 entry-0 triangles are head.

### Head removal method

The first time a driver solid is queued, each entry's triangles are reordered so head triangles
(centroid z > top - HeadSize and x < min x + 0.4) come last, in both the system-memory indices
and the D3D index buffer (Lock works on the D3D9Ex default-pool buffers). Instances whose head is
near the eyes get a copy of the entry with the triangle count (+0x40) and index count (+0x5C)
cut to the non-head part; everything else draws the full, reordered entry.

### Camera

- Player view `eViews[1]` at 0x919650, camera at +0x40, camera position at +0x40 in it.
- Game camera space: +x right, +y down, +z forward; world is Z-up. `M4` rows of the inverse view
  are right, down, forward, position.
- `CarRenderConn` (vtable 0x8B5254, `OnRender` at slot 3 = 0x750E20) draws the car with: in an
  in-car camera of the view (`view+0x44` first mover - 4, `GetAnchor()` = vtable +0x10, anchor
  +0x94 == conn +0x30 world id and int16 anchor +0xC8 == 1) the body's own matrix `*[conn+0x34]`
  with its row 3 moved by -(`conn+0x50` bVector4 through the matrix, D3DXVec4Transform);
  otherwise the smoothed render matrix at `conn+0x330`. Both are up to date before
  `___eDisplayFrame`. The VR mod hooks the vtable slot to know which car (and body matrix) a
  driver solid belongs to.

### Tools used

- Live memory: `ReadProcessMemory` on `speed.exe` from Python (32-bit addresses).
- Textures: TPK chunks `0xB3300000`; texture headers in `0x33310004` (0x7C each, name at +0x0C,
  hash +0x24, data offset +0x30, size +0x38, width/height +0x44), formats in `0x33310005`
  (0x20 each, FourCC at +0x14), data in `0x33320002` aligned to 0x80. Frontend textures (e.g. the
  main menu's quick race icon) are not loaded in game.
