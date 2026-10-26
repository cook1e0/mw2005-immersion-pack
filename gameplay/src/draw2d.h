// 2D drawing for the mod's overlays (the heat gauge, the classic menu, the telemetry panel):
// untextured quads, and text from a font rasterised once with GDI and stored as horizontal pixel
// runs. No textures means nothing to lose on a device reset and nothing that D3D9Ex (which the VR
// mod switches to) can't create.
//
// Shapes queue up in one batch, drawn and emptied by Flush. Coordinates are pixels of the target
// whose size is given to Flush. A colour's alpha is also the coverage on the VR mod's HUD layer.
#pragma once
#include <d3d9.h>
#include <vector>

namespace draw2d {

struct Run { short y, x0, x1; };
struct Glyph { int advance = 0; std::vector<Run> runs; };
struct Font {
    int px = 0;         // requested size
    int height = 0;     // line height in pixels, 0 when it could not be made
    Glyph glyph[96];    // ' ' .. '~'
};

// Rasterises `f` at px pixels (each overlay keeps its own, so two sizes never rebuild each other).
void BuildFont(Font& f, int px);
// The font Text, ShadowText and TextWidth use from now on.
void UseFont(const Font& f);

void Quad(float x0, float y0, float x1, float y1, D3DCOLOR c);
// Four free corners, x y pairs in order around the shape.
void Quad4(const float* xy, D3DCOLOR c);
void Line(float x0, float y0, float x1, float y1, float thick, D3DCOLOR c);
float TextWidth(const char* t);
void Text(float x, float y, const char* t, D3DCOLOR c);
// Text with a shadow, which keeps it readable over the game's picture.
void ShadowText(float x, float y, const char* t, D3DCOLOR c, float k);
// Draws everything queued so far into the current render target (w x h) and empties the queue.
void Flush(IDirect3DDevice9* dev, int w, int h);

}  // namespace draw2d
