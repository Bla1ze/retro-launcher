#pragma once

#include <SDL.h>

#include <string>

// Text renderer with two embedded typefaces.
//
// Body    = Noto Sans (clean, readable) for lists, tables, and hints.
// Display = Bebas Neue (tall condensed all-caps) for titles, headers, and big
//           numbers/scores — the "designed" look.
//
// Both TTFs are objcopy'd into the ELF by CMake and baked once into a texture
// atlas with stb_truetype (the same trick LeaderboardOverlay uses); no font
// files or font library are needed at runtime. Every function takes the
// renderer because each atlas is lazily baked for it on first use.
namespace AppFont {

enum class Face { Body, Display };

// Width in pixels the text would occupy if drawn at the given size and face.
// Used to center/right-align text or fit it to an available width.
float measureWidth(SDL_Renderer* renderer, const std::string& text, float size,
                   Face face = Face::Body);

// Draw text with (x, y) as the top-left corner of the text's bounding box.
void draw(SDL_Renderer* renderer, const std::string& text, float x, float y,
          float size, SDL_Color color, Face face = Face::Body);

// Draw text centered horizontally on center_x.
void drawCentered(SDL_Renderer* renderer, const std::string& text, float center_x,
                  float y, float size, SDL_Color color, Face face = Face::Body);

// Draw text right-aligned so it ends at right_x.
void drawRight(SDL_Renderer* renderer, const std::string& text, float right_x,
               float y, float size, SDL_Color color, Face face = Face::Body);

} // namespace AppFont
