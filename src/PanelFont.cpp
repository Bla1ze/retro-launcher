#include "PanelFont.h"

// Implementation of stb_truetype is compiled in AppFont.cpp; we only consume it.
// The include resolves via the sdk/leaderboard/vendor include dir (see CMake).
#include "vendor/stb_truetype.h"

#include <cmath>
#include <vector>

namespace PanelFont {
namespace {

extern "C" {
extern const unsigned char _binary_NotoSans_Regular_ttf_start[];
extern const unsigned char _binary_BebasNeue_Regular_ttf_start[];
}

// One stbtt_fontinfo per face, initialized on first use from the embedded bytes.
const stbtt_fontinfo& font(Face face)
{
    static stbtt_fontinfo body;
    static stbtt_fontinfo disp;
    static bool bodyInit = false;
    static bool dispInit = false;
    if (face == Face::Display) {
        if (!dispInit) {
            stbtt_InitFont(&disp, _binary_BebasNeue_Regular_ttf_start, 0);
            dispInit = true;
        }
        return disp;
    }
    if (!bodyInit) {
        stbtt_InitFont(&body, _binary_NotoSans_Regular_ttf_start, 0);
        bodyInit = true;
    }
    return body;
}

} // namespace

float measure(const std::string& text, float pxHeight, Face face)
{
    const stbtt_fontinfo& f = font(face);
    const float scale = stbtt_ScaleForPixelHeight(&f, pxHeight);
    float x = 0.0f;
    for (std::size_t i = 0; i < text.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f, static_cast<unsigned char>(text[i]), &adv, &lsb);
        x += adv * scale;
        if (i + 1 < text.size()) {
            x += stbtt_GetCodepointKernAdvance(&f, static_cast<unsigned char>(text[i]),
                                               static_cast<unsigned char>(text[i + 1])) * scale;
        }
    }
    return x;
}

void draw(uint8_t* canvas, int W, int H, const std::string& text,
          float x0, float y0, float pxHeight, uint8_t r, uint8_t g, uint8_t b,
          Face face, int clipX0, int clipX1)
{
    if (!canvas || text.empty()) {
        return;
    }
    if (clipX0 < 0) clipX0 = 0;
    if (clipX1 > W) clipX1 = W;

    const stbtt_fontinfo& f = font(face);
    const float scale = stbtt_ScaleForPixelHeight(&f, pxHeight);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&f, &ascent, &descent, &lineGap);
    const float baseline = y0 + ascent * scale; // y0 is the top of the em box

    float pen = x0;
    std::vector<unsigned char> glyph;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f, ch, &adv, &lsb);

        int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        stbtt_GetCodepointBitmapBox(&f, ch, scale, scale, &gx0, &gy0, &gx1, &gy1);
        const int gw = gx1 - gx0;
        const int gh = gy1 - gy0;
        if (gw > 0 && gh > 0) {
            glyph.assign(static_cast<std::size_t>(gw) * gh, 0);
            stbtt_MakeCodepointBitmap(&f, glyph.data(), gw, gh, gw, scale, scale, ch);
            const int dstX = static_cast<int>(std::floor(pen)) + gx0;
            const int dstY = static_cast<int>(std::floor(baseline)) + gy0;
            for (int yy = 0; yy < gh; ++yy) {
                const int dy = dstY + yy;
                if (dy < 0 || dy >= H) continue;
                const unsigned char* srow = &glyph[static_cast<std::size_t>(yy) * gw];
                uint8_t* drow = canvas + (static_cast<std::size_t>(dy) * W) * 4;
                for (int xx = 0; xx < gw; ++xx) {
                    const int dx = dstX + xx;
                    if (dx < clipX0 || dx >= clipX1) continue;
                    const unsigned a = srow[xx];
                    if (!a) continue;
                    uint8_t* p = drow + static_cast<std::size_t>(dx) * 4;
                    p[0] = static_cast<uint8_t>((r * a + p[0] * (255 - a)) / 255);
                    p[1] = static_cast<uint8_t>((g * a + p[1] * (255 - a)) / 255);
                    p[2] = static_cast<uint8_t>((b * a + p[2] * (255 - a)) / 255);
                    p[3] = 255;
                }
            }
        }

        pen += adv * scale;
        if (i + 1 < text.size()) {
            pen += stbtt_GetCodepointKernAdvance(&f, ch,
                       static_cast<unsigned char>(text[i + 1])) * scale;
        }
    }
}

} // namespace PanelFont
