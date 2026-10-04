#pragma once

#include "AppFont.h"
#include "AppTypes.h"
#include "Gfx.h"
#include "Utils.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

// "Neon, refined" — the jukebox-neon beta's visual system. Same Theme::
// surface as the Neon theme in jukebox-app (so screens keep compiling), but
// every shape is anti-aliased through Gfx (baked textures instead of stacked
// 1px strips), and layout follows one type ramp and an 8px grid:
//
//   Type  — Hero 88 · Title 64 · Heading 40 (display) / Body 30 · Small 24 ·
//           Caption 20 (body). Nothing else.
//   Frame — 48px side margin, header block y 40–170, content from y 200,
//           mini-player y 1072–1160, footer legend y 1176–1280.
//
// Accent color is live: Now Playing tints the UI with the playing album's
// color (Theme::accent()), falling back to brand teal.
namespace Theme {

// --- tokens ------------------------------------------------------------------

constexpr SDL_Color Ink       {10, 13, 22, 255};    // page ground
constexpr SDL_Color BgTop     {12, 16, 27, 255};
constexpr SDL_Color BgBottom  {10, 12, 20, 255};
constexpr SDL_Color HeaderL   {43, 199, 184, 255};
constexpr SDL_Color HeaderR   {124, 92, 232, 255};
constexpr SDL_Color Accent    {43, 199, 184, 255};  // brand teal
constexpr SDL_Color Accent2   {124, 92, 232, 255};  // brand violet
constexpr SDL_Color Gold      {245, 214, 95, 255};  // focus
constexpr SDL_Color Rose      {239, 92, 120, 255};
constexpr SDL_Color Live      {216, 44, 60, 255};
constexpr SDL_Color Text      {242, 245, 250, 255};
constexpr SDL_Color TextDim   {201, 210, 224, 255};
constexpr SDL_Color Muted     {154, 167, 189, 255};
constexpr SDL_Color Faint     {102, 114, 138, 255};
constexpr SDL_Color Card      {255, 255, 255, 12};  // translucent surfaces
constexpr SDL_Color CardHi    {255, 255, 255, 24};
constexpr SDL_Color Hairline  {255, 255, 255, 22};

namespace Type {
constexpr float Hero = 88.0f;
constexpr float Title = 64.0f;
constexpr float Heading = 40.0f;
constexpr float Body = 30.0f;
constexpr float Small = 24.0f;
constexpr float Caption = 20.0f;
} // namespace Type

constexpr float kMargin = 48.0f;
constexpr float kFooterTop = 1176.0f;
constexpr float kMiniTop = 1072.0f;

inline SDL_Color mix(SDL_Color a, SDL_Color b, float t) { return Gfx::mix(a, b, t); }
inline SDL_Color alpha(SDL_Color c, int a) { c.a = static_cast<Uint8>(std::max(0, std::min(255, a))); return c; }

// Shared animation clock (one instance program-wide), advanced in update().
inline float& clock()
{
    static float t = 0.0f;
    return t;
}

// Live accent: set by the suite each frame (album color on Now Playing).
inline SDL_Color& accent()
{
    static SDL_Color c = Accent;
    return c;
}

// Dark readable ink on top of a bright accent fill (play button, chips).
inline SDL_Color onAccent(SDL_Color c)
{
    return mix(c, {8, 8, 12, 255}, 0.86f);
}

// --- text helpers ------------------------------------------------------------

// Letter-spaced caption (the font has no tracking, so draw per character).
inline float trackedWidth(SDL_Renderer* r, const std::string& text, float size, float spacing)
{
    float w = 0.0f;
    for (char ch : text) w += AppFont::measureWidth(r, std::string(1, ch), size) + spacing;
    return text.empty() ? 0.0f : w - spacing;
}

inline void tracked(SDL_Renderer* r, const std::string& text, float x, float y, float size,
                    SDL_Color c, float spacing = 3.0f)
{
    for (char ch : text) {
        const std::string s(1, ch);
        AppFont::draw(r, s, x, y, size, c);
        x += AppFont::measureWidth(r, s, size) + spacing;
    }
}

inline void trackedRight(SDL_Renderer* r, const std::string& text, float right, float y,
                         float size, SDL_Color c, float spacing = 3.0f)
{
    tracked(r, text, right - trackedWidth(r, text, size, spacing), y, size, c, spacing);
}

inline void trackedCentered(SDL_Renderer* r, const std::string& text, float cx, float y,
                            float size, SDL_Color c, float spacing = 3.0f)
{
    tracked(r, text, cx - trackedWidth(r, text, size, spacing) * 0.5f, y, size, c, spacing);
}

// Trim `s` and append "..." so it fits within maxW at the given size.
inline std::string ellipsize(SDL_Renderer* r, std::string s, float maxW, float size,
                             AppFont::Face face = AppFont::Face::Body)
{
    if (AppFont::measureWidth(r, s, size, face) <= maxW) {
        return s;
    }
    while (!s.empty() && AppFont::measureWidth(r, s + "...", size, face) > maxW) {
        s.pop_back();
    }
    return s + "...";
}

// Largest size <= `size` (down to `minSize`) at which text fits maxW.
inline float fitSize(SDL_Renderer* r, const std::string& s, float maxW, float size, float minSize,
                     AppFont::Face face)
{
    const float w = AppFont::measureWidth(r, s, size, face);
    if (w <= maxW || w <= 0.0f) return size;
    return std::max(minSize, size * maxW / w);
}

inline std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Text in [x, x+availW]: fits -> plain; too long + active -> marquee scroll in a
// clip band; too long + inactive -> ellipsis.
inline void marquee(SDL_Renderer* r, const std::string& text, float x, float y, float availW,
                    float size, SDL_Color color, bool active,
                    AppFont::Face face = AppFont::Face::Body, float bandH = 0.0f)
{
    const float w = AppFont::measureWidth(r, text, size, face);
    if (w <= availW) {
        AppFont::draw(r, text, x, y, size, color, face);
        return;
    }
    if (!active) {
        AppFont::draw(r, ellipsize(r, text, availW, size, face), x, y, size, color, face);
        return;
    }
    const float gap = size * 2.4f;
    const float span = w + gap;
    // Hold at the start for a beat each cycle, then glide.
    const float period = span / 90.0f + 1.4f;
    const float t = std::fmod(clock(), period);
    const float off = t < 1.4f ? 0.0f : (t - 1.4f) * 90.0f;
    const float h = bandH > 0.0f ? bandH : size * 1.6f;
    SDL_Rect clip{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y - size * 0.2f)),
                  static_cast<int>(std::lround(availW)), static_cast<int>(std::lround(h))};
    SDL_RenderSetClipRect(r, &clip);
    AppFont::draw(r, text, x - off, y, size, color, face);
    AppFont::draw(r, text, x - off + span, y, size, color, face);
    SDL_RenderSetClipRect(r, nullptr);
}

inline void rowTitle(SDL_Renderer* r, const std::string& text, float x, float y,
                     float availW, float size, SDL_Color color, bool active,
                     float bandH = 56.0f)
{
    marquee(r, text, x, y, availW, size, color, active, AppFont::Face::Body, bandH);
}

// --- surfaces ----------------------------------------------------------------

// Page ground: near-black ink with two slow-drifting colored light pools.
inline void background(SDL_Renderer* r, int w, int h)
{
    Gfx::vGradient(r, {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, BgTop, BgBottom);
}

inline void backgroundAnimated(SDL_Renderer* r, int w, int h, float time)
{
    background(r, w, h);
    const float dx = std::sin(time * 0.11f) * 60.0f;
    const float dy = std::cos(time * 0.09f) * 50.0f;
    Gfx::glow(r, 60.0f + dx, 120.0f + dy, 560.0f, alpha(Accent, 70));
    Gfx::glow(r, w - 40.0f - dx, h - 260.0f - dy, 640.0f, alpha(Accent2, 78));
}

// Header block: tracked caption (section) over a big display title, left
// aligned on the margin. Content starts at y ~200.
inline void header(SDL_Renderer* r, int w, const std::string& title, const std::string& subtitle)
{
    const std::string big = subtitle.empty() ? title : subtitle;
    if (!subtitle.empty()) {
        tracked(r, upper(title), kMargin, 44.0f, Type::Caption, accent());
    }
    const float maxW = w - kMargin * 2.0f - 120.0f; // leave room for a counter
    const float size = fitSize(r, upper(big), maxW, Type::Hero, Type::Heading, AppFont::Face::Display);
    AppFont::draw(r, ellipsize(r, upper(big), maxW, size, AppFont::Face::Display),
        kMargin, 76.0f + (Type::Hero - size) * 0.5f, size, Text, AppFont::Face::Display);
}

// Right-aligned "3 / 48" position counter on the header's caption row.
inline void counter(SDL_Renderer* r, int w, int index, int total)
{
    if (total <= 0) return;
    trackedRight(r, std::to_string(index) + " / " + std::to_string(total),
        w - kMargin, 44.0f, Type::Caption, Faint);
}

// Selectable list row. Idle rows are a whisper of surface; the focused row is
// lit: soft gold glow, gold hairline border, warm gradient.
inline void rowCard(SDL_Renderer* r, const FRect& rect, bool active)
{
    if (active) {
        Gfx::softRect(r, rect, 20.0f, 18.0f, {245, 214, 95, 46}, true);
        Gfx::panel(r, rect, 20.0f, {245, 214, 95, 44}, {245, 214, 95, 12}, {245, 214, 95, 230}, 2.0f);
    } else {
        Gfx::roundRect(r, rect, 20.0f, {255, 255, 255, 9});
    }
}

inline void card(SDL_Renderer* r, const FRect& rect, bool active) { rowCard(r, rect, active); }

// Soft outer glow (kept for callers like the search keyboard).
inline void glowFrame(SDL_Renderer* r, const FRect& rect, SDL_Color c, int layers)
{
    Gfx::softRect(r, rect, 10.0f, 6.0f + layers * 3.0f, c, true);
}

inline void roundedRect(SDL_Renderer* r, const FRect& rect, float radius, SDL_Color c)
{
    Gfx::roundRect(r, rect, radius, c);
}

inline void vGradient(SDL_Renderer* r, const FRect& rect, SDL_Color top, SDL_Color bottom)
{
    Gfx::vGradient(r, rect, top, bottom);
}

inline void fillDisc(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color c)
{
    Gfx::disc(r, cx, cy, radius, c);
}

inline void circleOutline(SDL_Renderer* r, float cx, float cy, float radius, float thick, SDL_Color c)
{
    Gfx::ring(r, cx, cy, radius, thick, c);
}

// Bottom-anchored rounded spectrum bar.
inline void gradientBar(SDL_Renderer* r, float x, float baseY, float w, float height,
                        SDL_Color bottom, SDL_Color top)
{
    if (height <= 0.0f) return;
    Gfx::roundRect(r, {x, baseY - height, w, height}, std::min(w * 0.5f, 4.0f), mix(bottom, top, 0.35f));
}

// A stable brand-adjacent color for a label (FNV-1a over three anchors).
inline SDL_Color badgeColor(const std::string& text)
{
    unsigned h = 2166136261u;
    for (char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    static const SDL_Color anchors[4] = {
        {43, 199, 184, 255}, {124, 92, 232, 255}, {245, 214, 95, 255}, {239, 92, 120, 255},
    };
    const int i = static_cast<int>(h % 4u);
    const float t = static_cast<float>((h >> 3) & 0xFFu) / 255.0f * 0.5f;
    return mix(anchors[i], anchors[(i + 1) % 4], t);
}

// Rounded-square monogram thumbnail: diagonal-ish gradient tile + initial.
// (cx, cy) is its center and `radius` half its side, matching the old disc API.
inline void monogram(SDL_Renderer* r, float cx, float cy, float radius,
                     const std::string& label, SDL_Color disc)
{
    // One tinted shared shape (no per-color bake, no layered pieces).
    const float s = radius * 2.0f;
    Gfx::roundRect(r, {cx - radius, cy - radius, s, s}, radius * 0.34f, mix(disc, {12, 14, 24, 255}, 0.38f));
    const char first = label.empty()
        ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    AppFont::drawCentered(r, std::string(1, first), cx, cy - radius * 0.62f, radius * 1.25f,
        {255, 255, 255, 235}, AppFont::Face::Display);
}

// Pill chip with a centered label; returns its width. `x` is the left edge.
inline float chip(SDL_Renderer* r, float x, float y, const std::string& label, SDL_Color fill,
                  SDL_Color ink, float height = 36.0f)
{
    const float size = height * 0.5f;
    const float tw = trackedWidth(r, label, size, 1.5f);
    const float w = std::max(height, tw + height * 0.7f);
    Gfx::roundRect(r, {x, y, w, height}, height * 0.5f, fill);
    tracked(r, label, x + (w - tw) * 0.5f, y + (height - size) * 0.5f - size * 0.12f, size, ink, 1.5f);
    return w;
}

inline float chipWidth(SDL_Renderer* r, const std::string& label, float height = 36.0f)
{
    return std::max(height, trackedWidth(r, label, height * 0.5f, 1.5f) + height * 0.7f);
}

// The LIVE tag and similar (centered on cx).
inline void badge(SDL_Renderer* r, float cx, float y, const std::string& label,
                  float textSize, SDL_Color fill, SDL_Color textColor)
{
    const float h = textSize * 1.6f;
    const float w = chipWidth(r, label, h);
    chip(r, cx - w * 0.5f, y, label, fill, textColor, h);
}

// Centered glowing display title (dialogs).
inline void glowTitle(SDL_Renderer* r, const std::string& text, float cx, float y,
                      float size, SDL_Color glow, AppFont::Face face)
{
    const float w = AppFont::measureWidth(r, text, size, face);
    Gfx::glow(r, cx, y + size * 0.5f, std::max(w, size) * 0.7f, alpha(glow, 60));
    AppFont::drawCentered(r, text, cx, y, size, Text, face);
}

// --- footer legend -----------------------------------------------------------

// Button glyph for a legend key: arrows become drawn triangles (the font atlas
// is ASCII-only), everything else a text chip. Returns the chip width.
inline float keyChip(SDL_Renderer* r, float x, float y, const std::string& key, bool draw)
{
    const float h = 36.0f;
    const SDL_Color neutral{255, 255, 255, 30};
    auto arrows = [&](int n, const double* angles) {
        const float w = h * 0.7f + n * 16.0f + (n - 1) * 6.0f;
        if (draw) {
            Gfx::roundRect(r, {x, y, w, h}, h * 0.5f, neutral);
            float ax = x + h * 0.35f;
            for (int i = 0; i < n; ++i) {
                Gfx::triangle(r, {ax, y + 10.0f, 16.0f, 16.0f}, angles[i], Text);
                ax += 22.0f;
            }
        }
        return w;
    };
    if (key == "ARROWS") {
        static const double a[4] = {180.0, 0.0, 270.0, 90.0};
        return arrows(4, a);
    }
    if (key == "LEFT/RIGHT") {
        static const double a[2] = {180.0, 0.0};
        return arrows(2, a);
    }
    if (key == "UP/DOWN") {
        static const double a[2] = {270.0, 90.0};
        return arrows(2, a);
    }
    SDL_Color fill = neutral, ink = Text;
    if (key == "START" || key == "A") { fill = accent(); ink = onAccent(accent()); }
    if (key == "Y") { fill = Gold; ink = onAccent(Gold); }
    if (!draw) return chipWidth(r, key, h);
    return chip(r, x, y, key, fill, ink, h);
}

// Parse "KEY words   KEY words" hint lines into chip + label pairs and draw a
// pinned, centered legend bar. Keeps the old two-line signature.
inline void footerHints(SDL_Renderer* r, int w, const std::string& line1, const std::string& line2)
{
    struct Item { std::string key, label; };
    std::vector<Item> items;
    auto parse = [&](const std::string& line) {
        std::size_t i = 0;
        while (i < line.size()) {
            std::size_t end = line.find("   ", i);
            if (end == std::string::npos) end = line.size();
            std::string seg = line.substr(i, end - i);
            i = end;
            while (i < line.size() && line[i] == ' ') ++i;
            const std::size_t f = seg.find_first_not_of(' ');
            if (f == std::string::npos) continue;
            seg = seg.substr(f);
            const std::size_t sp = seg.find(' ');
            Item it;
            it.key = seg.substr(0, sp);
            std::string label = sp == std::string::npos ? "" : seg.substr(sp + 1);
            for (std::size_t k = 0; k < label.size(); ++k) {
                label[k] = static_cast<char>(k == 0 ? std::toupper(static_cast<unsigned char>(label[k]))
                                                    : std::tolower(static_cast<unsigned char>(label[k])));
            }
            it.label = label;
            items.push_back(it);
        }
    };
    parse(line1);
    parse(line2);

    Gfx::rect(r, {0.0f, kFooterTop, static_cast<float>(w), 1280.0f - kFooterTop}, {6, 8, 14, 200});
    Gfx::rect(r, {0.0f, kFooterTop, static_cast<float>(w), 1.0f}, Hairline);
    if (items.empty()) return;

    float size = Type::Caption;
    const float chipGap = 10.0f;
    float gap = 28.0f;
    auto total = [&]() {
        float t = 0.0f;
        for (const Item& it : items) {
            t += keyChip(r, 0, 0, it.key, false);
            if (!it.label.empty()) t += chipGap + AppFont::measureWidth(r, it.label, size);
        }
        return t + gap * (items.size() - 1);
    };
    float tw = total();
    const float maxW = w - kMargin;
    while (tw > maxW && gap > 14.0f) { gap -= 4.0f; tw = total(); }
    while (tw > maxW && size > 16.0f) { size -= 1.0f; tw = total(); }

    float x = (w - tw) * 0.5f;
    const float cy = kFooterTop + (1280.0f - kFooterTop) * 0.5f;
    for (const Item& it : items) {
        x += keyChip(r, x, cy - 18.0f, it.key, true);
        if (!it.label.empty()) {
            x += chipGap;
            AppFont::draw(r, it.label, x, cy - size * 0.62f, size, Muted);
            x += AppFont::measureWidth(r, it.label, size);
        }
        x += gap;
    }
}

// --- dialog ------------------------------------------------------------------

inline void confirmDialog(SDL_Renderer* r, int w, int h, const std::string& question,
                          const std::string& cancelLabel, const std::string& confirmLabel,
                          int selected)
{
    const float cx = w * 0.5f;
    Gfx::rect(r, {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, {4, 6, 12, 200});

    const float pw = w - 2.0f * kMargin - 32.0f;
    const float ph = 440.0f;
    const float px = cx - pw * 0.5f;
    const float py = (h - ph) * 0.5f - 40.0f;
    Gfx::softRect(r, {px, py, pw, ph}, 32.0f, 40.0f, {0, 0, 0, 200}, false);
    Gfx::panel(r, {px, py, pw, ph}, 32.0f, {30, 36, 58, 255}, {18, 22, 38, 255}, {255, 255, 255, 26}, 1.0f);

    AppFont::drawCentered(r, question, cx, py + 56.0f, Type::Title, Text, AppFont::Face::Display);

    const float btnW = pw - 96.0f;
    const float btnX = cx - btnW * 0.5f;
    const float btnH = 88.0f;
    const float b0y = py + 172.0f;
    const float b1y = b0y + btnH + 24.0f;

    const bool cancelSel = (selected == 0);
    if (cancelSel) {
        Gfx::softRect(r, {btnX, b0y, btnW, btnH}, 44.0f, 16.0f, alpha(accent(), 70), true);
        Gfx::roundRect(r, {btnX, b0y, btnW, btnH}, 44.0f, accent());
    } else {
        Gfx::roundRect(r, {btnX, b0y, btnW, btnH}, 44.0f, {255, 255, 255, 18});
    }
    AppFont::drawCentered(r, cancelLabel, cx, b0y + 22.0f, Type::Heading,
        cancelSel ? onAccent(accent()) : TextDim, AppFont::Face::Display);

    const bool confirmSel = (selected == 1);
    if (confirmSel) {
        Gfx::softRect(r, {btnX, b1y, btnW, btnH}, 44.0f, 16.0f, alpha(Live, 80), true);
        Gfx::roundRect(r, {btnX, b1y, btnW, btnH}, 44.0f, Live);
    } else {
        Gfx::roundRect(r, {btnX, b1y, btnW, btnH}, 44.0f, {255, 255, 255, 18});
    }
    AppFont::drawCentered(r, confirmLabel, cx, b1y + 22.0f, Type::Heading,
        confirmSel ? SDL_Color{255, 240, 240, 255} : TextDim, AppFont::Face::Display);
}

// --- icons -------------------------------------------------------------------

enum class Icon { Radio, Search, Heart, Folder, Note, Power, Clock, Gear, Gamepad };

// Arc from a0 to a1 (radians) as short round-capped segments.
inline void arc(SDL_Renderer* r, float cx, float cy, float radius, float a0, float a1,
                float thick, SDL_Color c)
{
    const int n = std::max(4, static_cast<int>(std::fabs(a1 - a0) * radius / 8.0f));
    float px = cx + std::cos(a0) * radius, py = cy + std::sin(a0) * radius;
    for (int i = 1; i <= n; ++i) {
        const float a = a0 + (a1 - a0) * i / n;
        const float nx = cx + std::cos(a) * radius, ny = cy + std::sin(a) * radius;
        Gfx::line(r, px, py, nx, ny, thick, c);
        px = nx;
        py = ny;
    }
}

// Icons drawn from AA primitives inside a size x size box at (x, y).
inline void icon(SDL_Renderer* r, Icon kind, float x, float y, float s, SDL_Color c)
{
    const float cx = x + s * 0.5f;
    const float cy = y + s * 0.5f;
    const float t = std::max(2.5f, s * 0.085f);
    const float pi = 3.14159265f;
    switch (kind) {
        case Icon::Radio:
            Gfx::disc(r, cx, cy, s * 0.09f, c);
            arc(r, cx, cy, s * 0.24f, -pi * 0.28f, pi * 0.28f, t, c);
            arc(r, cx, cy, s * 0.24f, pi * 0.72f, pi * 1.28f, t, c);
            arc(r, cx, cy, s * 0.42f, -pi * 0.26f, pi * 0.26f, t, c);
            arc(r, cx, cy, s * 0.42f, pi * 0.74f, pi * 1.26f, t, c);
            break;
        case Icon::Search:
            Gfx::ring(r, x + s * 0.43f, y + s * 0.43f, s * 0.27f, t, c);
            Gfx::line(r, x + s * 0.64f, y + s * 0.64f, x + s * 0.86f, y + s * 0.86f, t * 1.15f, c);
            break;
        case Icon::Heart:
            Gfx::heart(r, cx, cy, s * 0.86f, c);
            break;
        case Icon::Folder:
            Gfx::roundRect(r, {x + s * 0.08f, y + s * 0.20f, s * 0.40f, s * 0.20f}, s * 0.06f, c);
            Gfx::roundRect(r, {x + s * 0.08f, y + s * 0.30f, s * 0.84f, s * 0.52f}, s * 0.08f, c);
            break;
        case Icon::Note:
            Gfx::disc(r, x + s * 0.34f, y + s * 0.72f, s * 0.15f, c);
            Gfx::line(r, x + s * 0.47f, y + s * 0.70f, x + s * 0.47f, y + s * 0.16f, t, c);
            Gfx::line(r, x + s * 0.47f, y + s * 0.16f, x + s * 0.74f, y + s * 0.26f, t, c);
            break;
        case Icon::Power:
            arc(r, cx, cy + s * 0.04f, s * 0.32f, -pi * 0.30f, pi * 1.30f, t, c);
            Gfx::line(r, cx, y + s * 0.10f, cx, y + s * 0.44f, t, c);
            break;
        case Icon::Clock:
            Gfx::ring(r, cx, cy, s * 0.38f, t, c);
            Gfx::line(r, cx, cy, cx, cy - s * 0.24f, t, c);
            Gfx::line(r, cx, cy, cx + s * 0.17f, cy + s * 0.08f, t, c);
            break;
        case Icon::Gear: {
            const SDL_Color hole{22, 26, 40, 255};
            for (int k = 0; k < 8; ++k) {
                const float a = k * pi * 0.25f;
                Gfx::line(r, cx + std::cos(a) * s * 0.26f, cy + std::sin(a) * s * 0.26f,
                          cx + std::cos(a) * s * 0.42f, cy + std::sin(a) * s * 0.42f, s * 0.15f, c);
            }
            Gfx::disc(r, cx, cy, s * 0.30f, c);
            Gfx::disc(r, cx, cy, s * 0.12f, hole);
            break;
        }
        case Icon::Gamepad: {
            const SDL_Color ink{22, 26, 40, 255};
            Gfx::roundRect(r, {x + s * 0.04f, y + s * 0.26f, s * 0.92f, s * 0.42f}, s * 0.20f, c);
            Gfx::disc(r, x + s * 0.24f, y + s * 0.64f, s * 0.17f, c);
            Gfx::disc(r, x + s * 0.76f, y + s * 0.64f, s * 0.17f, c);
            const float d = s * 0.075f;
            Gfx::line(r, x + s * 0.17f, y + s * 0.47f, x + s * 0.37f, y + s * 0.47f, d, ink);
            Gfx::line(r, x + s * 0.27f, y + s * 0.37f, x + s * 0.27f, y + s * 0.57f, d, ink);
            Gfx::disc(r, x + s * 0.67f, y + s * 0.43f, s * 0.055f, ink);
            Gfx::disc(r, x + s * 0.79f, y + s * 0.53f, s * 0.055f, ink);
            break;
        }
    }
}

// --- list rows -------------------------------------------------------------

// Shared list geometry: 8 rows of 96px cards on a 104px stride from y 208,
// ending above the mini-player at y 1072.
constexpr int kListRows = 8;
constexpr float kListTop = 208.0f;
constexpr float kListStride = 104.0f;

// Scroll window so `sel` stays centered-ish within `rows` visible rows.
inline int listTop(int sel, int count, int rows)
{
    int top = sel - rows / 2;
    return std::max(0, std::min(top, std::max(0, count - rows)));
}

// One two-line list row: monogram thumb, title (marquee when focused),
// subtitle, and an optional right-hand value ("ON" / "OFF" render as a chip).
inline void listRow(SDL_Renderer* r, int w, float y, bool active, const std::string& badge,
                    const std::string& title, const std::string& sub,
                    const std::string& right = std::string(), bool starred = false)
{
    const FRect rc{32.0f, y, w - 64.0f, kListStride - 8.0f};
    rowCard(r, rc, active);

    float textX = rc.x + 28.0f;
    if (!badge.empty()) {
        const float cx = rc.x + 24.0f + 32.0f;
        const float cy = y + rc.h * 0.5f;
        monogram(r, cx, cy, 32.0f, badge, badgeColor(badge));
        if (starred) {
            Gfx::disc(r, cx + 26.0f, cy + 26.0f, 13.0f, {20, 22, 34, 255});
            Gfx::heart(r, cx + 26.0f, cy + 26.0f, 16.0f, Rose);
        }
        textX = rc.x + 24.0f + 64.0f + 24.0f;
    }

    float rightEdge = rc.x + rc.w - 24.0f;
    if (!right.empty()) {
        if (right == "ON" || right == "OFF") {
            const bool on = right == "ON";
            const float cw = chipWidth(r, right, 34.0f);
            chip(r, rightEdge - cw, y + rc.h * 0.5f - 17.0f, right,
                on ? accent() : SDL_Color{255, 255, 255, 26}, on ? onAccent(accent()) : Muted, 34.0f);
            rightEdge -= cw + 16.0f;
        } else {
            const float rw = trackedWidth(r, right, Type::Caption, 1.5f);
            tracked(r, right, rightEdge - rw, y + rc.h * 0.5f - 12.0f, Type::Caption, Faint, 1.5f);
            rightEdge -= rw + 20.0f;
        }
    }

    const float availW = rightEdge - textX;
    const SDL_Color tc = active ? Text : TextDim;
    if (sub.empty()) {
        marquee(r, title, textX, y + rc.h * 0.5f - 20.0f, availW, Type::Body, tc, active);
    } else {
        marquee(r, title, textX, y + 14.0f, availW, Type::Body, tc, active);
        AppFont::draw(r, ellipsize(r, sub, availW, Type::Caption), textX, y + 56.0f, Type::Caption,
            active ? Muted : Faint);
    }
}

// Centered empty / loading state: a big line and a hint below it.
inline void emptyState(SDL_Renderer* r, int w, const std::string& big, const std::string& hint,
                       SDL_Color bigColor = Text)
{
    AppFont::drawCentered(r, big, w * 0.5f, 520.0f, Type::Heading, bigColor, AppFont::Face::Display);
    if (!hint.empty()) {
        AppFont::drawCentered(r, hint, w * 0.5f, 584.0f, Type::Small, Muted);
    }
}

// Tiny animated equalizer glyph (mini-player, hero card, playing row).
inline void miniEq(SDL_Renderer* r, float x, float baseY, int bars, float h, SDL_Color c, bool live)
{
    for (int i = 0; i < bars; ++i) {
        const float ph = clock() * (5.2f + i * 0.9f) + i * 1.7f;
        const float v = live ? 0.35f + 0.65f * (0.5f + 0.5f * std::sin(ph)) : 0.3f;
        const float bh = std::max(4.0f, h * v);
        Gfx::roundRect(r, {x + i * 9.0f, baseY - bh, 5.0f, bh}, 2.5f, c);
    }
}

} // namespace Theme
