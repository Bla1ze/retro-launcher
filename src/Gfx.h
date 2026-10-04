#pragma once

#include "AppTypes.h"

#include <SDL.h>

#include <string>

// Anti-aliased drawing on the firmware's SDL 2.0.7, which has no geometry API,
// no circle or rounded-rect primitives, and no per-texture scale modes.
//
// The trick: every soft shape is rasterized ONCE on the CPU into a small RGBA
// texture (white, coverage in alpha), then drawn with SDL_RenderCopy /
// SDL_RenderCopyEx and tinted per draw with color/alpha mod. The global linear
// filter (SDL_HINT_RENDER_SCALE_QUALITY, set in App::init) smooths the edges
// at any size, and RenderCopyEx gives true rotation. One textured quad replaces
// the hundreds of 1px fill strips the old helpers stacked up per shape.
//
// Shapes whose look depends on their exact size (rounded panels, soft shadows)
// are baked per size into a small LRU cache; size-independent ones (discs,
// capsules, triangles, glow blobs) are single shared textures.
namespace Gfx {

// Call once after the renderer exists. `scale` is the canvas scale factor (the
// portrait canvas is rendered at 3x on a 4K panel); baked textures use it so
// edges stay crisp instead of being magnified.
void init(SDL_Renderer* renderer, float scale);
void shutdown();

// --- primitives -------------------------------------------------------------

// Solid rect (alpha honored). Thin wrapper so callers needn't touch blend state.
void rect(SDL_Renderer* r, const FRect& rc, SDL_Color c);

// Filled anti-aliased disc.
void disc(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color c);

// Anti-aliased ring (outline circle) of the given stroke width.
void ring(SDL_Renderer* r, float cx, float cy, float radius, float stroke, SDL_Color c);

// Thick anti-aliased line with round caps (a rotated capsule).
void line(SDL_Renderer* r, float x0, float y0, float x1, float y1, float thick, SDL_Color c);

// Filled rounded rectangle (true circular corners, anti-aliased).
void roundRect(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color c);

// Rounded panel with a vertical gradient fill and an optional inner border.
// Baked per (size, radius, colors) and cached — use for cards, tiles, keys.
void panel(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color top, SDL_Color bottom,
           SDL_Color border = {0, 0, 0, 0}, float borderWidth = 0.0f);

// Soft blurred shadow / glow around a rounded rect. `spread` is the blur
// distance in logical px. Additive when `additive` (a glow), else a darkening
// shadow drawn with normal blending.
void softRect(SDL_Renderer* r, const FRect& rc, float radius, float spread, SDL_Color c,
              bool additive);

// Radial soft blob (gaussian falloff), additive by default — ambient lighting.
void glow(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color c, bool additive = true);

// Filled anti-aliased triangle pointing right, inscribed in the box, rotated by
// `degrees` about the box center (90 = down, 180 = left, 270 = up).
void triangle(SDL_Renderer* r, const FRect& box, double degrees, SDL_Color c);

// Heart, drawn from two discs and a rotated soft square.
void heart(SDL_Renderer* r, float cx, float cy, float size, SDL_Color c);

// Vertical gradient over a plain rect (a 1xN ramp texture stretched: one draw,
// no banding). `top`/`bottom` alpha honored.
void vGradient(SDL_Renderer* r, const FRect& rc, SDL_Color top, SDL_Color bottom);

// Horizontal gradient over a plain rect.
void hGradient(SDL_Renderer* r, const FRect& rc, SDL_Color left, SDL_Color right);

// Vinyl record texture (grooves, sheen band, two-tone label) drawn rotated by
// `degrees`, so the spin is visible. Label colors tint the center.
void vinyl(SDL_Renderer* r, float cx, float cy, float radius, double degrees,
           SDL_Color labelA, SDL_Color labelB);

// A short rounded bar rotated about the canvas point (cx, cy): the bar starts
// `inner` px from the center and extends `length` px outward at `degrees`.
// Used for the radial spectrum around the record.
void ray(SDL_Renderer* r, float cx, float cy, float inner, float length, float thick,
         double degrees, SDL_Color c);

// Mix two colors (alpha interpolated too).
SDL_Color mix(SDL_Color a, SDL_Color b, float t);
SDL_Color withAlpha(SDL_Color c, Uint8 a);

} // namespace Gfx
