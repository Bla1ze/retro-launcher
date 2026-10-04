#include "Gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <list>
#include <vector>

namespace Gfx {
namespace {

SDL_Renderer* g_renderer = nullptr;
float g_bake = 2.0f; // texels per logical px for size-baked shapes

// Shared, size-independent masks (white RGB, coverage in alpha).
SDL_Texture* g_disc = nullptr;
SDL_Texture* g_capsule = nullptr;
SDL_Texture* g_bar = nullptr;       // straight bar, soft long edges (line bodies)
SDL_Texture* g_triangle = nullptr;
SDL_Texture* g_square = nullptr;
SDL_Texture* g_glow = nullptr;
SDL_Texture* g_vramp = nullptr;     // 1x256, alpha 255 -> 0 top to bottom
SDL_Texture* g_hramp = nullptr;     // 256x1, alpha 255 -> 0 left to right
SDL_Texture* g_vinyl = nullptr;     // full-color record body
SDL_Texture* g_label = nullptr;     // two-tone label mask (tinted per draw)

constexpr int kDisc = 256;
constexpr int kTri = 128;
constexpr int kGlow = 128;
constexpr int kVinyl = 768;

// Size-baked shapes, least-recently-used eviction.
struct Cached {
    std::string key;
    SDL_Texture* tex = nullptr;
};
std::list<Cached> g_cache;
constexpr std::size_t kCacheMax = 64;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

SDL_Texture* upload(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!g_renderer || w <= 0 || h <= 0) {
        return nullptr;
    }
    // ABGR8888 is R,G,B,A byte order on little-endian — the same static-texture
    // path AlbumArt uses, known to work on the firmware SDL.
    SDL_Texture* t = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) {
        return nullptr;
    }
    SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

// Build a white mask texture from a coverage function sampled at pixel centers.
template <typename F>
SDL_Texture* bakeMask(int w, int h, F coverage)
{
    std::vector<uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(clamp01(coverage(x + 0.5f, y + 0.5f)) * 255.0f + 0.5f);
        }
    }
    return upload(px, w, h);
}

// Signed distance from (px, py) to a rounded rect [0,w]x[0,h] with radius rr
// (negative inside).
inline float roundRectSd(float px, float py, float w, float h, float rr)
{
    const float qx = std::fabs(px - w * 0.5f) - (w * 0.5f - rr);
    const float qy = std::fabs(py - h * 0.5f) - (h * 0.5f - rr);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rr;
}

SDL_Texture* cacheFind(const std::string& key)
{
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
        if (it->key == key) {
            g_cache.splice(g_cache.begin(), g_cache, it); // mark most recent
            return g_cache.front().tex;
        }
    }
    return nullptr;
}

void cachePut(const std::string& key, SDL_Texture* tex)
{
    g_cache.push_front(Cached{key, tex});
    while (g_cache.size() > kCacheMax) {
        if (g_cache.back().tex) {
            SDL_DestroyTexture(g_cache.back().tex);
        }
        g_cache.pop_back();
    }
}

std::string colorKey(SDL_Color c)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return b;
}

SDL_Rect toRect(const FRect& rc)
{
    const int x0 = static_cast<int>(std::lround(rc.x));
    const int y0 = static_cast<int>(std::lround(rc.y));
    const int x1 = static_cast<int>(std::lround(rc.x + rc.w));
    const int y1 = static_cast<int>(std::lround(rc.y + rc.h));
    return SDL_Rect{x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

void draw(SDL_Texture* t, const FRect& dst, SDL_Color c, SDL_BlendMode mode = SDL_BLENDMODE_BLEND,
          double angle = 0.0, const SDL_Rect* src = nullptr)
{
    if (!t || !g_renderer || c.a == 0) {
        return;
    }
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_SetTextureBlendMode(t, mode);
    const SDL_Rect d = toRect(dst);
    if (angle == 0.0) {
        SDL_RenderCopy(g_renderer, t, src, &d);
    } else {
        SDL_RenderCopyEx(g_renderer, t, src, &d, angle, nullptr, SDL_FLIP_NONE);
    }
}

// Separable box blur on a single-channel buffer, repeated for a near-gaussian.
void boxBlur(std::vector<float>& a, int w, int h, int radius, int passes)
{
    if (radius <= 0) {
        return;
    }
    std::vector<float> tmp(a.size());
    for (int pass = 0; pass < passes; ++pass) {
        for (int y = 0; y < h; ++y) {
            float acc = 0.0f;
            const float* row = &a[static_cast<std::size_t>(y) * w];
            for (int x = -radius; x <= radius; ++x) acc += row[std::min(std::max(x, 0), w - 1)];
            for (int x = 0; x < w; ++x) {
                tmp[static_cast<std::size_t>(y) * w + x] = acc / (2 * radius + 1);
                acc += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)];
            }
        }
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int y = -radius; y <= radius; ++y) acc += tmp[static_cast<std::size_t>(std::min(std::max(y, 0), h - 1)) * w + x];
            for (int y = 0; y < h; ++y) {
                a[static_cast<std::size_t>(y) * w + x] = acc / (2 * radius + 1);
                acc += tmp[static_cast<std::size_t>(std::min(y + radius + 1, h - 1)) * w + x] -
                       tmp[static_cast<std::size_t>(std::max(y - radius, 0)) * w + x];
            }
        }
    }
}

void bakeVinyl()
{
    const int N = kVinyl;
    const float R = N * 0.5f - 2.0f;
    const float c = N * 0.5f;
    std::vector<uint8_t> px(static_cast<std::size_t>(N) * N * 4);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float dx = x + 0.5f - c;
            const float dy = y + 0.5f - c;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float cov = clamp01(R - d + 0.5f);
            uint8_t* p = &px[(static_cast<std::size_t>(y) * N + x) * 4];
            // Base body with fine grooves (a radial ripple) and a slightly
            // lighter run-out band near the label.
            const float rn = d / R;
            float lum = 16.0f + 7.0f * (0.5f + 0.5f * std::sin(d * 1.9f));
            if (rn < 0.42f) lum = 26.0f;
            if (rn > 0.965f) lum = 30.0f; // raised lip
            // Two opposing sheen wedges — they turn with the record, which is
            // what makes the rotation legible.
            const float ang = std::atan2(dy, dx);
            const float s1 = std::pow(std::max(0.0f, std::cos(ang - 0.7f)), 18.0f);
            const float s2 = std::pow(std::max(0.0f, std::cos(ang - 0.7f - 3.14159265f)), 18.0f);
            lum += (s1 * 46.0f + s2 * 30.0f) * (rn > 0.42f ? 1.0f : 0.0f);
            const uint8_t L = static_cast<uint8_t>(std::min(255.0f, lum));
            p[0] = L;
            p[1] = static_cast<uint8_t>(std::min(255.0f, lum * 1.04f));
            p[2] = static_cast<uint8_t>(std::min(255.0f, lum * 1.18f));
            p[3] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
        }
    }
    g_vinyl = upload(px, N, N);

    // Label: a disc split into a bright half and a dimmer half (tinted by the
    // caller), with the spindle hole punched out.
    const int L = 256;
    const float lc = L * 0.5f;
    const float lr = L * 0.5f - 2.0f;
    std::vector<uint8_t> lp(static_cast<std::size_t>(L) * L * 4);
    for (int y = 0; y < L; ++y) {
        for (int x = 0; x < L; ++x) {
            const float dx = x + 0.5f - lc;
            const float dy = y + 0.5f - lc;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float cov = clamp01(lr - d + 0.5f) * clamp01(d - L * 0.045f + 0.5f);
            uint8_t* p = &lp[(static_cast<std::size_t>(y) * L + x) * 4];
            const uint8_t v = (dy < 0.0f) ? 255 : 170;
            const bool rim = d > lr - 7.0f;
            p[0] = p[1] = p[2] = rim ? 120 : v;
            p[3] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
        }
    }
    g_label = upload(lp, L, L);
}

} // namespace

SDL_Color mix(SDL_Color a, SDL_Color b, float t)
{
    t = clamp01(t);
    return {
        static_cast<Uint8>(a.r + (b.r - a.r) * t),
        static_cast<Uint8>(a.g + (b.g - a.g) * t),
        static_cast<Uint8>(a.b + (b.b - a.b) * t),
        static_cast<Uint8>(a.a + (b.a - a.a) * t)
    };
}

SDL_Color withAlpha(SDL_Color c, Uint8 a)
{
    c.a = a;
    return c;
}

void init(SDL_Renderer* renderer, float scale)
{
    shutdown();
    g_renderer = renderer;
    g_bake = std::max(1.0f, std::min(scale, 2.0f));

    const float dc = kDisc * 0.5f;
    const float dr = kDisc * 0.5f - 1.5f;
    g_disc = bakeMask(kDisc, kDisc, [&](float x, float y) {
        const float d = std::sqrt((x - dc) * (x - dc) + (y - dc) * (y - dc));
        return dr - d + 0.5f;
    });

    // Capsule: 256x64, fully rounded ends.
    g_capsule = bakeMask(256, 64, [](float x, float y) {
        return 0.5f - roundRectSd(x, y, 256.0f, 64.0f, 31.0f);
    });

    // Straight bar: opaque along x, 1-texel soft edge across y (16 rows).
    g_bar = bakeMask(4, 16, [](float, float y) {
        return std::min(y, 16.0f - y) - 0.5f;
    });

    // Triangle pointing right, supersampled 4x4 for smooth edges.
    g_triangle = bakeMask(kTri, kTri, [](float x, float y) {
        const float pad = 6.0f;
        const float ax = pad, ay = pad, bx = pad, by = kTri - pad, cx = kTri - pad, cy = kTri * 0.5f;
        int inside = 0;
        for (int sy = 0; sy < 4; ++sy) {
            for (int sx = 0; sx < 4; ++sx) {
                const float px = x - 0.5f + (sx + 0.5f) / 4.0f;
                const float py = y - 0.5f + (sy + 0.5f) / 4.0f;
                const float e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                const float e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
                const float e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
                if ((e0 <= 0 && e1 <= 0 && e2 <= 0) || (e0 >= 0 && e1 >= 0 && e2 >= 0)) ++inside;
            }
        }
        return inside / 16.0f;
    });

    // Soft-edged square (transparent margin) so rotated copies stay smooth.
    g_square = bakeMask(64, 64, [](float x, float y) {
        return std::min(std::min(x, 64.0f - x), std::min(y, 64.0f - y)) - 2.0f;
    });

    const float gc = kGlow * 0.5f;
    g_glow = bakeMask(kGlow, kGlow, [&](float x, float y) {
        const float d = std::sqrt((x - gc) * (x - gc) + (y - gc) * (y - gc)) / gc;
        return d >= 1.0f ? 0.0f : std::exp(-d * d * 4.0f) * (1.0f - d);
    });

    g_vramp = bakeMask(1, 256, [](float, float y) { return 1.0f - y / 256.0f; });
    g_hramp = bakeMask(256, 1, [](float x, float) { return 1.0f - x / 256.0f; });

    bakeVinyl();
}

void shutdown()
{
    SDL_Texture** all[] = {&g_disc, &g_capsule, &g_bar, &g_triangle, &g_square,
                           &g_glow, &g_vramp, &g_hramp, &g_vinyl, &g_label};
    for (SDL_Texture** t : all) {
        if (*t) {
            SDL_DestroyTexture(*t);
            *t = nullptr;
        }
    }
    for (Cached& c : g_cache) {
        if (c.tex) SDL_DestroyTexture(c.tex);
    }
    g_cache.clear();
    g_renderer = nullptr;
}

void rect(SDL_Renderer* r, const FRect& rc, SDL_Color c)
{
    if (c.a == 0) return;
    // Round both edges and skip anything that collapses to nothing. (toRect
    // clamps to 1px for textured quads; for fills that clamp drew a stray 1px
    // strip through pill shapes whose middle band has zero height.)
    const int x0 = static_cast<int>(std::lround(rc.x));
    const int y0 = static_cast<int>(std::lround(rc.y));
    const int x1 = static_cast<int>(std::lround(rc.x + rc.w));
    const int y1 = static_cast<int>(std::lround(rc.y + rc.h));
    if (x1 <= x0 || y1 <= y0) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    const SDL_Rect d{x0, y0, x1 - x0, y1 - y0};
    SDL_RenderFillRect(r, &d);
}

void disc(SDL_Renderer*, float cx, float cy, float radius, SDL_Color c)
{
    draw(g_disc, {cx - radius, cy - radius, radius * 2.0f, radius * 2.0f}, c);
}

void ring(SDL_Renderer*, float cx, float cy, float radius, float stroke, SDL_Color c)
{
    const int R = static_cast<int>(std::lround(radius * 2.0f));
    const int S = static_cast<int>(std::lround(stroke * 4.0f));
    const std::string key = "ring:" + std::to_string(R) + ":" + std::to_string(S);
    SDL_Texture* t = cacheFind(key);
    const float ext = radius + stroke;
    if (!t) {
        const int N = std::max(8, static_cast<int>(std::ceil(ext * 2.0f * g_bake)) + 4);
        const float cc = N * 0.5f;
        const float rr = radius * g_bake;
        const float hs = stroke * g_bake * 0.5f;
        t = bakeMask(N, N, [&](float x, float y) {
            const float d = std::sqrt((x - cc) * (x - cc) + (y - cc) * (y - cc));
            return hs + 0.5f - std::fabs(d - rr);
        });
        cachePut(key, t);
    }
    int tw = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, nullptr);
    const float half = tw / g_bake * 0.5f;
    draw(t, {cx - half, cy - half, half * 2.0f, half * 2.0f}, c);
}

void line(SDL_Renderer* r, float x0, float y0, float x1, float y1, float thick, SDL_Color c)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float hw = thick * 0.5f;
    if (len > 0.5f) {
        const float mx = (x0 + x1) * 0.5f;
        const float my = (y0 + y1) * 0.5f;
        const float h = thick * 16.0f / 14.0f; // bar texture has 1-texel soft rows
        const double deg = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
        draw(g_bar, {mx - len * 0.5f, my - h * 0.5f, len, h}, c, SDL_BLENDMODE_BLEND, deg);
    }
    disc(r, x0, y0, hw, c);
    disc(r, x1, y1, hw, c);
}

void roundRect(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color c)
{
    if (rc.w <= 0.0f || rc.h <= 0.0f) return;
    const float rr = std::min(radius, std::min(rc.w, rc.h) * 0.5f);
    if (rr < 0.75f) {
        rect(r, rc, c);
        return;
    }
    // Four corner quadrants of the disc mask + three body rects. The body
    // rects never overlap the corners, so translucent colors stay even.
    const int q = kDisc / 2;
    const SDL_Rect tl{0, 0, q, q}, tr{q, 0, q, q}, bl{0, q, q, q}, br{q, q, q, q};
    draw(g_disc, {rc.x, rc.y, rr, rr}, c, SDL_BLENDMODE_BLEND, 0.0, &tl);
    draw(g_disc, {rc.x + rc.w - rr, rc.y, rr, rr}, c, SDL_BLENDMODE_BLEND, 0.0, &tr);
    draw(g_disc, {rc.x, rc.y + rc.h - rr, rr, rr}, c, SDL_BLENDMODE_BLEND, 0.0, &bl);
    draw(g_disc, {rc.x + rc.w - rr, rc.y + rc.h - rr, rr, rr}, c, SDL_BLENDMODE_BLEND, 0.0, &br);
    rect(r, {rc.x + rr, rc.y, rc.w - 2.0f * rr, rr}, c);
    rect(r, {rc.x, rc.y + rr, rc.w, rc.h - 2.0f * rr}, c);
    rect(r, {rc.x + rr, rc.y + rc.h - rr, rc.w - 2.0f * rr, rr}, c);
}

void panel(SDL_Renderer*, const FRect& rc, float radius, SDL_Color top, SDL_Color bottom,
           SDL_Color border, float borderWidth)
{
    const int W = static_cast<int>(std::lround(rc.w));
    const int H = static_cast<int>(std::lround(rc.h));
    if (W <= 0 || H <= 0) return;
    const std::string key = "panel:" + std::to_string(W) + "x" + std::to_string(H) + ":" +
        std::to_string(static_cast<int>(radius)) + ":" + colorKey(top) + colorKey(bottom) +
        colorKey(border) + ":" + std::to_string(static_cast<int>(borderWidth * 4.0f));
    SDL_Texture* t = cacheFind(key);
    if (!t) {
        const int bw = static_cast<int>(std::ceil(W * g_bake));
        const int bh = static_cast<int>(std::ceil(H * g_bake));
        const float rr = std::min(radius, std::min(rc.w, rc.h) * 0.5f) * g_bake;
        const float bwid = borderWidth * g_bake;
        std::vector<uint8_t> px(static_cast<std::size_t>(bw) * bh * 4);
        for (int y = 0; y < bh; ++y) {
            const SDL_Color fill = mix(top, bottom, (y + 0.5f) / bh);
            for (int x = 0; x < bw; ++x) {
                const float sd = roundRectSd(x + 0.5f, y + 0.5f, static_cast<float>(bw),
                                             static_cast<float>(bh), rr);
                const float cov = clamp01(0.5f - sd);
                float fr = fill.r, fg = fill.g, fb = fill.b, fa = fill.a / 255.0f;
                if (bwid > 0.0f && border.a > 0) {
                    const float tb = clamp01(sd + bwid + 0.5f) * (border.a / 255.0f);
                    const float oa = tb + fa * (1.0f - tb);
                    if (oa > 0.0f) {
                        fr = (border.r * tb + fr * fa * (1.0f - tb)) / oa;
                        fg = (border.g * tb + fg * fa * (1.0f - tb)) / oa;
                        fb = (border.b * tb + fb * fa * (1.0f - tb)) / oa;
                    }
                    fa = oa;
                }
                uint8_t* p = &px[(static_cast<std::size_t>(y) * bw + x) * 4];
                p[0] = static_cast<uint8_t>(fr);
                p[1] = static_cast<uint8_t>(fg);
                p[2] = static_cast<uint8_t>(fb);
                p[3] = static_cast<uint8_t>(clamp01(fa * cov) * 255.0f + 0.5f);
            }
        }
        t = upload(px, bw, bh);
        cachePut(key, t);
    }
    draw(t, rc, {255, 255, 255, 255});
}

void softRect(SDL_Renderer*, const FRect& rc, float radius, float spread, SDL_Color c, bool additive)
{
    const int W = static_cast<int>(std::lround(rc.w));
    const int H = static_cast<int>(std::lround(rc.h));
    if (W <= 0 || H <= 0) return;
    const std::string key = "soft:" + std::to_string(W) + "x" + std::to_string(H) + ":" +
        std::to_string(static_cast<int>(radius)) + ":" + std::to_string(static_cast<int>(spread));
    SDL_Texture* t = cacheFind(key);
    // Baked at half logical resolution: it is blurred anyway.
    constexpr float f = 0.5f;
    const float pad = spread * 1.5f;
    if (!t) {
        const int bw = static_cast<int>(std::ceil((W + pad * 2.0f) * f));
        const int bh = static_cast<int>(std::ceil((H + pad * 2.0f) * f));
        std::vector<float> a(static_cast<std::size_t>(bw) * bh);
        const float rr = std::min(radius, std::min(rc.w, rc.h) * 0.5f) * f;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                const float sd = roundRectSd(x + 0.5f - pad * f, y + 0.5f - pad * f, W * f, H * f, rr);
                a[static_cast<std::size_t>(y) * bw + x] = clamp01(0.5f - sd);
            }
        }
        boxBlur(a, bw, bh, std::max(1, static_cast<int>(spread * f / 2.0f)), 3);
        std::vector<uint8_t> px(a.size() * 4);
        for (std::size_t i = 0; i < a.size(); ++i) {
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 255;
            px[i * 4 + 3] = static_cast<uint8_t>(clamp01(a[i]) * 255.0f + 0.5f);
        }
        t = upload(px, bw, bh);
        cachePut(key, t);
    }
    draw(t, {rc.x - pad, rc.y - pad, rc.w + pad * 2.0f, rc.h + pad * 2.0f}, c,
         additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
}

void glow(SDL_Renderer*, float cx, float cy, float radius, SDL_Color c, bool additive)
{
    draw(g_glow, {cx - radius, cy - radius, radius * 2.0f, radius * 2.0f}, c,
         additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
}

void triangle(SDL_Renderer*, const FRect& box, double degrees, SDL_Color c)
{
    draw(g_triangle, box, c, SDL_BLENDMODE_BLEND, degrees);
}

void heart(SDL_Renderer* r, float cx, float cy, float size, SDL_Color c)
{
    // Two lobes + a square turned 45°, all soft-edged. Drawn opaque-first so a
    // translucent color doesn't show overlaps: callers pass opaque colors.
    const float lobe = size * 0.29f;
    const float sq = size * 0.50f;
    draw(g_square, {cx - sq * 0.5f, cy - sq * 0.5f + size * 0.06f, sq, sq}, c, SDL_BLENDMODE_BLEND, 45.0);
    disc(r, cx - lobe * 0.78f, cy - size * 0.10f, lobe, c);
    disc(r, cx + lobe * 0.78f, cy - size * 0.10f, lobe, c);
}

void vGradient(SDL_Renderer* r, const FRect& rc, SDL_Color top, SDL_Color bottom)
{
    rect(r, rc, bottom);
    draw(g_vramp, rc, top);
}

void hGradient(SDL_Renderer* r, const FRect& rc, SDL_Color left, SDL_Color right)
{
    rect(r, rc, right);
    draw(g_hramp, rc, left);
}

void vinyl(SDL_Renderer*, float cx, float cy, float radius, double degrees,
           SDL_Color labelA, SDL_Color)
{
    draw(g_vinyl, {cx - radius, cy - radius, radius * 2.0f, radius * 2.0f},
         {255, 255, 255, 255}, SDL_BLENDMODE_BLEND, degrees);
    const float lr = radius * 0.40f;
    draw(g_label, {cx - lr, cy - lr, lr * 2.0f, lr * 2.0f}, labelA, SDL_BLENDMODE_BLEND, degrees);
}

void ray(SDL_Renderer*, float cx, float cy, float inner, float length, float thick,
         double degrees, SDL_Color c)
{
    if (length < thick) length = thick;
    const double rad = degrees * 3.14159265358979 / 180.0;
    const float mid = inner + length * 0.5f;
    const float mx = cx + static_cast<float>(std::cos(rad)) * mid;
    const float my = cy + static_cast<float>(std::sin(rad)) * mid;
    draw(g_capsule, {mx - length * 0.5f, my - thick * 0.5f, length, thick}, c,
         SDL_BLENDMODE_BLEND, degrees);
}

} // namespace Gfx
