#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>

// Fills the bars beside a game with light taken from the game itself, instead
// of flat black:
//   - glow:   the frame averaged to a 16x9 colour grid, stretched over the whole
//             screen with linear filtering and darkened, so each bar picks up
//             the colours at that edge of the picture;
//   - shadow: a soft falloff around the picture so it sits above the glow.
// Colours ease toward the game's, so flashes in the game don't strobe the bars.
// Only CPU work is a sparse sample of the frame every few frames.
class Ambient {
public:
    enum class Style { Ambient, Black };

    bool init(SDL_Renderer* r, Style style);
    void shutdown();

    // Feed the core's frame (any libretro pixel format: 0 = 0RGB1555,
    // 1 = XRGB8888, 2 = RGB565). Cheap; samples only every few frames.
    void sample(const void* data, unsigned w, unsigned h, size_t pitch, int pixelFormat);

    // Draw everything behind the game picture. `game` is the picture's
    // on-screen rectangle (already rotated), screen is winW x winH.
    void draw(const SDL_Rect& game, int winW, int winH, float dt);

private:
    struct Rgb { float r, g, b; };
    static constexpr int kCols = 16, kRows = 9;

    void bakeTextures();

    SDL_Renderer* m_r = nullptr;
    Style m_style = Style::Ambient;
    SDL_Texture* m_grid = nullptr;     // kCols x kRows, stretched full screen
    SDL_Texture* m_shadow = nullptr;   // 1D falloff for the picture's edges
    Rgb m_target[kCols * kRows];
    Rgb m_shown[kCols * kRows];
    bool m_haveColors = false;
    unsigned m_frameCount = 0;
    float m_time = 0.0f;
};
