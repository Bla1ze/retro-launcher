#include "GamePanels.h"

#include "DrmKms.h"
#include "Library.h"
#include "PanelFont.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "vendor/stb_image.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <list>
#include <map>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Library::log;

constexpr uint8_t kInk[3] = {245, 246, 250};
constexpr uint8_t kGold[3] = {245, 214, 95};
constexpr uint8_t kTeal[3] = {43, 199, 184};
constexpr uint8_t kViolet[3] = {124, 92, 232};
constexpr uint8_t kWhite[3] = {255, 255, 255};
constexpr uint8_t kGradTop[3] = {16, 30, 46};
constexpr uint8_t kGradBot[3] = {30, 16, 50};

int findCard0Fd() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int best = -1;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[64], target[256];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        ssize_t n = ::readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (std::strstr(target, "/dev/dri/card")) { best = std::atoi(e->d_name); break; }
    }
    ::closedir(d);
    return best;
}

void gradient(std::vector<uint8_t>& c, int W, int H, const uint8_t top[3], const uint8_t bot[3]) {
    for (int y = 0; y < H; ++y) {
        float t = H > 1 ? (float)y / (H - 1) : 0.0f;
        uint8_t r = (uint8_t)(top[0] + (bot[0] - top[0]) * t), g = (uint8_t)(top[1] + (bot[1] - top[1]) * t),
                b = (uint8_t)(top[2] + (bot[2] - top[2]) * t);
        uint8_t* row = &c[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x) { row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255; }
    }
}

// A soft radial pool of light (additive), for the no-art title card.
void glow(std::vector<uint8_t>& c, int W, int H, float cx, float cy, float radius, const uint8_t col[3], float strength) {
    int x0 = std::max(0, (int)(cx - radius)), x1 = std::min(W, (int)(cx + radius));
    int y0 = std::max(0, (int)(cy - radius)), y1 = std::min(H, (int)(cy + radius));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
            if (d >= 1.0f) continue;
            float a = (1.0f - d) * (1.0f - d) * strength;
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::min(255.0f, p[k] + col[k] * a);
        }
}

void text(std::vector<uint8_t>& c, int W, int H, const std::string& s, float cx, float top, float px,
          const uint8_t col[3], PanelFont::Face face, float maxW) {
    if (s.empty()) return;
    float w = PanelFont::measure(s, px, face);
    if (w > maxW && w > 0) { px *= maxW / w; w = maxW; }
    PanelFont::draw(c.data(), W, H, s, cx - w * 0.5f, top, px, col[0], col[1], col[2], face);
}

// Blurred, darkened backdrop: average to a tiny grid, bilinear back up.
void backdrop(std::vector<uint8_t>& c, int W, int H, const std::vector<uint8_t>& img, int iw, int ih) {
    constexpr int SB = 18;
    float small[SB * SB * 3];
    for (int sy = 0; sy < SB; ++sy)
        for (int sx = 0; sx < SB; ++sx) {
            int x0 = sx * iw / SB, x1 = std::max(x0 + 1, (sx + 1) * iw / SB);
            int y0 = sy * ih / SB, y1 = std::max(y0 + 1, (sy + 1) * ih / SB);
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    const uint8_t* s = &img[((size_t)y * iw + x) * 4];
                    r += s[0]; g += s[1]; b += s[2]; ++n;
                }
            float* d = &small[(sy * SB + sx) * 3];
            d[0] = r / n; d[1] = g / n; d[2] = b / n;
        }
    const float dim = 0.36f;
    for (int y = 0; y < H; ++y) {
        float fv = (y + 0.5f) / H * SB - 0.5f;
        int v0 = (int)std::floor(fv);
        float vt = fv - v0;
        int a0 = std::min(std::max(v0, 0), SB - 1), a1 = std::min(std::max(v0 + 1, 0), SB - 1);
        for (int x = 0; x < W; ++x) {
            float fu = (x + 0.5f) / W * SB - 0.5f;
            int u0 = (int)std::floor(fu);
            float ut = fu - u0;
            int b0 = std::min(std::max(u0, 0), SB - 1), b1 = std::min(std::max(u0 + 1, 0), SB - 1);
            uint8_t* o = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) {
                float t = small[(a0 * SB + b0) * 3 + k] * (1 - ut) + small[(a0 * SB + b1) * 3 + k] * ut;
                float bo = small[(a1 * SB + b0) * 3 + k] * (1 - ut) + small[(a1 * SB + b1) * 3 + k] * ut;
                o[k] = (uint8_t)((t * (1 - vt) + bo * vt) * dim);
            }
            o[3] = 255;
        }
    }
}

// Bilinear, aspect-fit blit of an RGBA image into a box, with a soft shadow.
void blitFit(std::vector<uint8_t>& c, int W, int H, const std::vector<uint8_t>& img, int iw, int ih, int bx, int by,
             int bw, int bh) {
    float s = std::min((float)bw / iw, (float)bh / ih);
    int dw = std::max(1, (int)(iw * s)), dh = std::max(1, (int)(ih * s));
    int ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
    // shadow (only for opaque pictures; a cut-out console casts none)
    bool opaque = true;
    for (size_t i = 3; i < img.size() && opaque; i += 4 * 97) opaque = img[i] > 250;
    const int sh = opaque ? std::max(8, H / 60) : 0;
    for (int y = oy - sh; sh > 0 && y < oy + dh + sh * 2; ++y)
        for (int x = ox - sh; x < ox + dw + sh * 2; ++x) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float dx = std::max(0, std::max(ox - x, x - (ox + dw - 1) - sh));
            float dy = std::max(0, std::max(oy - y, y - (oy + dh - 1) - sh));
            float d = std::sqrt(dx * dx + dy * dy) / (sh * 1.5f);
            if (d >= 1.0f) continue;
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            float k = 1.0f - 0.55f * (1.0f - d);
            p[0] = (uint8_t)(p[0] * k); p[1] = (uint8_t)(p[1] * k); p[2] = (uint8_t)(p[2] * k);
        }
    for (int y = 0; y < dh; ++y) {
        int cy = oy + y;
        if (cy < 0 || cy >= H) continue;
        float fv = (y + 0.5f) / dh * ih - 0.5f;
        int v0 = std::min(std::max((int)std::floor(fv), 0), ih - 1), v1 = std::min(v0 + 1, ih - 1);
        float vt = std::max(0.0f, fv - v0);
        for (int x = 0; x < dw; ++x) {
            int cx = ox + x;
            if (cx < 0 || cx >= W) continue;
            float fu = (x + 0.5f) / dw * iw - 0.5f;
            int u0 = std::min(std::max((int)std::floor(fu), 0), iw - 1), u1 = std::min(u0 + 1, iw - 1);
            float ut = std::max(0.0f, fu - u0);
            const uint8_t *s00 = &img[((size_t)v0 * iw + u0) * 4], *s10 = &img[((size_t)v0 * iw + u1) * 4],
                          *s01 = &img[((size_t)v1 * iw + u0) * 4], *s11 = &img[((size_t)v1 * iw + u1) * 4];
            uint8_t* q = &c[((size_t)cy * W + cx) * 4];
            float a = ((s00[3] * (1 - ut) + s10[3] * ut) * (1 - vt) + (s01[3] * (1 - ut) + s11[3] * ut) * vt) / 255.0f;
            for (int k = 0; k < 3; ++k) {
                float v = (s00[k] * (1 - ut) + s10[k] * ut) * (1 - vt) + (s01[k] * (1 - ut) + s11[k] * ut) * vt;
                q[k] = (uint8_t)(v * a + q[k] * (1 - a));
            }
        }
    }
}

// ---- placeholder cover --------------------------------------------------------
// Until real box art exists, each game gets a generated cover in the same layout
// real art uses: colors picked from its name (stable between visits), a system
// banner, the title in the display face, and a faint giant initial behind it.

uint32_t fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ (uint8_t)c) * 16777619u;
    return h;
}

void mixRgb(const uint8_t a[3], const uint8_t b[3], float t, uint8_t out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = (uint8_t)(a[k] + (b[k] - a[k]) * t);
}

// Splits a title into at most `maxLines` lines that fit `maxW` at `px`.
std::vector<std::string> wrap(const std::string& text, float px, float maxW, int maxLines) {
    std::vector<std::string> words, lines;
    std::string w;
    for (char c : text + " ") {
        if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += c;
    }
    std::string line;
    for (const std::string& word : words) {
        std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && PanelFont::measure(trial, px, PanelFont::Face::Display) > maxW) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
    }
    if (!line.empty()) lines.push_back(line);
    if ((int)lines.size() > maxLines) {
        lines.resize(maxLines);
        lines.back() += "...";
    }
    return lines;
}

std::vector<uint8_t> makeCover(const std::string& system, const std::string& title, int& outW, int& outH) {
    const int W = 600, H = 840;
    outW = W; outH = H;
    std::vector<uint8_t> c((size_t)W * H * 4, 255);
    static const uint8_t pal[5][3] = {{43, 199, 184}, {124, 92, 232}, {245, 214, 95}, {239, 92, 120}, {70, 140, 240}};
    uint32_t h = fnv(title);
    const uint8_t* a = pal[h % 5];
    const uint8_t* b = pal[(h / 5 + 1 + h % 4) % 5];
    const uint8_t dark[3] = {10, 12, 22};
    uint8_t top[3], bot[3], band[3];
    mixRgb(a, dark, 0.35f, top);
    mixRgb(b, dark, 0.72f, bot);
    mixRgb(pal[fnv(system) % 5], dark, 0.15f, band);

    // Body: diagonal-ish gradient with soft light streaks.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float t = std::min(1.0f, std::max(0.0f, (y * 0.8f + x * 0.35f) / (H * 0.8f + W * 0.35f)));
            float streak = 0.5f + 0.5f * std::sin((x * 0.9f - y) * 0.018f + (h % 7));
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k)
                p[k] = (uint8_t)std::min(255.0f, top[k] + (bot[k] - top[k]) * t + streak * 10.0f);
            p[3] = 255;
        }
    // Giant faint initial.
    char initial = '#';
    for (char ch : title)
        if (std::isalnum((unsigned char)ch)) { initial = (char)std::toupper((unsigned char)ch); break; }
    std::string ini(1, initial);
    float ipx = H * 0.9f;
    float iw = PanelFont::measure(ini, ipx, PanelFont::Face::Display);
    uint8_t ghost[3];
    mixRgb(top, kWhite, 0.10f, ghost);
    PanelFont::draw(c.data(), W, H, ini, W * 0.62f - iw * 0.5f, H * 0.10f, ipx, ghost[0], ghost[1], ghost[2],
                    PanelFont::Face::Display);

    // System banner.
    const int bandH = 96;
    for (int y = 0; y < bandH; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            float shade = 1.0f - 0.25f * y / bandH;
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)(band[k] * shade);
        }
    for (int x = 0; x < W; ++x)
        for (int y = bandH; y < bandH + 4; ++y) {
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            p[0] = 245; p[1] = 246; p[2] = 250;
        }
    std::string sys = system;
    for (char& ch : sys) ch = (char)std::toupper((unsigned char)ch);
    float spx = 58.0f, sw = PanelFont::measure(sys, spx, PanelFont::Face::Display);
    if (sw > W * 0.86f) { spx *= W * 0.86f / sw; sw = W * 0.86f; }
    PanelFont::draw(c.data(), W, H, sys, (W - sw) * 0.5f, (bandH - spx) * 0.5f - 4.0f, spx, 255, 255, 255,
                    PanelFont::Face::Display);

    // Title, wrapped and centered in the lower body, with a drop shadow.
    float tpx = 112.0f;
    std::vector<std::string> lines = wrap(title, tpx, W * 0.84f, 4);
    while (tpx > 56.0f && (lines.size() > 3 || [&] {
               for (auto& l : lines) if (PanelFont::measure(l, tpx, PanelFont::Face::Display) > W * 0.84f) return true;
               return false;
           }())) {
        tpx -= 8.0f;
        lines = wrap(title, tpx, W * 0.84f, 4);
    }
    float lineH = tpx * 1.02f;
    float y0 = H * 0.70f - lines.size() * lineH * 0.5f;
    for (size_t i = 0; i < lines.size(); ++i) {
        float lw = PanelFont::measure(lines[i], tpx, PanelFont::Face::Display);
        float x = (W - lw) * 0.5f, y = y0 + i * lineH;
        PanelFont::draw(c.data(), W, H, lines[i], x + 5, y + 6, tpx, 0, 0, 0, PanelFont::Face::Display);
        PanelFont::draw(c.data(), W, H, lines[i], x, y, tpx, 255, 255, 255, PanelFont::Face::Display);
    }

    // Thin light edge.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (x < 3 || y < 3 || x >= W - 3 || y >= H - 3) {
                uint8_t* p = &c[((size_t)y * W + x) * 4];
                p[0] = 230; p[1] = 234; p[2] = 242;
            }
    return c;
}

bool isFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

std::string findConsoleArt(const std::string& appDir, const std::string& system) {
    for (const char* ext : {".png", ".jpg"}) {
        std::string p = appDir + "/media/" + system + "/console" + ext;
        if (isFile(p)) return p;
    }
    return "";
}

namespace {

// Product photos usually sit on white. For an image with no transparency, make
// the background transparent:
//   1. flood-fill near-white pixels connected to the border;
//   2. also clear enclosed pockets (inside a cable loop, between a controller
//      and the console) when they are very white and not tiny, so light gray
//      plastic survives;
//   2b. turn soft drop shadows into transparent black (see below);
//   3. soften a 3-pixel band around the result: alpha from how close a pixel is
//      to white, with the white that bled into its color removed, so there is
//      no pale halo on a dark panel.
void keyOutWhite(std::vector<uint8_t>& img, int w, int h) {
    for (size_t i = 3; i < img.size(); i += 4)
        if (img[i] < 250) return;  // already has transparency
    const size_t N = (size_t)w * h;
    auto minc = [&](size_t i) { const uint8_t* p = &img[i * 4]; return (int)std::min(p[0], std::min(p[1], p[2])); };
    auto sat = [&](size_t i) {
        const uint8_t* p = &img[i * 4];
        return (int)std::max(p[0], std::max(p[1], p[2])) - (int)std::min(p[0], std::min(p[1], p[2]));
    };
    auto whiteish = [&](size_t i) { return minc(i) > 222 && sat(i) < 28; };
    auto veryWhite = [&](size_t i) { return minc(i) > 238 && sat(i) < 14; };

    std::vector<uint8_t> bg(N, 0);
    std::vector<int> stack;
    // 1. border-connected background
    auto flood = [&](int sx, int sy, uint8_t mark, bool (*accept)(void*, size_t), void* ctx, std::vector<int>* region) {
        stack.clear();
        size_t si = (size_t)sy * w + sx;
        if (bg[si] || !accept(ctx, si)) return;
        bg[si] = mark;
        stack.push_back((int)si);
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            if (region) region->push_back(i);
            int x = i % w, y = i / w;
            const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                size_t j = (size_t)ny[k] * w + nx[k];
                if (!bg[j] && accept(ctx, j)) { bg[j] = mark; stack.push_back((int)j); }
            }
        }
    };
    struct Ctx { decltype(whiteish)* white; decltype(veryWhite)* very; };
    Ctx ctx{&whiteish, &veryWhite};
    auto acceptWhite = [](void* c, size_t i) { return (*static_cast<Ctx*>(c)->white)(i); };
    auto acceptVery = [](void* c, size_t i) { return (*static_cast<Ctx*>(c)->very)(i); };
    for (int x = 0; x < w; ++x) { flood(x, 0, 1, acceptWhite, &ctx, nullptr); flood(x, h - 1, 1, acceptWhite, &ctx, nullptr); }
    for (int y = 0; y < h; ++y) { flood(0, y, 1, acceptWhite, &ctx, nullptr); flood(w - 1, y, 1, acceptWhite, &ctx, nullptr); }
    // 2. enclosed very-white pockets of at least 0.05% of the image
    std::vector<int> region;
    for (size_t i = 0; i < N; ++i) {
        if (bg[i] || !veryWhite(i)) continue;
        region.clear();
        flood((int)(i % w), (int)(i / w), 2, acceptVery, &ctx, &region);
        if (region.size() < N / 2000)
            for (int j : region) bg[j] = 3;  // too small: keep (a highlight, a label)
    }
    // 2b. soft drop shadows: from the background, spread into low-color gray
    //     pixels only while brightness changes gently (a shadow fades; a
    //     console's outline is a sharp edge). Shadow pixels become black with
    //     alpha from their darkness, which vanishes on a dark panel.
    std::vector<uint8_t> shadow(N, 0);
    auto lum = [&](size_t i) { const uint8_t* p = &img[i * 4]; return (p[0] * 3 + p[1] * 6 + p[2]) / 10; };
    // Only for a mostly dark console (median brightness of what is left): under
    // light gray plastic a shadow barely shows, and its soft near-white edges
    // are indistinguishable from the plastic itself (the NES top went black).
    int hist[256] = {0}, fg = 0;
    for (size_t i = 0; i < N; ++i) if (bg[i] == 0 || bg[i] == 3) { ++hist[lum(i)]; ++fg; }
    int median = 255;
    for (int v = 0, acc = 0; v < 256; ++v) { acc += hist[v]; if (acc * 2 >= fg) { median = v; break; } }
    if (median < 110) {
        // Edge strength: brightness change across 4 pixels, on a 3x3-smoothed
        // copy. A shadow changes a little over dozens of pixels; an object's
        // outline (even a soft, anti-aliased one) changes a lot over a few.
        // The shadow never spreads into an edge pixel, so it cannot leak across
        // the outline into light gray plastic (the NES top did, before).
        std::vector<int16_t> sm(N);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int sum = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        sum += lum((size_t)yy * w + xx);
                        ++n;
                    }
                sm[(size_t)y * w + x] = (int16_t)(sum / n);
            }
        auto edge = [&](int x, int y) {
            auto at = [&](int xx, int yy) {
                xx = std::max(0, std::min(w - 1, xx));
                yy = std::max(0, std::min(h - 1, yy));
                return (int)sm[(size_t)yy * w + xx];
            };
            return std::max(std::abs(at(x + 2, y) - at(x - 2, y)), std::abs(at(x, y + 2) - at(x, y - 2)));
        };
        stack.clear();
        for (size_t i = 0; i < N; ++i) if (bg[i] == 1 || bg[i] == 2) stack.push_back((int)i);
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            int x = i % w, y = i / w, li = lum((size_t)i);
            const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                size_t j = (size_t)ny[k] * w + nx[k];
                if (bg[j] == 1 || bg[j] == 2 || shadow[j]) continue;
                int lj = lum(j);
                if (sat(j) < 20 && lj > 110 && std::abs(lj - li) <= 3 && edge(nx[k], ny[k]) <= 6) {
                    shadow[j] = 1;
                    stack.push_back((int)j);
                }
            }
        }
        for (size_t i = 0; i < N; ++i)
            if (shadow[i]) {
                uint8_t* p = &img[i * 4];
                float a = std::max(0.0f, std::min(1.0f, (250.0f - lum(i)) / 250.0f));
                p[0] = p[1] = p[2] = 0;
                p[3] = (uint8_t)(a * 255.0f);
                bg[i] = 4;
            }
    }
    // 3. distance (in pixels, up to 3) from the background, for the soft band
    std::vector<uint8_t> dist(N, 255);
    for (size_t i = 0; i < N; ++i) if (bg[i] == 1 || bg[i] == 2) dist[i] = 0;
    for (int pass = 1; pass <= 3; ++pass)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = (size_t)y * w + x;
                if (dist[i] != 255) continue;
                bool near = (x > 0 && dist[i - 1] == pass - 1) || (x + 1 < w && dist[i + 1] == pass - 1) ||
                            (y > 0 && dist[i - w] == pass - 1) || (y + 1 < h && dist[i + w] == pass - 1);
                if (near) dist[i] = (uint8_t)pass;
            }
    for (size_t i = 0; i < N; ++i) {
        uint8_t* p = &img[i * 4];
        if (bg[i] == 4) continue;  // shadow pixels are already final
        if (dist[i] == 0) { p[3] = 0; continue; }
        if (dist[i] > 3) continue;
        // How much of this pixel is the white background: 0 (none) .. 1 (all).
        float whiteness = std::max(0.0f, std::min(1.0f, (minc(i) - 170.0f) / 80.0f)) * (sat(i) < 40 ? 1.0f : 0.3f);
        float a = std::max(0.0f, 1.0f - whiteness * (1.0f - (dist[i] - 1) / 3.0f));
        if (a < 0.02f) { p[3] = 0; continue; }
        for (int k = 0; k < 3; ++k)  // un-mix the white: c = a*fg + (1-a)*255
            p[k] = (uint8_t)std::max(0.0f, std::min(255.0f, (p[k] - (1.0f - a) * 255.0f) / a));
        p[3] = (uint8_t)(a * 255.0f);
    }
}

bool loadImage(const std::string& path, std::vector<uint8_t>& out, int& w, int& h) {
    int n = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return false;
    out.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

} // namespace

bool consoleCutout(const std::string& appDir, const std::string& system, int maxW, int maxH,
                   std::vector<uint8_t>& out, int& outW, int& outH) {
    std::string path = findConsoleArt(appDir, system);
    std::vector<uint8_t> img;
    int w = 0, h = 0;
    if (path.empty() || !loadImage(path, img, w, h)) return false;
    keyOutWhite(img, w, h);
    // Crop to what is left, so the console fills its slot.
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (img[((size_t)y * w + x) * 4 + 3] > 16) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < x0) return false;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    float scale = std::min(1.0f, std::min(maxW / (float)cw, maxH / (float)ch));
    outW = std::max(1, (int)std::lround(cw * scale));
    outH = std::max(1, (int)std::lround(ch * scale));
    out.assign((size_t)outW * outH * 4, 0);
    // Area average with premultiplied alpha (no dark fringe from clear pixels).
    for (int oy = 0; oy < outH; ++oy) {
        int sy0 = y0 + oy * ch / outH, sy1 = std::max(sy0 + 1, y0 + (oy + 1) * ch / outH);
        for (int ox = 0; ox < outW; ++ox) {
            int sx0 = x0 + ox * cw / outW, sx1 = std::max(sx0 + 1, x0 + (ox + 1) * cw / outW);
            double acc[4] = {0, 0, 0, 0};
            for (int y = sy0; y < sy1; ++y)
                for (int x = sx0; x < sx1; ++x) {
                    const uint8_t* p = &img[((size_t)y * w + x) * 4];
                    double a = p[3] / 255.0;
                    acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += p[3];
                }
            double n = (double)(sy1 - sy0) * (sx1 - sx0);
            uint8_t* o = &out[((size_t)oy * outW + ox) * 4];
            double a = acc[3] / n;
            o[3] = (uint8_t)std::lround(a);
            if (a > 0.5)
                for (int k = 0; k < 3; ++k)
                    o[k] = (uint8_t)std::min(255.0, acc[k] / n / (a / 255.0));
        }
    }
    return true;
}

namespace {

// Title matching, the same rules as tools/fetch_boxart.py: the title before any
// (tag) or [tag], "Legend of Zelda, The" -> "the legend of zelda", & -> and,
// then letters and digits only.
std::string titleOf(const std::string& stem) {
    size_t cut = stem.find_first_of("([");
    std::string t = stem.substr(0, cut);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

// The title before a " - " subtitle ("Desert Strike - Return to the Gulf" ->
// "Desert Strike"), or the whole title.
std::string mainTitle(const std::string& title) {
    size_t dash = title.find(" - ");
    return dash == std::string::npos ? title : title.substr(0, dash);
}

std::string subtitleOf(const std::string& title) {
    size_t dash = title.find(" - ");
    return dash == std::string::npos ? "" : title.substr(dash + 3);
}

// `underscoreAnd`: libretro file names spell both & and / (and : ? ...) as _,
// so covers are indexed under both readings.
std::string titleKey(std::string t, bool underscoreAnd = true) {
    // A trailing article moves to the front: at the end ("Legend of Zelda, The")
    // or before a subtitle ("Ren & Stimpy Show Presents, The - Stimpy's Invention").
    for (const char* art : {", The", ", An", ", A"}) {
        size_t n = std::strlen(art), at = std::string::npos;
        if (t.size() > n && t.compare(t.size() - n, n, art) == 0) at = t.size() - n;
        else at = t.find(std::string(art) + " - ");
        if (at != std::string::npos) { t = std::string(art + 2) + " " + t.substr(0, at) + t.substr(at + n); break; }
    }
    // Roman numerals as digits, so "Street Fighter II" meets "streetfighter2".
    static const std::map<std::string, std::string> romans = {
        {"ii", "2"}, {"iii", "3"}, {"iv", "4"}, {"v", "5"}, {"vi", "6"}, {"vii", "7"},
        {"viii", "8"}, {"ix", "9"}, {"x", "10"}};
    std::string words;
    for (size_t i = 0; i <= t.size();) {
        size_t j = t.find(' ', i);
        if (j == std::string::npos) j = t.size();
        std::string w = t.substr(i, j - i);
        for (char& ch : w) ch = (char)std::tolower((unsigned char)ch);
        // Punctuation after a numeral counts too ("II:", "II_" in MAME's names, "II'").
        size_t core = w.size();
        while (core > 0 && !std::isalnum((unsigned char)w[core - 1])) --core;
        auto r = romans.find(w.substr(0, core));
        words += (r != romans.end() ? r->second + w.substr(core) : w) + " ";
        i = j + 1;
    }
    t = words;
    std::string k;
    for (size_t i = 0; i < t.size(); ++i) {
        unsigned char c = (unsigned char)t[i];
        if (c == '&' || (c == '_' && underscoreAnd)) k += "and";
        else if (std::isalnum(c)) k += (char)std::tolower(c);
    }
    if (k.compare(0, 5, "adand") == 0) k = "advanceddungeonsanddragons" + k.substr(5);  // "AD&D Hillsfar"
    return k;
}

std::string tagsOf(const std::string& stem) {
    size_t cut = stem.find_first_of("([");
    return cut == std::string::npos ? "" : stem.substr(cut);
}

// The ROM's region from GoodTools codes ("(U)", "(JU)") or No-Intro words.
std::string regionOf(const std::string& tags) {
    static const std::pair<const char*, const char*> codes[] = {
        {"(U)", "USA"}, {"(UE)", "USA"}, {"(JU)", "USA"}, {"(E)", "Europe"}, {"(EU)", "Europe"},
        {"(J)", "Japan"}, {"(W)", "World"}};
    for (const auto& c : codes)
        if (tags.find(c.first) != std::string::npos) return c.second;
    for (const char* w : {"USA", "Europe", "Japan", "World"})
        if (tags.find(w) != std::string::npos) return w;
    return "USA";
}

// Lower is better: the ROM's region first, then USA / World / Europe / Japan;
// betas and pirates, and special releases ("(Sonic Mega Collection)", "(Virtual
// Console)") count against a cover more than a different region does.
int coverScore(const std::string& file, const std::string& region) {
    std::string tags = tagsOf(file.substr(0, file.find_last_of('.')));
    int parens = 0;
    for (size_t open = tags.find('('); open != std::string::npos; open = tags.find('(', open + 1)) {
        size_t close = tags.find(')', open);
        std::string t = tags.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
        bool plain = t.compare(0, 4, "Rev ") == 0 || t.compare(0, 1, "v") == 0;
        for (const char* w : {"USA", "Europe", "Japan", "World", "Korea", "Brazil", "Asia", "Australia", "En", "Fr", "De", "Es", "It", "Ja"})
            if (t.find(w) != std::string::npos) plain = true;
        parens += plain ? 1 : 15;
    }
    const std::string order[] = {region, "USA", "World", "Europe", "Japan"};
    for (int i = 0; i < 5; ++i)
        if (tags.find(order[i]) != std::string::npos) {
            bool odd = false;
            for (const char* w : {"Beta", "Proto", "Sample", "Demo", "Pirate", "Unl"})
                if (tags.find(w) != std::string::npos) odd = true;
            return i * 10 + (odd ? 5 : 0) + parens;
        }
    return 100 + parens;
}

using ArtIndex = std::map<std::string, std::vector<std::string>>;

// File names indexed by title key (and "\x01" + main title key).
ArtIndex buildIndex(const std::vector<std::string>& names) {
    ArtIndex index;
    for (const std::string& n : names) {
        if (n.empty() || n[0] == '.') continue;
        const std::string t = titleOf(n.substr(0, n.find_last_of('.')));
        for (int pass = 0; pass < (t.find('_') != std::string::npos ? 2 : 1); ++pass) {
            std::string k = titleKey(t, pass == 0);
            if (k.empty()) continue;
            auto add = [&](const std::string& key) {
                std::vector<std::string>& v = index[key];
                if (v.empty() || v.back() != n) v.push_back(n);
            };
            add(k);
            if (k.compare(0, 3, "the") == 0 && k.size() > 3) add(k.substr(3));
            std::string m = titleKey(mainTitle(t), pass == 0);
            if (!m.empty()) add("\x01" + m);  // by main title
            if (m.compare(0, 3, "the") == 0 && m.size() > 3) add("\x01" + m.substr(3));
        }
    }
    return index;
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (struct dirent* e = ::readdir(d)) names.push_back(e->d_name);
        ::closedir(d);
    }
    return names;
}

std::mutex g_indexMu;
std::map<std::string, ArtIndex> g_indexes;  // by folder (or a downloader list's name)

// media/<system>/Named_Boxarts indexed by title key, built once per system.
const std::vector<std::string>* coversFor(const std::string& dir, const std::string& key) {
    std::lock_guard<std::mutex> lock(g_indexMu);
    auto it = g_indexes.find(dir);
    if (it == g_indexes.end()) it = g_indexes.emplace(dir, buildIndex(listDir(dir))).first;
    auto hit = it->second.find(key);
    return hit == it->second.end() ? nullptr : &hit->second;
}

} // namespace

void warmArtIndex(const std::string& appDir, const std::string& system) {
    coversFor(appDir + "/media/" + system + "/Named_Boxarts", "");
    coversFor(appDir + "/media/" + system + "/Named_Logos", "");
}

void setArtIndex(const std::string& dir, const std::vector<std::string>& names) {
    ArtIndex index = buildIndex(names);  // outside the lock: lookups carry on meanwhile
    std::lock_guard<std::mutex> lock(g_indexMu);
    g_indexes[dir].swap(index);
    // matchCover holds pointers into an index after the lock is released, so
    // a folder's old index is kept (a download list's only the downloader reads).
    static std::list<ArtIndex> retired;
    if (!dir.empty() && dir[0] != '@') retired.push_back(std::move(index));
}

void refreshArtIndex(const std::string& appDir, const std::string& system) {
    for (const char* f : {"/Named_Boxarts", "/Named_Logos"}) {
        std::string dir = appDir + "/media/" + system + f;
        setArtIndex(dir, listDir(dir));
    }
}

std::string matchCover(const std::string& base, const std::string& stem, const char* folder);

// A game's logo (media/<system>/Named_Logos, from prefill_boxart.py --logos), by
// file name or title like the covers. NAOMI / Atomiswave use the arcade (MAME) set.
std::string findLogo(const std::string& appDir, const std::string& system, const std::string& romFile) {
    std::string stem = romFile.substr(0, romFile.find_last_of('.'));
    std::string safe = stem;
    for (char& ch : safe)
        if (std::strchr("&*/:`<>?\\|\"", ch)) ch = '_';
    std::vector<std::string> systems{system};
    if (system == "naomi" || system == "atomiswave") systems.push_back("arcade");
    for (const std::string& sys : systems) {
        std::string base = appDir + "/media/" + sys + "/";
        for (const std::string* name : {&stem, &safe}) {
            std::string p = base + "Named_Logos/" + *name + ".png";
            if (isFile(p)) return p;
        }
        std::string match = matchCover(base, stem, "Named_Logos");
        if (!match.empty()) return base + "Named_Logos/" + match;
    }
    return "";
}

std::string matchCover(const std::string& base, const std::string& stem, const char* folder = "Named_Boxarts");

// Holds findArt's filtered loose matches (best points into it until it returns).
static std::vector<std::string>& looseKeep() {
    static thread_local std::vector<std::string> v;
    return v;
}

std::string findArt(const std::string& appDir, const std::string& system, const std::string& romFile) {
    std::string stem = romFile.substr(0, romFile.find_last_of('.'));
    // libretro-thumbnails replaces these characters with '_' in file names.
    std::string safe = stem;
    for (char& ch : safe)
        if (std::strchr("&*/:`<>?\\|\"", ch)) ch = '_';
    static const char* kFolders[] = {"boxart", "Named_Boxarts", "snaps", "Named_Snaps", "titles", "Named_Titles"};
    std::string base = appDir + "/media/" + system + "/";
    for (const char* f : kFolders)
        for (const std::string* name : {&stem, &safe})
            for (const char* ext : {".png", ".jpg"}) {
                std::string p = base + f + "/" + *name + ext;
                if (isFile(p)) return p;
            }
    // No cover under the ROM's own name: match the title against the prefilled
    // covers (tools/prefill_boxart.py), preferring the ROM's region.
    std::string match = matchCover(base, stem);
    return match.empty() ? "" : base + "Named_Boxarts/" + match;
}

// The prefilled cover (file name in media/<system>/Named_Boxarts) whose title
// matches the ROM's, or "".
std::string matchCover(const std::string& base, const std::string& stem, const char* folder) {
    std::string title = titleOf(stem), key = titleKey(title);
    if (key.empty()) return "";
    const std::string dir = base + folder;
    const std::vector<std::string>* c = coversFor(dir, key);
    if (!c && key.compare(0, 3, "the") == 0) c = coversFor(dir, key.substr(3));
    std::string region = regionOf(tagsOf(stem));
    const std::string* best = nullptr;
    int bestScore = 1 << 30;
    auto consider = [&](const std::vector<std::string>* list, int penalty) {
        if (!list) return;
        for (const std::string& n : *list) {
            int sc = coverScore(n, region) + penalty;
            if (sc < bestScore) { bestScore = sc; best = &n; }
        }
    };
    consider(c, 0);
    // Nothing exact, or only betas / prototypes / bad dumps: also take covers
    // with the same main title, so "Desert Strike - Return to the Gulf" finds
    // "Desert Strike (Europe)" and "Arch Rivals" finds "Arch Rivals - A Basketbrawl!".
    bool weak = !best;
    if (best) {
        std::string t = tagsOf(best->substr(0, best->find_last_of('.')));
        for (const char* w : {"Beta", "Proto", "Sample", "Demo", "Pirate", "["})
            if (t.find(w) != std::string::npos) weak = true;
    }
    if (weak) {
        // Only when it is clearly the same game: the cover has no subtitle, or one
        // subtitle starts the other, or the ROM has none and every cover agrees
        // ("GI Joe" has two different games; "Wheel of Fortune - X" four editions).
        std::string m = titleKey(mainTitle(title));
        const std::vector<std::string>* loose = m.empty() ? nullptr : coversFor(dir, "\x01" + m);
        if (!loose && m.compare(0, 3, "the") == 0) loose = coversFor(dir, "\x01" + m.substr(3));
        if (loose) {
            auto subKey = [](const std::string& file) {
                return titleKey(subtitleOf(titleOf(file.substr(0, file.find_last_of('.')))));
            };
            std::string romSub = titleKey(subtitleOf(title));
            // Count the different subtitles among the covers from the ROM's own
            // region (all covers if it has none): other regions' releases and
            // archive copies spelled differently ("Buckaroo$" / "Buckeroo$!")
            // aren't another game.
            bool anyInRegion = false;
            for (const std::string& n : *loose)
                if (tagsOf(n.substr(0, n.find_last_of('.'))).find(region) != std::string::npos) anyInRegion = true;
            std::set<std::string> subs;
            for (const std::string& n : *loose) {
                if (anyInRegion && tagsOf(n.substr(0, n.find_last_of('.'))).find(region) == std::string::npos) continue;
                std::string cs = subKey(n);
                if (!cs.empty()) subs.insert(cs);
            }
            std::vector<std::string> ok;
            for (const std::string& n : *loose) {
                std::string cs = subKey(n);
                bool same = cs.empty() ||
                            (!romSub.empty() && (cs.compare(0, romSub.size(), romSub) == 0 ||
                                                 romSub.compare(0, cs.size(), cs) == 0)) ||
                            (romSub.empty() && subs.size() == 1);
                if (same) ok.push_back(n);
            }
            std::vector<std::string>& keep = looseKeep();
            keep.swap(ok);
            consider(&keep, 3);
        }
    }
    return best ? *best : "";
}

std::string niceTitle(const std::string& appDir, const std::string& system, const std::string& romFile) {
    std::string stem = romFile.substr(0, romFile.find_last_of('.'));
    // Only names squashed into one lowercase word ("supermarioworld").
    if (stem.empty()) return "";
    for (unsigned char c : stem)
        if (!(std::islower(c) || std::isdigit(c))) return "";
    std::string match = matchCover(appDir + "/media/" + system + "/", stem);
    if (match.empty()) return "";
    std::string t = titleOf(match.substr(0, match.find_last_of('.')));
    for (char& ch : t)
        if (ch == '_') ch = '&';  // libretro file names spell & as _
    // "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past"
    for (const char* art : {", The", ", An", ", A"}) {
        size_t n = std::strlen(art), at = std::string::npos;
        if (t.size() > n && t.compare(t.size() - n, n, art) == 0) at = t.size() - n;
        else at = t.find(std::string(art) + " - ");
        if (at != std::string::npos) { t = std::string(art + 2) + " " + t.substr(0, at) + t.substr(at + n); break; }
    }
    return t;
}

GamePanels::~GamePanels() {
    // Stop the worker before releasing the buffers it draws into (and before
    // SDL closes the fd they live on; Menu::shutdown destroys us first).
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_one();
        m_thread.join();
    }
    for (Panel& p : m_panels) {
        if (p.map) ::munmap(p.map, p.size);
        if (p.fbId) ::ioctl(m_fd, DRM_IOCTL_MODE_RMFB, &p.fbId);
        if (p.handle) {
            drm_mode_destroy_dumb dd;
            std::memset(&dd, 0, sizeof(dd));
            dd.handle = p.handle;
            ::ioctl(m_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
    }
}

// The CRTC to drive a connector with. Normally the one its current encoder is
// bound to; but once a screen has been switched off (our buffers are released
// when the menu hands over to a game) the connector can come back with no
// encoder, so fall back to a CRTC one of its encoders can use that isn't
// driving the playfield or a panel we already claimed.
uint32_t GamePanels::pickCrtc(uint32_t currentEncoder, const std::vector<uint32_t>& encoders, uint32_t count,
                              uint32_t mainConnector) {
    if (currentEncoder) {
        drm_mode_get_encoder en;
        std::memset(&en, 0, sizeof(en));
        en.encoder_id = currentEncoder;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &en) == 0 && en.crtc_id) return en.crtc_id;
    }
    drm_mode_card_res res;
    std::memset(&res, 0, sizeof(res));
    if (::ioctl(m_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_crtcs == 0) return 0;
    std::vector<uint32_t> crtcs(res.count_crtcs);
    res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs.data());
    res.count_connectors = res.count_encoders = res.count_fbs = 0;
    res.connector_id_ptr = res.encoder_id_ptr = res.fb_id_ptr = 0;
    if (::ioctl(m_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) return 0;
    // CRTCs that must not be taken: the playfield's, and panels already claimed.
    std::vector<uint32_t> taken;
    for (const Panel& p : m_panels) taken.push_back(p.crtcId);
    if (mainConnector) {
        drm_mode_get_connector mc;
        std::memset(&mc, 0, sizeof(mc));
        mc.connector_id = mainConnector;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &mc) == 0 && mc.encoder_id) {
            drm_mode_get_encoder me;
            std::memset(&me, 0, sizeof(me));
            me.encoder_id = mc.encoder_id;
            if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &me) == 0 && me.crtc_id) taken.push_back(me.crtc_id);
        }
    }
    for (uint32_t e = 0; e < count && e < encoders.size(); ++e) {
        drm_mode_get_encoder en;
        std::memset(&en, 0, sizeof(en));
        en.encoder_id = encoders[e];
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &en) != 0) continue;
        for (uint32_t i = 0; i < crtcs.size() && i < 32; ++i) {
            if (!(en.possible_crtcs & (1u << i))) continue;
            if (std::find(taken.begin(), taken.end(), crtcs[i]) != taken.end()) continue;
            log("panels: connector had no active encoder; using encoder %u with crtc %u", en.encoder_id, crtcs[i]);
            return crtcs[i];
        }
    }
    return 0;
}

int GamePanels::init(const DisplayProfile::Topology& topo, Mode mode, uint32_t gameConnector) {
    m_mode = mode;
    m_fd = findCard0Fd();
    if (m_fd < 0) { log("panels: no card0 fd"); return 0; }
    // Whatever SDL owns (the menu on the playfield, or the game's screen) is
    // left alone; every other screen gets artwork.
    uint32_t avoid = mode == Mode::Playing ? (gameConnector ? gameConnector : topo.backglass.connectorId)
                                           : topo.main.connectorId;
    for (const DisplayProfile::Screen* scr : {&topo.main, &topo.backglass, &topo.dmd}) {
        if (!scr->available || scr->connectorId == 0 || scr->connectorId == avoid) continue;
        Panel::Role role = scr == &topo.dmd ? Panel::Role::Dmd
                           : scr == &topo.main ? Panel::Role::Playfield : Panel::Role::Backglass;
        drm_mode_get_connector cn;
        std::memset(&cn, 0, sizeof(cn));
        cn.connector_id = scr->connectorId;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn) != 0 || cn.count_modes == 0) {
            log("panels: connector %u unavailable", scr->connectorId);
            continue;
        }
        std::vector<drm_mode_modeinfo> modes(cn.count_modes);
        std::vector<uint32_t> encoders(cn.count_encoders ? cn.count_encoders : 1);
        cn.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
        cn.encoders_ptr = reinterpret_cast<uint64_t>(encoders.data());
        cn.count_props = 0;
        cn.props_ptr = cn.prop_values_ptr = 0;
        ::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn);
        uint32_t crtc = pickCrtc(cn.encoder_id, encoders, cn.count_encoders, avoid);
        if (!crtc) { log("panels: no display controller for connector %u", scr->connectorId); continue; }
        const drm_mode_modeinfo mode = modes[0];

        drm_mode_create_dumb cd;
        std::memset(&cd, 0, sizeof(cd));
        cd.width = mode.hdisplay; cd.height = mode.vdisplay; cd.bpp = 32;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) { log("panels: CREATE_DUMB %s", std::strerror(errno)); continue; }
        drm_mode_fb_cmd fbc;
        std::memset(&fbc, 0, sizeof(fbc));
        fbc.width = cd.width; fbc.height = cd.height; fbc.pitch = cd.pitch; fbc.bpp = 32; fbc.depth = 24; fbc.handle = cd.handle;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_ADDFB, &fbc) != 0) { log("panels: ADDFB %s", std::strerror(errno)); continue; }
        drm_mode_map_dumb md;
        std::memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) continue;
        uint8_t* map = (uint8_t*)::mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, md.offset);
        if (map == MAP_FAILED) continue;

        Panel p;
        p.connId = scr->connectorId; p.crtcId = crtc; p.fbId = fbc.fb_id; p.handle = cd.handle;
        p.pitch = cd.pitch; p.w = mode.hdisplay; p.h = mode.vdisplay; p.map = map; p.size = cd.size;
        p.role = role;
        if (role == Panel::Role::Playfield) {
            // Mounted portrait: compose portrait and turn it 90 degrees like the
            // menu's canvas (the same buffer mapping as the DMD's).
            p.rot = topo.main.rotationDegrees == 90 || topo.main.rotationDegrees == 270;
            p.vw = p.rot ? p.h : p.w;
            p.vh = p.rot ? p.w : p.h;
        } else {
            p.rot = p.h > p.w;
            p.vw = p.rot ? p.h : p.w;
            p.vh = p.rot ? p.w : p.h;
        }
        p.canvas.assign((size_t)p.vw * p.vh * 4, 0);

        drm_mode_crtc sc;
        std::memset(&sc, 0, sizeof(sc));
        sc.crtc_id = p.crtcId; sc.fb_id = p.fbId;
        sc.set_connectors_ptr = reinterpret_cast<uint64_t>(&p.connId);
        sc.count_connectors = 1; sc.mode = mode; sc.mode_valid = 1;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_SETCRTC, &sc) != 0) {
            log("panels: SETCRTC %u: %s", p.connId, std::strerror(errno));
            ::munmap(map, cd.size);
            continue;
        }
        log("panels: %s up on connector %u (%dx%d%s)",
            role == Panel::Role::Dmd ? "DMD" : role == Panel::Role::Playfield ? "playfield" : "backglass", p.connId, p.w,
            p.h, p.rot ? ", rotated" : "");
        m_panels.push_back(std::move(p));
    }
    if (!m_panels.empty()) m_thread = std::thread(&GamePanels::worker, this);
    return (int)m_panels.size();
}

void GamePanels::show(const Item& item, float dt) {
    if (m_panels.empty()) return;
    if (item.key != m_pendingKey) { m_pendingKey = item.key; m_stable = 0.0f; }
    else m_stable += dt;
    // Wait until the highlight rests a moment (or immediately the first time),
    // then hand it to the worker; a newer item replaces one not yet started.
    if (m_pendingKey != m_shownKey && (m_stable >= 0.12f || m_shownKey == "\x01")) {
        m_shownKey = m_pendingKey;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_job = item;
            m_hasJob = true;
        }
        m_cv.notify_one();
    }
}

void GamePanels::worker() {
    for (;;) {
        Item job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || m_hasJob; });
            if (m_stop) return;
            job = m_job;
            m_hasJob = false;
        }
        auto t0 = std::chrono::steady_clock::now();
        compose(job);
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        log("panels: %s in %ld ms%s", job.key.c_str(), ms, job.artPath.empty() ? "" : " (with art)");
    }
}

void GamePanels::compose(const Item& item) {
    std::vector<uint8_t> art;
    int aw = 0, ah = 0;
    if (!item.artPath.empty()) {
        int n = 0;
        if (uint8_t* px = stbi_load(item.artPath.c_str(), &aw, &ah, &n, 4)) {
            art.assign(px, px + (size_t)aw * ah * 4);
            stbi_image_free(px);
        } else {
            log("panels: could not decode %s", item.artPath.c_str());
        }
    }
    // No art yet: a generated cover (games only; systems keep the title card).
    if (art.empty() && !item.title.empty()) art = makeCover(item.system, item.title, aw, ah);
    std::vector<uint8_t> console;
    int cw = 0, ch = 0;
    if (!item.consolePath.empty()) {
        if (loadImage(item.consolePath, console, cw, ch)) keyOutWhite(console, cw, ch);
        else log("panels: could not decode %s", item.consolePath.c_str());
    }
    std::vector<uint8_t> logo;
    int lw = 0, lh = 0;
    if (!item.logoPath.empty() && !loadImage(item.logoPath, logo, lw, lh))
        log("panels: could not decode %s", item.logoPath.c_str());
    for (Panel& p : m_panels) {
        if (p.role == Panel::Role::Dmd) composeDmd(p, item, console, cw, ch, logo, lw, lh);
        else if (p.role == Panel::Role::Playfield) composePlayfield(p, item, art, aw, ah);
        else composeBackglass(p, item, art, aw, ah);
        present(p);
    }
}

void GamePanels::composeBackglass(Panel& p, const Item& item, const std::vector<uint8_t>& art, int aw, int ah) {
    const int W = p.vw, H = p.vh;
    std::vector<uint8_t>& c = p.canvas;
    const std::string headline = item.title.empty() ? item.system : item.title;
    if (!art.empty()) {
        backdrop(c, W, H, art, aw, ah);
        blitFit(c, W, H, art, aw, ah, (int)(W * 0.08f), (int)(H * 0.07f), (int)(W * 0.84f), (int)(H * 0.66f));
        text(c, W, H, headline, W * 0.5f, H * 0.78f, H * 0.085f, kInk, PanelFont::Face::Display, W * 0.9f);
        text(c, W, H, item.detail.empty() ? item.system : item.detail, W * 0.5f, H * 0.885f, H * 0.045f, kGold,
             PanelFont::Face::Body, W * 0.8f);
        return;
    }
    // Title card: Neon gradient, two light pools, big title.
    gradient(c, W, H, kGradTop, kGradBot);
    glow(c, W, H, W * 0.18f, H * 0.2f, H * 0.9f, kTeal, 0.22f);
    glow(c, W, H, W * 0.85f, H * 0.85f, H * 1.0f, kViolet, 0.25f);
    text(c, W, H, item.title.empty() ? "RETRO LAUNCHER" : item.system, W * 0.5f, H * 0.22f, H * 0.06f, kTeal,
         PanelFont::Face::Display, W * 0.8f);
    text(c, W, H, headline, W * 0.5f, H * 0.36f, H * 0.18f, kInk, PanelFont::Face::Display, W * 0.88f);
    text(c, W, H, item.detail, W * 0.5f, H * 0.62f, H * 0.055f, kGold, PanelFont::Face::Body, W * 0.8f);
}

void GamePanels::composeDmd(Panel& p, const Item& item, const std::vector<uint8_t>& console, int cw, int ch,
                            const std::vector<uint8_t>& logo, int lw, int lh) {
    const int W = p.vw, H = p.vh;
    std::vector<uint8_t>& c = p.canvas;
    gradient(c, W, H, kGradTop, kGradBot);
    glow(c, W, H, W * 0.5f, H * 0.5f, W * 0.5f, kTeal, 0.18f);
    // The game's logo, like a marquee, browsing or playing; the system's name under it.
    if (!logo.empty()) {
        glow(c, W, H, W * 0.5f, H * 0.42f, W * 0.45f, kWhite, 0.08f);
        blitFit(c, W, H, logo, lw, lh, (int)(W * 0.06f), (int)(H * 0.08f), (int)(W * 0.88f), (int)(H * 0.68f));
        text(c, W, H, item.system, W * 0.5f, H * 0.84f, H * 0.09f, kGold, PanelFont::Face::Display, W * 0.9f);
        return;
    }
    if (m_mode == Mode::Playing) {
        // The playfield card already names the game: show the console itself.
        if (!console.empty()) {
            glow(c, W, H, W * 0.5f, H * 0.46f, W * 0.42f, kWhite, 0.10f);
            blitFit(c, W, H, console, cw, ch, (int)(W * 0.06f), (int)(H * 0.06f), (int)(W * 0.88f), (int)(H * 0.76f));
            text(c, W, H, item.system, W * 0.5f, H * 0.85f, H * 0.09f, kGold, PanelFont::Face::Display, W * 0.9f);
        } else {
            text(c, W, H, item.system, W * 0.5f, H * 0.32f, H * 0.32f, kInk, PanelFont::Face::Display, W * 0.92f);
        }
        return;
    }
    if (item.title.empty()) {
        text(c, W, H, "RETRO LAUNCHER", W * 0.5f, H * 0.16f, H * 0.12f, kTeal, PanelFont::Face::Display, W * 0.9f);
        text(c, W, H, item.system, W * 0.5f, H * 0.38f, H * 0.26f, kInk, PanelFont::Face::Display, W * 0.92f);
    } else {
        text(c, W, H, item.system, W * 0.5f, H * 0.14f, H * 0.13f, kGold, PanelFont::Face::Display, W * 0.9f);
        text(c, W, H, item.title, W * 0.5f, H * 0.38f, H * 0.24f, kInk, PanelFont::Face::Display, W * 0.94f);
    }
    text(c, W, H, item.detail, W * 0.5f, H * 0.76f, H * 0.09f, kTeal, PanelFont::Face::Body, W * 0.9f);
}

// Darken a rectangle (a translucent card) with a light top edge.
void card(std::vector<uint8_t>& c, int W, int H, int x0, int y0, int w, int h, float darken) {
    for (int y = std::max(0, y0); y < std::min(H, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x0 + w); ++x) {
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            bool edge = y < y0 + 3;
            for (int k = 0; k < 3; ++k) p[k] = edge ? (uint8_t)std::min(255, p[k] + 60) : (uint8_t)(p[k] * darken);
        }
}

// Playing mode, playfield (portrait): cover, title, controls, how to exit.
void GamePanels::composePlayfield(Panel& p, const Item& item, const std::vector<uint8_t>& art, int aw, int ah) {
    const int W = p.vw, H = p.vh;
    const float u = W / 720.0f;  // design units: the menu's 720-wide portrait canvas
    std::vector<uint8_t>& c = p.canvas;
    if (!art.empty()) backdrop(c, W, H, art, aw, ah);
    else gradient(c, W, H, kGradTop, kGradBot);
    glow(c, W, H, W * 0.15f, H * 0.08f, W * 0.9f, kTeal, 0.10f);
    glow(c, W, H, W * 0.9f, H * 0.95f, W * 1.0f, kViolet, 0.14f);

    // Fit the controls card above the footer line: a long list (8-10 buttons on
    // arcade, PlayStation, N64) first takes room from the cover, then tightens
    // its rows.
    const int rows = (int)item.controls.size();
    const float footerY = H - 74 * u, footerGap = 28 * u;
    float rowH = 46 * u, scale = 1.0f;
    const float cardTop0 = 850 * u;
    auto cardHeight = [&](float rh) { return rows ? 64 * u + rows * rh : 0.0f; };
    float lift = 0.0f;  // how far the cover shrinks (and everything below it moves up)
    float over = cardTop0 + cardHeight(rowH) - (footerY - footerGap);
    if (over > 0) {
        lift = std::min(over, 260 * u);
        over -= lift;
    }
    if (over > 0 && rows) {
        rowH = std::max(rowH * 0.7f, (footerY - footerGap - (cardTop0 - lift) - 64 * u) / rows);
        scale = rowH / (46 * u);
    }

    text(c, W, H, "NOW PLAYING", W * 0.5f, 64 * u, 30 * u, kTeal, PanelFont::Face::Display, W * 0.8f);
    if (!art.empty())
        blitFit(c, W, H, art, aw, ah, (int)(130 * u), (int)(120 * u), (int)(460 * u), (int)(560 * u - lift));
    text(c, W, H, item.title, W * 0.5f, 712 * u - lift, 64 * u, kInk, PanelFont::Face::Display, W * 0.88f);
    std::string sub = item.detail.empty() ? item.system : item.system + "   " + item.detail;
    text(c, W, H, sub, W * 0.5f, 790 * u - lift, 26 * u, kGold, PanelFont::Face::Body, W * 0.86f);

    // Controls card.
    if (rows) {
        int cardY = (int)(cardTop0 - lift), cardH = (int)cardHeight(rowH);
        card(c, W, H, (int)(48 * u), cardY, W - (int)(96 * u), cardH, 0.45f);
        text(c, W, H, "CONTROLS", W * 0.5f, cardY + 16 * u, 22 * u, kTeal, PanelFont::Face::Display, W * 0.6f);
        const float keySize = 30 * u * scale, labelSize = 28 * u * scale;
        for (int i = 0; i < rows; ++i) {
            float y = cardY + 56 * u + i * rowH;
            const std::string& key = item.controls[i].first;
            float kw = PanelFont::measure(key, keySize, PanelFont::Face::Display);
            PanelFont::draw(c.data(), W, H, key, W * 0.40f - kw, y, keySize, kGold[0], kGold[1], kGold[2],
                            PanelFont::Face::Display);
            PanelFont::draw(c.data(), W, H, item.controls[i].second, W * 0.46f, y + 2 * u * scale, labelSize, kInk[0],
                            kInk[1], kInk[2], PanelFont::Face::Body);
        }
    }
    text(c, W, H, "Hold START or press HOME for the pause menu", W * 0.5f, H - 74 * u, 24 * u, kInk,
         PanelFont::Face::Body, W * 0.9f);
}

void GamePanels::present(Panel& p) {
    const uint8_t* c = p.canvas.data();
    for (int vy = 0; vy < p.vh; ++vy) {
        const uint8_t* srow = c + (size_t)vy * p.vw * 4;
        if (!p.rot) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(p.map + (size_t)vy * p.pitch);
            for (int vx = 0; vx < p.vw; ++vx) {
                const uint8_t* s = srow + vx * 4;
                dst[vx] = (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
            }
        } else {
            for (int vx = 0; vx < p.vw; ++vx) {  // 90° CCW for the portrait-native DMD
                const uint8_t* s = srow + vx * 4;
                int bx = p.w - 1 - vy, by = vx;
                *reinterpret_cast<uint32_t*>(p.map + (size_t)by * p.pitch + bx * 4) =
                    (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
            }
        }
    }
}
