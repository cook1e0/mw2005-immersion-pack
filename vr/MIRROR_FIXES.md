# Mirror fixes on v0.29.0

These changes were developed against the upstream
[v0.29.0 release](https://github.com/cook1e0/mw2005-immersion-pack/releases/tag/v0.29.0),
commit `4cbe49f84d413efcefb63ed1807f3c5ec9b1226f`.

The local mirror-fix session covered the Monaro, Carrera GT, Punto, Cayman S
and Camaro. The implementation includes:

- Recover original triangles for mirror detection when editor hides have
  collapsed their live indices, without unhiding their draw geometry.
- Split glass out of shared draw entries even when only one side needs it.
  This is a shared fix, not a Monaro-specific triangle override.
- Use the complete, distinct user-picked glass triangles for the stock
  Carrera GT side-mirror solids (18 per side) and Camaro body (14 per side).
  Keep the normal detection path when the complete selection is unavailable.
- Restore original vertex UVs after native mirror rendering, including its
  fallback path, so shared body vertices retain their own texture mapping.
- Intersect the Punto overlay outline with its horizontal reflection, keeping
  the result symmetric without expanding it into the mirror housing.
- Preserve the installed Cayman S and Punto interior-mirror offsets of
  `0.0100` metres toward the driver in `nfsmw_vr_mirror.ini`.

## Building and installing

Run `vr\tests\run.cmd` and `vr\build.cmd` using the Visual Studio 2017 Build
Tools configured by those scripts. With an ASI-loader installation, install
the resulting `vr\bin\d3d9.dll` as `scripts\nfsmw_vr.asi`. Keep the matching
v0.29.0 VR host and other release components installed.

For existing settings, merge the two `[MirrorOffset]` values from
`vr\nfsmw_vr_mirror.ini` into `scripts\nfsmw_vr_mirror.ini` rather than
overwriting other per-car settings.

Automated checks cover hidden geometry, side-specific Carrera GT and Camaro
triangle selection, and Punto outline symmetry. Visual mirror alignment and
Direct3D rendering still require an in-game check with the actual car models.
Personal driver, wheel, dashboard and other editor settings are not part of
this mirror-only change.