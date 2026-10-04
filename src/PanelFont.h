#pragma once

#include <cstdint>
#include <string>

// CPU text rasterizer for the secondary panels. The panels are raw framebuffers
// (not SDL renderers), so AppFont's texture-atlas path can't reach them — this
// draws glyphs straight into an RGBA byte canvas with stb_truetype. It reuses
// the same two TTFs already objcopy'd into the ELF (Noto Sans + Bebas Neue), so
// no extra assets are needed. The stb_truetype implementation lives in
// AppFont.cpp; this only uses it.
namespace PanelFont {

enum class Face { Body, Display };

// Pixel width the text would occupy at the given pixel height.
float measure(const std::string& text, float pxHeight, Face face);

// Alpha-blend `text` into an RGBA canvas (w x h, tightly packed, pitch = w*4).
// (x, y) is the top-left of the text's em box; color is straight RGB. Pixels
// are only written where clipX0 <= column < clipX1 (defaults span the canvas),
// which keeps a scrolling marquee from spilling past its region.
void draw(uint8_t* canvas, int w, int h, const std::string& text,
          float x, float y, float pxHeight, uint8_t r, uint8_t g, uint8_t b,
          Face face, int clipX0 = 0, int clipX1 = 1 << 30);

} // namespace PanelFont
