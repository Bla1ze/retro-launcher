#pragma once

#include <SDL.h>

#include <cstddef>

// CRT filter: the game picture drawn through a CRT shader by Timothy Lottes
// (public domain) instead of a plain copy. It runs in SDL's own GLES2
// renderer context, so it keeps a copy of each frame in a GL texture of its
// own (a format it knows, not whatever SDL picked) and puts back every piece of
// GL state SDL 2.0.7 caches. If anything is missing (a software renderer, a
// shader that won't compile) draw() returns false and the plain copy is used.
namespace CrtFilter {

enum class Kind { Off, Lottes, LottesFast };

Kind kindNamed(const char* setting);  // "lottes", "lottes-fast", anything else = Off
void setKind(Kind k);
void setRefresh(double hz);  // the screen's rate: below it, the filter draws smaller
bool wanted();  // a filter is chosen and hasn't failed

// From the video callback (renderer context current): copies the frame.
// format is the libretro pixel format (0 = 0RGB1555, 1 = XRGB8888, 2 = RGB565).
void store(const void* data, unsigned w, unsigned h, size_t pitch, int format);

// Draws the frame's src rectangle into dst, turned clockwise by angle degrees
// around dst's center, as SDL_RenderCopyEx would. win: the drawable's size.
bool draw(const SDL_Rect& src, const SDL_Rect& dst, int angle, int winW, int winH);

void shutdown();

}  // namespace CrtFilter
