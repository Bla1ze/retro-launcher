#pragma once

#include "AppTypes.h"

#include <SDL.h>

#include <cmath>

// Rendering converts float rectangles to SDL_Rect so the app stays compatible
// with older firmware SDL2 versions.
inline void render_rect(SDL_Renderer* renderer, const FRect& rect, SDL_Color color)
{
    SDL_Rect draw_rect {
        static_cast<int>(std::lround(rect.x)),
        static_cast<int>(std::lround(rect.y)),
        static_cast<int>(std::lround(rect.w)),
        static_cast<int>(std::lround(rect.h))
    };

    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &draw_rect);
}
