#include "Ambient.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

} // namespace

bool Ambient::init(SDL_Renderer* r, Style style) {
    m_r = r;
    m_style = style;
    for (int i = 0; i < kCols * kRows; ++i) m_target[i] = m_shown[i] = {0.0f, 0.0f, 0.0f};
    if (style == Style::Black) return true;

    // Linear filtering is what turns the 16x9 grid into a smooth wash.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    m_grid = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kCols, kRows);
    bakeTextures();

    return m_grid != nullptr;
}

void Ambient::shutdown() {
    if (m_grid) SDL_DestroyTexture(m_grid);
    if (m_shadow) SDL_DestroyTexture(m_shadow);
    m_grid = m_shadow = nullptr;
}

void Ambient::bakeTextures() {
    // Shadow: black, alpha falling from 200 at the picture edge to 0.
    const int sw = 64;
    std::vector<uint32_t> sh(sw);
    for (int x = 0; x < sw; ++x) {
        float t = 1.0f - (x + 0.5f) / sw;
        sh[x] = (uint32_t)(t * t * 200.0f) << 24;
    }
    m_shadow = SDL_CreateTexture(m_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, sw, 1);
    if (m_shadow) {
        SDL_UpdateTexture(m_shadow, nullptr, sh.data(), sw * 4);
        SDL_SetTextureBlendMode(m_shadow, SDL_BLENDMODE_BLEND);
    }
}

void Ambient::sample(const void* data, unsigned w, unsigned h, size_t pitch, int pixelFormat) {
    if (m_style == Style::Black || !data || w < kCols || h < kRows) return;
    if (m_frameCount++ % 4 != 0 && m_haveColors) return;
    const uint8_t* base = static_cast<const uint8_t*>(data);
    for (int gy = 0; gy < kRows; ++gy)
        for (int gx = 0; gx < kCols; ++gx) {
            unsigned x0 = gx * w / kCols, x1 = (gx + 1) * w / kCols;
            unsigned y0 = gy * h / kRows, y1 = (gy + 1) * h / kRows;
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (unsigned y = y0; y < y1; y += 3)
                for (unsigned x = x0; x < x1; x += 3) {
                    const uint8_t* p = base + y * pitch;
                    if (pixelFormat == 1) {
                        uint32_t v;
                        std::memcpy(&v, p + x * 4, 4);
                        r += (v >> 16) & 0xff; g += (v >> 8) & 0xff; b += v & 0xff;
                    } else {
                        uint16_t v;
                        std::memcpy(&v, p + x * 2, 2);
                        if (pixelFormat == 2) {
                            r += ((v >> 11) & 31) * 8.226f; g += ((v >> 5) & 63) * 4.048f; b += (v & 31) * 8.226f;
                        } else {
                            r += ((v >> 10) & 31) * 8.226f; g += ((v >> 5) & 31) * 8.226f; b += (v & 31) * 8.226f;
                        }
                    }
                    ++n;
                }
            if (n) m_target[gy * kCols + gx] = {r / n, g / n, b / n};
        }
    if (!m_haveColors) std::memcpy(m_shown, m_target, sizeof(m_shown));
    m_haveColors = true;
}

void Ambient::draw(const SDL_Rect& game, int winW, int winH, float dt) {
    SDL_SetRenderDrawColor(m_r, 0, 0, 0, 255);
    SDL_RenderClear(m_r);
    if (m_style == Style::Black || !m_grid || !m_haveColors) return;
    m_time += dt;

    // Ease toward the game's colors (about a quarter second).
    float k = std::min(1.0f, dt * 4.0f);
    for (int i = 0; i < kCols * kRows; ++i) {
        m_shown[i].r += (m_target[i].r - m_shown[i].r) * k;
        m_shown[i].g += (m_target[i].g - m_shown[i].g) * k;
        m_shown[i].b += (m_target[i].b - m_shown[i].b) * k;
    }

    // Glow: the grid, darkened and lifted a little in saturation, over the
    // whole screen. The picture covers the middle; the bars see its edges.
    uint32_t px[kCols * kRows];
    for (int i = 0; i < kCols * kRows; ++i) {
        const Rgb& c = m_shown[i];
        float l = (c.r + c.g + c.b) / 3.0f;
        auto ch = [l](float v) { return std::max(0.0f, std::min(255.0f, (l + (v - l) * 1.35f) * 0.42f)); };
        px[i] = 0xff000000u | ((uint32_t)ch(c.r) << 16) | ((uint32_t)ch(c.g) << 8) | (uint32_t)ch(c.b);
    }
    SDL_UpdateTexture(m_grid, nullptr, px, kCols * 4);
    // Inset by half a cell so the outer cells' color reaches the screen edge.
    SDL_Rect full{-winW / (2 * kCols), -winH / (2 * kRows), winW + winW / kCols, winH + winH / kRows};
    SDL_RenderCopy(m_r, m_grid, nullptr, &full);

    // Soft shadow hugging the picture's left and right edges (games on the
    // backglass are wider than tall, so the bars are always at the sides).
    if (m_shadow) {
        const int s = std::max(16, winW / 40);
        SDL_Rect l{game.x - s, game.y, s, game.h}, r{game.x + game.w, game.y, s, game.h};
        SDL_RenderCopyEx(m_r, m_shadow, nullptr, &l, 0.0, nullptr, SDL_FLIP_HORIZONTAL);
        SDL_RenderCopy(m_r, m_shadow, nullptr, &r);
    }
}
