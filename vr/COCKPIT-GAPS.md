# Stock cockpit gap closures (2026-10-28)

Scope: only the `1cookie` installation. Normal CORVETTE, all car binaries, editor
Hide/Wheel/Black assignments and gameplay settings are unchanged.

`CockpitGapTris` in `src/cockpit.cpp` supplies opaque black triangles only for the
exact MURCIELAGO, CTS and CORVETTEC6R KIT00_INTERIOR_A solid names. The triangles
use that static solid's model matrix, not steering-wheel animation. Existing black
editor triangles and fillers share one renderer submission to preserve both.
The renderer depth-tests and writes depth, with culling disabled.

Geometry measurements (car-local coordinates, metres):

* Murcielago: six triangles join the instrument binnacle's underside lip
  (interior vertices 777/780/784) to its forward rim (789/790), terminating at
  shared corners 786/791. The instrument face is not capped.
* CTS: sixteen triangles bridge the interior cowl (x approximately 1.04–1.17,
  z approximately .732–.739) to the stock hood rear hem (x approximately
  1.10–1.21, z approximately .775–.796). Intermediate stations interpolate
  the hood hem. This was measured on the stock hood, not every upgrade hood.
* C6.R: ten triangles bridge the interior front boundary (391/539/611 and
  adjacent outer stations) to the body cowl. The outer corner is shared;
  the central separation is approximately .14 m longitudinal and .074 m high.

Validation: production build and cockpit/hook/XR synthetic tests passed,
including nondegenerate bounded filler geometry, exact-name scoping and a
regression check preserving existing black edits and their eye lift.
No live headset/game validation has been performed. Offline diagnostic views
are two-sided, untextured and use approximate eye positions; they are not game
screenshots. Alternate hoods, seat offsets and moving-head clearance still need
in-game confirmation. The binary parser retains its existing 36-byte-vertex and
header-count assumptions; it checks finite positions and index ranges.

Backup directory:
`T:\Games\ModWanted\1cookie Need For Speed Most Wanted Black Edition\mod-builds\b2762ee-with-wheel\vr\gap-backup-20261028-033606`

To roll back with the game closed, copy that directory's `nfsmw_vr.asi` over
`T:\Games\ModWanted\1cookie Need For Speed Most Wanted Black Edition\scripts\nfsmw_vr.asi`.
It also contains the pre-change production source and cockpit test source.