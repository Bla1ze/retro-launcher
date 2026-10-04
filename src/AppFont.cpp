// Font renderer derived from the AtGames External Applications SDK sample apps
// (https://www.atgames.net/features/external-apps), extended with a second
// face and per-renderer atlases. Included with attribution to AtGames.
// Uses stb_truetype (public domain / MIT, src/vendor/stb_truetype.h).

#include "AppFont.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

#include <cmath>
#include <vector>

namespace AppFont {
namespace {

// Both TTFs are objcopy'd directly into the ELF by CMake (see CMakeLists.txt).
// No font file or font library is needed at runtime.
extern "C" {
extern const unsigned char _binary_NotoSans_Regular_ttf_start[];
extern const unsigned char _binary_BebasNeue_Regular_ttf_start[];
}

constexpr int kFirstCharacter = 32;
constexpr int kCharacterCount = 96;

// Bake each face near the size it is actually drawn at, so glyphs render close
// to 1:1 on screen instead of being minified ~2-3x (which aliases and made the
// small text look wavy/uneven — linear filtering alone can't fix that much
// minification without mipmaps). Body (Noto) is only used for small text
// (hints/lists, ~50-90px on the 3x canvas), so it bakes small; Display (Bebas)
// is used for large headers/titles, so it bakes large.
constexpr float kBodyBake = 72.0f;
constexpr float kDisplayBake = 160.0f;
constexpr int kAtlasSize = 2048;

struct Font {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    stbtt_packedchar characters[kCharacterCount] = {};
    bool loaded = false;
};

// Index by (int)Face: 0 = Body (Noto Sans), 1 = Display (Bebas Neue).
Font g_fonts[2];

const unsigned char* faceData(Face face)
{
    return (face == Face::Display)
        ? _binary_BebasNeue_Regular_ttf_start
        : _binary_NotoSans_Regular_ttf_start;
}

float bakeSize(Face face)
{
    return (face == Face::Display) ? kDisplayBake : kBodyBake;
}

Font& ensureLoaded(SDL_Renderer* renderer, Face face)
{
    Font& font = g_fonts[static_cast<int>(face)];
    if (font.loaded && font.renderer == renderer) {
        return font;
    }

    if (font.texture) {
        SDL_DestroyTexture(font.texture);
        font.texture = nullptr;
    }
    font.loaded = false;

    // Pack the glyphs with padding between them (stbtt_PackFontRange, rather
    // than the simpler BakeFontBitmap). The padding is what lets the atlas be
    // sampled with LINEAR filtering without a glyph bleeding pixels from its
    // neighbor — and linear filtering is what keeps small text smooth when SDL
    // minifies the 160px atlas glyphs down to ~40-60px on screen (nearest-
    // neighbor minification is what made the body text look uneven).
    std::vector<unsigned char> alpha(static_cast<std::size_t>(kAtlasSize) * kAtlasSize);
    stbtt_pack_context pack;
    if (!stbtt_PackBegin(&pack, alpha.data(), kAtlasSize, kAtlasSize, 0, 2, nullptr)) {
        return font;
    }
    // Oversample the small Body atlas for extra sharpness; the Display atlas is
    // already high-resolution so 1x keeps it inside the 2048 atlas.
    const int oversample = (face == Face::Display) ? 1 : 2;
    stbtt_PackSetOversampling(&pack, oversample, oversample);
    const int packed = stbtt_PackFontRange(
        &pack, faceData(face), 0, bakeSize(face),
        kFirstCharacter, kCharacterCount, font.characters);
    stbtt_PackEnd(&pack);
    if (!packed) {
        return font;
    }

    std::vector<Uint32> pixels(alpha.size());
    for (std::size_t index = 0; index < alpha.size(); ++index) {
        pixels[index] = 0xffffff00u | alpha[index];
    }

    font.texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STATIC, kAtlasSize, kAtlasSize
    );
    if (!font.texture) {
        return font;
    }

    SDL_UpdateTexture(font.texture, nullptr, pixels.data(), kAtlasSize * sizeof(Uint32));
    SDL_SetTextureBlendMode(font.texture, SDL_BLENDMODE_BLEND);
    // Linear filtering is applied via the global SDL_HINT_RENDER_SCALE_QUALITY
    // hint set in App::init (SDL 2.0.0), NOT SDL_SetTextureScaleMode — that API
    // is SDL 2.0.12+ and is missing from older firmware SDL2, which makes the
    // ELF fail to load on the device. The padded atlas above keeps the global
    // linear filter from bleeding neighboring glyphs.
    font.renderer = renderer;
    font.loaded = true;
    return font;
}

} // namespace

float measureWidth(SDL_Renderer* renderer, const std::string& text, float size, Face face)
{
    const Font& font = ensureLoaded(renderer, face);
    if (!font.loaded || text.empty()) {
        return 0.0f;
    }

    const float scale = size / bakeSize(face);
    float width = 0.0f;
    for (unsigned char character : text) {
        if (character >= kFirstCharacter && character < kFirstCharacter + kCharacterCount) {
            width += font.characters[character - kFirstCharacter].xadvance * scale;
        }
    }
    return width;
}

void draw(SDL_Renderer* renderer, const std::string& text, float x, float y,
          float size, SDL_Color color, Face face)
{
    const Font& font = ensureLoaded(renderer, face);
    if (!font.loaded || text.empty()) {
        return;
    }

    // Place glyphs on PHYSICAL pixels. The canvas is drawn with a render scale
    // (3x on a 4K panel), so rounding glyph rects in logical units snapped
    // every edge to 3-pixel steps: narrow glyphs like 'i' and 'l' got squeezed
    // or stretched by up to a third of a logical pixel and their stems blurred,
    // reading visibly lighter than their neighbors. Drop to scale 1 for the
    // glyph copies and round in physical space instead, carrying the clip rect
    // (stored in logical units) across the change.
    float rsx = 1.0f, rsy = 1.0f;
    SDL_RenderGetScale(renderer, &rsx, &rsy);
    SDL_Rect clip{0, 0, 0, 0};
    SDL_RenderGetClipRect(renderer, &clip);
    const bool scaled = (rsx != 1.0f || rsy != 1.0f);
    if (scaled) {
        SDL_RenderSetScale(renderer, 1.0f, 1.0f);
        if (clip.w > 0 && clip.h > 0) {
            const SDL_Rect pc{static_cast<int>(std::lround(clip.x * rsx)), static_cast<int>(std::lround(clip.y * rsy)),
                              static_cast<int>(std::lround(clip.w * rsx)), static_cast<int>(std::lround(clip.h * rsy))};
            SDL_RenderSetClipRect(renderer, &pc);
        }
    }

    const float scale = size / bakeSize(face);
    const float baseline = y + size;
    SDL_SetTextureColorMod(font.texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(font.texture, color.a);

    float penX = x;
    for (unsigned char character : text) {
        if (character < kFirstCharacter || character >= kFirstCharacter + kCharacterCount) {
            continue;
        }

        float bakedX = penX / scale;
        float bakedY = baseline / scale;
        stbtt_aligned_quad quad;
        stbtt_GetPackedQuad(
            font.characters, kAtlasSize, kAtlasSize,
            character - kFirstCharacter, &bakedX, &bakedY, &quad, 1
        );
        penX = bakedX * scale;

        const SDL_Rect source {
            static_cast<int>(quad.s0 * kAtlasSize),
            static_cast<int>(quad.t0 * kAtlasSize),
            static_cast<int>((quad.s1 - quad.s0) * kAtlasSize),
            static_cast<int>((quad.t1 - quad.t0) * kAtlasSize)
        };
        // Round each glyph to whole pixels (rather than truncating) so glyphs
        // sit on a consistent baseline instead of jittering up/down by a pixel.
        const int dx0 = static_cast<int>(std::lround(quad.x0 * scale * rsx));
        const int dy0 = static_cast<int>(std::lround(quad.y0 * scale * rsy));
        const int dx1 = static_cast<int>(std::lround(quad.x1 * scale * rsx));
        const int dy1 = static_cast<int>(std::lround(quad.y1 * scale * rsy));
        const SDL_Rect destination { dx0, dy0, dx1 - dx0, dy1 - dy0 };
        SDL_RenderCopy(renderer, font.texture, &source, &destination);
    }

    if (scaled) {
        SDL_RenderSetScale(renderer, rsx, rsy);
        if (clip.w > 0 && clip.h > 0) {
            SDL_RenderSetClipRect(renderer, &clip);
        }
    }
}

void drawCentered(SDL_Renderer* renderer, const std::string& text, float center_x,
                  float y, float size, SDL_Color color, Face face)
{
    draw(renderer, text, center_x - measureWidth(renderer, text, size, face) * 0.5f,
         y, size, color, face);
}

void drawRight(SDL_Renderer* renderer, const std::string& text, float right_x,
               float y, float size, SDL_Color color, Face face)
{
    draw(renderer, text, right_x - measureWidth(renderer, text, size, face),
         y, size, color, face);
}

} // namespace AppFont
