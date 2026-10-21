// SMAA (Jorge Jimenez et al., iryoku/smaa, MIT licence; third_party/smaa) on each eye's picture:
// the three passes of the reference implementation (luma edge detection, blending weights,
// neighbourhood blending), compiled once with the system's d3dcompiler_47 for shader model 3.
#pragma once
#include <d3d9.h>

// SMAA's quality presets: 1 low, 2 medium, 3 high, 4 ultra (how far edges are searched and how
// fine the edge threshold is).
// Compiles a preset's shaders (each once, a few hundred ms) and uploads SMAA's area and search
// textures. False if d3dcompiler_47 or a shader is missing: SMAA then stays off.
bool Smaa_Prepare(IDirect3DDevice9* dev, int preset);
// The picture in `src` (any surface: the back buffer, multisampled or not) antialiased into
// `dst`, a w x h render target, with `preset`. With showEdges, dst gets the edges SMAA found
// instead (red: left edges, green: top edges), to see it working. False (dst untouched) if SMAA
// is not ready.
bool Smaa_Apply(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst, int w, int h, int preset,
                bool showEdges);
// The device's resources (before a Reset or release); Smaa_Prepare / Smaa_Apply make them again.
void Smaa_Release();
