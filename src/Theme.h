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

// Two looks behind one Theme:: surface, chosen at startup from settings
// ("theme" = neon | attract) and fixed for the life of the process (changing
// it restarts the menu). Each look's code lives in its own namespace below,
// unchanged in shape: Neon is Retro Launcher's own, Attract is
// sdk/themes/attract with Retro Launcher's centered header, counter and four
// icons. The app calls the forwarding functions at the end of this file, and
// reads the tokens (colors, type sizes) as runtime values set by setStyle().
// Geometry is shared and checked equal at compile time.

enum class Style { Neon, Attract };
Style style();
void setStyle(Style s);
Style styleNamed(const std::string& name);   // "attract" -> Attract, anything else Neon
const char* styleName(Style s);

// In a namespace of its own so argument-dependent lookup on an Icon finds no
// Theme:: forwarding function (it would clash with each look's own icon()).
namespace icons { enum class Icon { Radio, Search, Heart, Folder, Note, Power, Video, YouTube, Clock, Gear, Gamepad, Joystick }; }
using icons::Icon;

extern SDL_Color Ink, BgTop, BgBottom, HeaderL, HeaderR, Accent, Accent2, Gold, Rose, Live,
    Text, TextDim, Muted, Faint, Card, CardHi, Hairline;
namespace Type { extern float Hero, Title, Heading, Body, Small, Caption; }

constexpr float kMargin = 48.0f;
constexpr float kFooterTop = 1176.0f;
constexpr float kMiniTop = 1072.0f;
constexpr int kListRows = 8;
constexpr float kListTop = 208.0f;
constexpr float kListStride = 104.0f;

// ============================================================== Neon
namespace neon {


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
    // Centered: the small caption, then the big title.
    const std::string big = subtitle.empty() ? title : subtitle;
    if (!subtitle.empty()) {
        trackedCentered(r, upper(title), w * 0.5f, 44.0f, Type::Caption, accent());
    }
    const float maxW = w - kMargin * 2.0f;
    const float size = fitSize(r, upper(big), maxW, Type::Hero, Type::Heading, AppFont::Face::Display);
    AppFont::drawCentered(r, ellipsize(r, upper(big), maxW, size, AppFont::Face::Display),
        w * 0.5f, 76.0f + (Type::Hero - size) * 0.5f, size, Text, AppFont::Face::Display);
}
// "3 / 18", centered under the title, above the list.
inline void counter(SDL_Renderer* r, int w, int index, int total)
{
    if (total <= 0) return;
    trackedCentered(r, std::to_string(index) + " / " + std::to_string(total),
        w * 0.5f, 172.0f, Type::Caption, Faint);
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
        case Icon::Joystick: {
            const SDL_Color ink{22, 26, 40, 255};
            Gfx::line(r, cx, y + s * 0.30f, cx, y + s * 0.70f, s * 0.09f, c);       // stick
            Gfx::disc(r, cx, y + s * 0.24f, s * 0.15f, c);                         // ball top
            Gfx::roundRect(r, {x + s * 0.10f, y + s * 0.66f, s * 0.80f, s * 0.22f}, s * 0.08f, c);  // base
            Gfx::disc(r, x + s * 0.24f, y + s * 0.77f, s * 0.045f, ink);
            Gfx::disc(r, x + s * 0.76f, y + s * 0.77f, s * 0.045f, ink);
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


// --- screen building blocks (Retro Launcher) ----------------------------------
// The shapes Retro Launcher's own screens draw (dialogs, popups, toasts, keys),
// so each look can draw them its own way. Neon's are the screens' original calls.

inline void shadowedPanel(SDL_Renderer* r, const FRect& rc, float radius, float spread, SDL_Color shadow,
                          SDL_Color top, SDL_Color bottom, SDL_Color border, float bw)
{
    Gfx::softRect(r, rc, radius, spread, shadow, false);
    Gfx::panel(r, rc, radius, top, bottom, border, bw);
}

inline void framedPanel(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color top, SDL_Color bottom,
                        SDL_Color border, float bw)
{
    Gfx::panel(r, rc, radius, top, bottom, border, bw);
}

inline void pill(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color c) { Gfx::roundRect(r, rc, radius, c); }

inline void button(SDL_Renderer* r, const FRect& rc, float radius, bool selected, SDL_Color on, SDL_Color off,
                   float glowSpread, int glowAlpha)
{
    if (selected) {
        Gfx::softRect(r, rc, radius, glowSpread, alpha(on, glowAlpha), true);
        Gfx::roundRect(r, rc, radius, on);
    } else {
        Gfx::roundRect(r, rc, radius, off);
    }
}

// Text on a button: the screen's own colors.
inline SDL_Color buttonText(bool selected, SDL_Color /*fill*/, SDL_Color selectedColor, SDL_Color idleColor)
{
    return selected ? selectedColor : idleColor;
}

inline void topRule(SDL_Renderer* r, const FRect& rc) { Gfx::hGradient(r, rc, Accent, Accent2); }

// Text set in the display face: as given.
inline std::string displayText(const std::string& s) { return s; }
} // namespace neon

// ============================================================== Attract
namespace attract {


// --- tokens ------------------------------------------------------------------

constexpr SDL_Color Ink       {4, 4, 10, 255};      // page ground
constexpr SDL_Color BgTop     {8, 8, 20, 255};
constexpr SDL_Color BgBottom  {4, 4, 10, 255};
constexpr SDL_Color HeaderL   {61, 245, 255, 255};
constexpr SDL_Color HeaderR   {255, 95, 210, 255};
constexpr SDL_Color Accent    {61, 245, 255, 255};  // phosphor cyan
constexpr SDL_Color Accent2   {255, 95, 210, 255};  // hot pink
constexpr SDL_Color Gold      {255, 225, 77, 255};  // coin-op yellow (focus)
constexpr SDL_Color Rose      {229, 36, 75, 255};   // cabinet red
constexpr SDL_Color Live      {229, 36, 75, 255};
constexpr SDL_Color Text      {236, 240, 255, 255};
constexpr SDL_Color TextDim   {159, 179, 255, 255};
constexpr SDL_Color Muted     {120, 136, 200, 255};
constexpr SDL_Color Faint     {78, 88, 140, 255};
constexpr SDL_Color Card      {61, 245, 255, 14};   // box fill
constexpr SDL_Color CardHi    {61, 245, 255, 30};
constexpr SDL_Color Hairline  {61, 245, 255, 70};   // box border

namespace Type {
constexpr float Hero = 56.0f;     // Press Start 2P: ~1em per glyph
constexpr float Title = 44.0f;
constexpr float Heading = 30.0f;
constexpr float Body = 34.0f;     // VT323 sets small; run it larger
constexpr float Small = 28.0f;
constexpr float Caption = 24.0f;
} // namespace Type

constexpr float kMargin = 48.0f;
constexpr float kFooterTop = 1176.0f;
constexpr float kMiniTop = 1072.0f;

constexpr float kShadow = 6.0f;   // hard offset drop shadow

inline SDL_Color mix(SDL_Color a, SDL_Color b, float t) { return Gfx::mix(a, b, t); }
inline SDL_Color alpha(SDL_Color c, int a) { c.a = static_cast<Uint8>(std::max(0, std::min(255, a))); return c; }

inline float& clock()
{
    static float t = 0.0f;
    return t;
}

inline SDL_Color& accent()
{
    static SDL_Color c = Accent;
    return c;
}

// Readable ink on an accent fill: near-black on bright colors, white on dark.
inline SDL_Color onAccent(SDL_Color c)
{
    const float lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    return lum > 140.0f ? mix(c, {4, 4, 10, 255}, 0.88f) : SDL_Color{255, 255, 255, 255};
}

// On for ~0.55s of every 0.9s: the attract-mode blink.
inline bool blinkOn() { return std::fmod(clock(), 0.9f) < 0.55f; }

// --- text helpers ------------------------------------------------------------

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

inline float fitSize(SDL_Renderer* r, const std::string& s, float maxW, float size, float minSize,
                     AppFont::Face face)
{
    const float w = AppFont::measureWidth(r, s, size, face);
    if (w <= maxW || w <= 0.0f) return size;
    // 3% short of the exact fit: glyph advances round up at the new size, and a
    // pixel over makes ellipsize() cut the title ("SUPER NINT...").
    return std::max(minSize, size * maxW / w * 0.97f);
}

inline std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

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

// Square box: hard offset shadow, fill, inset border. The Attract primitive.
inline void box(SDL_Renderer* r, const FRect& rc, SDL_Color fill, SDL_Color border, float bw,
                bool shadow = true, SDL_Color shadowColor = {0, 0, 0, 220})
{
    if (shadow) Gfx::rect(r, {rc.x + kShadow, rc.y + kShadow, rc.w, rc.h}, shadowColor);
    Gfx::rect(r, rc, fill);
    if (bw <= 0.0f || border.a == 0) return;
    Gfx::rect(r, {rc.x, rc.y, rc.w, bw}, border);
    Gfx::rect(r, {rc.x, rc.y + rc.h - bw, rc.w, bw}, border);
    Gfx::rect(r, {rc.x, rc.y + bw, bw, rc.h - 2.0f * bw}, border);
    Gfx::rect(r, {rc.x + rc.w - bw, rc.y + bw, bw, rc.h - 2.0f * bw}, border);
}

// Scanlines every 4px plus a dark vignette at top and bottom.
inline void scanlines(SDL_Renderer* r, int w, int h)
{
    for (int y = 0; y < h; y += 4) {
        Gfx::rect(r, {0.0f, static_cast<float>(y), static_cast<float>(w), 1.0f}, {0, 0, 0, 90});
    }
    Gfx::vGradient(r, {0.0f, 0.0f, static_cast<float>(w), 220.0f}, {0, 0, 0, 120}, {0, 0, 0, 0});
    Gfx::vGradient(r, {0.0f, h - 260.0f, static_cast<float>(w), 260.0f}, {0, 0, 0, 0}, {0, 0, 0, 140});
}

inline void background(SDL_Renderer* r, int w, int h)
{
    Gfx::vGradient(r, {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, BgTop, BgBottom);
    scanlines(r, w, h);
}

// Adds the CRT "roll": a faint bright band drifting slowly down the tube.
inline void backgroundAnimated(SDL_Renderer* r, int w, int h, float /*time*/)
{
    // Retro Launcher: no rolling CRT band (the user's call); the static scanlines stay.
    background(r, w, h);
}

// Caption in cyan over a yellow pixel title with a hard red offset shadow.
inline void header(SDL_Renderer* r, int w, const std::string& title, const std::string& subtitle)
{
    const std::string big = upper(subtitle.empty() ? title : subtitle);
    // Retro Launcher: centered, as its Neon header (caption, then the title).
    if (!subtitle.empty()) {
        trackedCentered(r, upper(title), w * 0.5f, 44.0f, Type::Caption, accent());
    }
    const float maxW = w - kMargin * 2.0f;
    const float size = fitSize(r, big, maxW, Type::Hero, Type::Heading, AppFont::Face::Display);
    const std::string s = ellipsize(r, big, maxW, size, AppFont::Face::Display);
    const float y = 92.0f + (Type::Hero - size) * 0.5f;
    AppFont::drawCentered(r, s, w * 0.5f + 4.0f, y + 4.0f, size, Rose, AppFont::Face::Display);
    AppFont::drawCentered(r, s, w * 0.5f, y, size, Gold, AppFont::Face::Display);
}

inline void counter(SDL_Renderer* r, int w, int index, int total)
{
    if (total <= 0) return;
    // Retro Launcher: centered under the title, above the list.
    trackedCentered(r, std::to_string(index) + "/" + std::to_string(total),
        w * 0.5f, 172.0f, Type::Caption, Muted, 2.0f);
}

// Idle: a cyan-bordered box. Focused: red box, yellow border, yellow shadow.
inline void rowCard(SDL_Renderer* r, const FRect& rect, bool active)
{
    if (active) {
        box(r, rect, Rose, Gold, 3.0f, true, Gold);
    } else {
        box(r, rect, Card, Hairline, 2.0f, false);
    }
}

inline void card(SDL_Renderer* r, const FRect& rect, bool active) { rowCard(r, rect, active); }

// No soft glow in this theme: a hard double border in the glow color instead.
inline void glowFrame(SDL_Renderer* r, const FRect& rect, SDL_Color c, int layers)
{
    const float g = 3.0f + layers;
    box(r, {rect.x - g, rect.y - g, rect.w + 2.0f * g, rect.h + 2.0f * g}, {0, 0, 0, 0},
        alpha(c, 255), 3.0f, false);
}

// Rounding is ignored: everything in Attract is square.
inline void roundedRect(SDL_Renderer* r, const FRect& rect, float /*radius*/, SDL_Color c)
{
    Gfx::rect(r, rect, c);
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

// LED-segment spectrum bar: stacked 8px blocks with 4px gaps, ramping from
// `bottom` to `top`.
inline void gradientBar(SDL_Renderer* r, float x, float baseY, float w, float height,
                        SDL_Color bottom, SDL_Color top)
{
    if (height <= 0.0f) return;
    const float seg = 8.0f, gap = 4.0f;
    const int n = std::max(1, static_cast<int>(height / (seg + gap)));
    for (int i = 0; i < n; ++i) {
        const float t = n > 1 ? static_cast<float>(i) / (n - 1) : 0.0f;
        const SDL_Color c = mix(bottom, top, t);
        Gfx::rect(r, {x, baseY - (i + 1) * (seg + gap) + gap, w, seg}, c);
    }
}

inline SDL_Color badgeColor(const std::string& text)
{
    unsigned h = 2166136261u;
    for (char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    static const SDL_Color anchors[4] = {
        {61, 245, 255, 255}, {255, 95, 210, 255}, {255, 225, 77, 255}, {229, 36, 75, 255},
    };
    return anchors[h % 4u];   // flat palette colors, no blending
}

// Square pixel tile with the initial; (cx, cy) center, `radius` half-side.
inline void monogram(SDL_Renderer* r, float cx, float cy, float radius,
                     const std::string& label, SDL_Color disc)
{
    const float s = radius * 2.0f;
    box(r, {cx - radius, cy - radius, s, s}, mix(disc, {4, 4, 10, 255}, 0.55f), disc, 3.0f, false);
    const char first = label.empty()
        ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    AppFont::drawCentered(r, std::string(1, first), cx, cy - radius * 0.42f, radius * 0.9f,
        disc, AppFont::Face::Display);
}

// Square chip with a centered label; returns its width.
inline float chip(SDL_Renderer* r, float x, float y, const std::string& label, SDL_Color fill,
                  SDL_Color ink, float height = 36.0f)
{
    const float size = height * 0.62f;
    const float tw = trackedWidth(r, label, size, 1.0f);
    const float w = std::max(height, tw + height * 0.6f);
    Gfx::rect(r, {x, y, w, height}, fill);
    tracked(r, label, x + (w - tw) * 0.5f, y + (height - size) * 0.5f - size * 0.08f, size, ink, 1.0f);
    return w;
}

inline float chipWidth(SDL_Renderer* r, const std::string& label, float height = 36.0f)
{
    return std::max(height, trackedWidth(r, label, height * 0.62f, 1.0f) + height * 0.6f);
}

inline void badge(SDL_Renderer* r, float cx, float y, const std::string& label,
                  float textSize, SDL_Color fill, SDL_Color textColor)
{
    const float h = textSize * 1.6f;
    const float w = chipWidth(r, label, h);
    chip(r, cx - w * 0.5f, y, label, fill, textColor, h);
}

// Dialog title: yellow on a hard offset shadow in the glow color.
inline void glowTitle(SDL_Renderer* r, const std::string& text, float cx, float y,
                      float size, SDL_Color glow, AppFont::Face face)
{
    AppFont::drawCentered(r, text, cx + 4.0f, y + 4.0f, size, glow, face);
    AppFont::drawCentered(r, text, cx, y, size, Gold, face);
}

// --- footer legend -----------------------------------------------------------

inline float keyChip(SDL_Renderer* r, float x, float y, const std::string& key, bool draw)
{
    const float h = 36.0f;
    const SDL_Color neutral{61, 245, 255, 40};
    auto arrows = [&](int n, const double* angles) {
        const float w = h * 0.7f + n * 16.0f + (n - 1) * 6.0f;
        if (draw) {
            Gfx::rect(r, {x, y, w, h}, neutral);
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
    if (key == "START" || key == "A") { fill = Rose; ink = Text; }
    if (key == "Y") { fill = Gold; ink = onAccent(Gold); }
    if (!draw) return chipWidth(r, key, h);
    return chip(r, x, y, key, fill, ink, h);
}

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
            it.label = sp == std::string::npos ? "" : upper(seg.substr(sp + 1));
            items.push_back(it);
        }
    };
    parse(line1);
    parse(line2);

    Gfx::rect(r, {0.0f, kFooterTop, static_cast<float>(w), 1280.0f - kFooterTop}, {0, 0, 0, 230});
    Gfx::rect(r, {0.0f, kFooterTop, static_cast<float>(w), 2.0f}, Accent);
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
    while (tw > maxW && size > 18.0f) { size -= 1.0f; tw = total(); }

    float x = (w - tw) * 0.5f;
    const float cy = kFooterTop + (1280.0f - kFooterTop) * 0.5f;
    for (const Item& it : items) {
        x += keyChip(r, x, cy - 18.0f, it.key, true);
        if (!it.label.empty()) {
            x += chipGap;
            AppFont::draw(r, it.label, x, cy - size * 0.55f, size, TextDim);
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
    Gfx::rect(r, {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, {0, 0, 0, 210});

    const float pw = w - 2.0f * kMargin - 32.0f;
    const float ph = 440.0f;
    const float px = cx - pw * 0.5f;
    const float py = (h - ph) * 0.5f - 40.0f;
    box(r, {px, py, pw, ph}, {10, 10, 26, 255}, Accent, 3.0f);
    box(r, {px + 9.0f, py + 9.0f, pw - 18.0f, ph - 18.0f}, {0, 0, 0, 0}, Hairline, 2.0f, false);

    const std::string q = upper(question);
    const float qs = fitSize(r, q, pw - 80.0f, Type::Heading, 18.0f, AppFont::Face::Display);
    glowTitle(r, q, cx, py + 64.0f, qs, Rose, AppFont::Face::Display);

    const float btnW = pw - 96.0f;
    const float btnX = cx - btnW * 0.5f;
    const float btnH = 88.0f;
    const float b0y = py + 172.0f;
    const float b1y = b0y + btnH + 24.0f;

    auto button = [&](float by, const std::string& label, bool sel, SDL_Color on) {
        if (sel) box(r, {btnX, by, btnW, btnH}, on, Gold, 3.0f);
        else box(r, {btnX, by, btnW, btnH}, Card, Hairline, 2.0f, false);
        const std::string l = upper(label);
        const float ls = fitSize(r, l, btnW - 64.0f, Type::Heading * 0.8f, 16.0f, AppFont::Face::Display);
        AppFont::drawCentered(r, l, cx, by + (btnH - ls) * 0.5f, ls,
            sel ? onAccent(on) : TextDim, AppFont::Face::Display);
        if (sel && blinkOn()) Gfx::triangle(r, {btnX + 20.0f, by + btnH * 0.5f - 12.0f, 20.0f, 24.0f}, 0.0, Gold);
    };
    button(b0y, cancelLabel, selected == 0, accent());
    button(b1y, confirmLabel, selected == 1, Live);
}

// --- icons -------------------------------------------------------------------


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

// Same glyph geometry as Neon, a touch heavier and squared where it can be.
inline void icon(SDL_Renderer* r, Icon kind, float x, float y, float s, SDL_Color c)
{
    const float cx = x + s * 0.5f;
    const float cy = y + s * 0.5f;
    const float t = std::max(3.0f, s * 0.10f);
    const float pi = 3.14159265f;
    switch (kind) {
        case Icon::Radio:
            Gfx::rect(r, {cx - s * 0.08f, cy - s * 0.08f, s * 0.16f, s * 0.16f}, c);
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
            Gfx::rect(r, {x + s * 0.08f, y + s * 0.20f, s * 0.40f, s * 0.16f}, c);
            Gfx::rect(r, {x + s * 0.08f, y + s * 0.32f, s * 0.84f, s * 0.50f}, c);
            break;
        case Icon::Note:
            Gfx::rect(r, {x + s * 0.20f, y + s * 0.60f, s * 0.27f, s * 0.24f}, c);
            Gfx::rect(r, {x + s * 0.42f, y + s * 0.16f, t, s * 0.56f}, c);
            Gfx::rect(r, {x + s * 0.42f, y + s * 0.16f, s * 0.32f, t}, c);
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
        case Icon::Joystick: {
            const SDL_Color ink{22, 26, 40, 255};
            Gfx::line(r, cx, y + s * 0.30f, cx, y + s * 0.70f, s * 0.09f, c);       // stick
            Gfx::disc(r, cx, y + s * 0.24f, s * 0.15f, c);                         // ball top
            Gfx::roundRect(r, {x + s * 0.10f, y + s * 0.66f, s * 0.80f, s * 0.22f}, s * 0.08f, c);  // base
            Gfx::disc(r, x + s * 0.24f, y + s * 0.77f, s * 0.045f, ink);
            Gfx::disc(r, x + s * 0.76f, y + s * 0.77f, s * 0.045f, ink);
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
        case Icon::Video: {
            const float l = x + s * 0.10f, rr = x + s * 0.90f, tp = y + s * 0.20f, bt = y + s * 0.80f;
            Gfx::rect(r, {l, tp, rr - l, t}, c);
            Gfx::rect(r, {l, bt - t, rr - l, t}, c);
            Gfx::rect(r, {l, tp, t, bt - tp}, c);
            Gfx::rect(r, {rr - t, tp, t, bt - tp}, c);
            Gfx::triangle(r, {x + s * 0.40f, y + s * 0.35f, s * 0.26f, s * 0.30f}, 0.0, c);
            break;
        }
        case Icon::YouTube:
            Gfx::rect(r, {x + s * 0.06f, y + s * 0.20f, s * 0.88f, s * 0.60f}, c);
            Gfx::triangle(r, {x + s * 0.40f, y + s * 0.35f, s * 0.26f, s * 0.30f}, 0.0, {4, 4, 10, 235});
            break;
    }
}

// --- list rows -------------------------------------------------------------

constexpr int kListRows = 8;
constexpr float kListTop = 208.0f;
constexpr float kListStride = 104.0f;

inline int listTop(int sel, int count, int rows)
{
    int top = sel - rows / 2;
    return std::max(0, std::min(top, std::max(0, count - rows)));
}

// Two-line row; the focused row gets a blinking yellow arrow in the gutter.
inline void listRow(SDL_Renderer* r, int w, float y, bool active, const std::string& badge,
                    const std::string& title, const std::string& sub,
                    const std::string& right = std::string(), bool starred = false)
{
    const FRect rc{40.0f, y, w - 80.0f, kListStride - 12.0f};
    rowCard(r, rc, active);
    if (active && blinkOn()) {
        Gfx::triangle(r, {12.0f, y + rc.h * 0.5f - 12.0f, 20.0f, 24.0f}, 0.0, Gold);
    }

    float textX = rc.x + 24.0f;
    if (!badge.empty()) {
        const float cx = rc.x + 20.0f + 30.0f;
        const float cy = y + rc.h * 0.5f;
        monogram(r, cx, cy, 30.0f, badge, active ? Gold : badgeColor(badge));
        if (starred) {
            Gfx::rect(r, {cx + 14.0f, cy + 14.0f, 26.0f, 26.0f}, {4, 4, 10, 255});
            Gfx::heart(r, cx + 27.0f, cy + 27.0f, 18.0f, active ? Gold : Rose);
        }
        textX = rc.x + 20.0f + 60.0f + 22.0f;
    }

    float rightEdge = rc.x + rc.w - 20.0f;
    if (!right.empty()) {
        if (right == "ON" || right == "OFF") {
            const bool on = right == "ON";
            const float cw = chipWidth(r, right, 34.0f);
            chip(r, rightEdge - cw, y + rc.h * 0.5f - 17.0f, right,
                on ? Gold : SDL_Color{61, 245, 255, 40}, on ? onAccent(Gold) : Muted, 34.0f);
            rightEdge -= cw + 16.0f;
        } else {
            const float rw = trackedWidth(r, right, Type::Caption, 1.5f);
            tracked(r, right, rightEdge - rw, y + rc.h * 0.5f - 12.0f, Type::Caption,
                active ? Text : Faint, 1.5f);
            rightEdge -= rw + 20.0f;
        }
    }

    const float availW = rightEdge - textX;
    const SDL_Color tc = active ? Text : TextDim;
    if (sub.empty()) {
        marquee(r, title, textX, y + rc.h * 0.5f - 20.0f, availW, Type::Body, tc, active);
    } else {
        marquee(r, title, textX, y + 10.0f, availW, Type::Body, tc, active);
        AppFont::draw(r, ellipsize(r, sub, availW, Type::Caption), textX, y + 52.0f, Type::Caption,
            active ? SDL_Color{255, 225, 200, 255} : Faint);
    }
}

// Empty state: the big line blinks like "INSERT COIN".
inline void emptyState(SDL_Renderer* r, int w, const std::string& big, const std::string& hint,
                       SDL_Color bigColor = Text)
{
    if (blinkOn()) {
        AppFont::drawCentered(r, upper(big), w * 0.5f, 520.0f, Type::Heading, bigColor, AppFont::Face::Display);
    }
    if (!hint.empty()) {
        AppFont::drawCentered(r, hint, w * 0.5f, 584.0f, Type::Small, Muted);
    }
}

// Tiny equalizer: square LED blocks.
inline void miniEq(SDL_Renderer* r, float x, float baseY, int bars, float h, SDL_Color c, bool live)
{
    for (int i = 0; i < bars; ++i) {
        const float ph = clock() * (5.2f + i * 0.9f) + i * 1.7f;
        const float v = live ? 0.35f + 0.65f * (0.5f + 0.5f * std::sin(ph)) : 0.3f;
        const int blocks = std::max(1, static_cast<int>(h * v / 5.0f));
        for (int b = 0; b < blocks; ++b) {
            Gfx::rect(r, {x + i * 9.0f, baseY - (b + 1) * 5.0f + 1.0f, 6.0f, 4.0f}, c);
        }
    }
}


// --- screen building blocks (Retro Launcher) ----------------------------------
// Attract's versions: square boxes, hard offset shadows, no blur or rounding.

inline void shadowedPanel(SDL_Renderer* r, const FRect& rc, float /*radius*/, float /*spread*/, SDL_Color shadow,
                          SDL_Color top, SDL_Color /*bottom*/, SDL_Color /*border*/, float /*bw*/)
{
    // Navy, a cyan border and an inner rule, the hard black shadow (as the confirm dialog).
    box(r, rc, {11, 11, 30, top.a}, alpha(Accent, top.a), 3.0f, true, {0, 0, 0, shadow.a});
    const float in = 8.0f;
    box(r, {rc.x + in, rc.y + in, rc.w - 2.0f * in, rc.h - 2.0f * in}, {0, 0, 0, 0}, alpha(Accent, top.a * 50 / 255), 1.0f, false);
}

inline void framedPanel(SDL_Renderer* r, const FRect& rc, float /*radius*/, SDL_Color top, SDL_Color /*bottom*/,
                        SDL_Color /*border*/, float /*bw*/)
{
    box(r, rc, {8, 8, 20, std::max<Uint8>(top.a, 200)}, alpha(Accent, 200), 2.0f, false);
}

inline void pill(SDL_Renderer* r, const FRect& rc, float /*radius*/, SDL_Color c) { Gfx::rect(r, rc, c); }

inline void button(SDL_Renderer* r, const FRect& rc, float /*radius*/, bool selected, SDL_Color on, SDL_Color /*off*/,
                   float /*glowSpread*/, int /*glowAlpha*/)
{
    if (selected) {
        box(r, rc, on, Gold, 3.0f, true, Gold);
    } else {
        box(r, rc, Card, Hairline, 2.0f, false);
    }
}

// Dark text on a light fill (cyan, yellow), light text on a dark one (red).
inline SDL_Color buttonText(bool selected, SDL_Color fill, SDL_Color /*selectedColor*/, SDL_Color /*idleColor*/)
{
    if (!selected) return TextDim;
    const int luma = (fill.r * 299 + fill.g * 587 + fill.b * 114) / 1000;
    return luma > 140 ? Ink : Text;
}

inline void topRule(SDL_Renderer* r, const FRect& rc) { Gfx::rect(r, rc, Accent); }

// Text set in the display face: upper case, as all of Attract's pixel type.
inline std::string displayText(const std::string& s) { return upper(s); }
} // namespace attract

static_assert(neon::kMargin == kMargin && attract::kMargin == kMargin, "theme geometry differs");
static_assert(neon::kFooterTop == kFooterTop && attract::kFooterTop == kFooterTop, "theme geometry differs");
static_assert(neon::kMiniTop == kMiniTop && attract::kMiniTop == kMiniTop, "theme geometry differs");
static_assert(neon::kListRows == kListRows && attract::kListRows == kListRows, "theme geometry differs");
static_assert(neon::kListTop == kListTop && attract::kListTop == kListTop, "theme geometry differs");
static_assert(neon::kListStride == kListStride && attract::kListStride == kListStride, "theme geometry differs");

// ============================================================== the active look
inline SDL_Color mix(SDL_Color a, SDL_Color b, float t) { return style() == Style::Attract ? attract::mix(a, b, t) : neon::mix(a, b, t); }
inline SDL_Color alpha(SDL_Color c, int a) { return style() == Style::Attract ? attract::alpha(c, a) : neon::alpha(c, a); }
inline float& clock() { return style() == Style::Attract ? attract::clock() : neon::clock(); }
inline SDL_Color& accent() { return style() == Style::Attract ? attract::accent() : neon::accent(); }
inline SDL_Color onAccent(SDL_Color c) { return style() == Style::Attract ? attract::onAccent(c) : neon::onAccent(c); }
inline float trackedWidth(SDL_Renderer* r, const std::string& text, float size, float spacing) { return style() == Style::Attract ? attract::trackedWidth(r, text, size, spacing) : neon::trackedWidth(r, text, size, spacing); }
inline void tracked(SDL_Renderer* r, const std::string& text, float x, float y, float size, SDL_Color c, float spacing = 3.0f) { if (style() == Style::Attract) attract::tracked(r, text, x, y, size, c, spacing); else neon::tracked(r, text, x, y, size, c, spacing); }
inline void trackedRight(SDL_Renderer* r, const std::string& text, float right, float y, float size, SDL_Color c, float spacing = 3.0f) { if (style() == Style::Attract) attract::trackedRight(r, text, right, y, size, c, spacing); else neon::trackedRight(r, text, right, y, size, c, spacing); }
inline void trackedCentered(SDL_Renderer* r, const std::string& text, float cx, float y, float size, SDL_Color c, float spacing = 3.0f) { if (style() == Style::Attract) attract::trackedCentered(r, text, cx, y, size, c, spacing); else neon::trackedCentered(r, text, cx, y, size, c, spacing); }
inline std::string ellipsize(SDL_Renderer* r, std::string s, float maxW, float size, AppFont::Face face = AppFont::Face::Body) { return style() == Style::Attract ? attract::ellipsize(r, s, maxW, size, face) : neon::ellipsize(r, s, maxW, size, face); }
inline float fitSize(SDL_Renderer* r, const std::string& s, float maxW, float size, float minSize, AppFont::Face face) { return style() == Style::Attract ? attract::fitSize(r, s, maxW, size, minSize, face) : neon::fitSize(r, s, maxW, size, minSize, face); }
inline std::string upper(std::string s) { return style() == Style::Attract ? attract::upper(s) : neon::upper(s); }
inline void marquee(SDL_Renderer* r, const std::string& text, float x, float y, float availW, float size, SDL_Color color, bool active, AppFont::Face face = AppFont::Face::Body, float bandH = 0.0f) { if (style() == Style::Attract) attract::marquee(r, text, x, y, availW, size, color, active, face, bandH); else neon::marquee(r, text, x, y, availW, size, color, active, face, bandH); }
inline void rowTitle(SDL_Renderer* r, const std::string& text, float x, float y, float availW, float size, SDL_Color color, bool active, float bandH = 56.0f) { if (style() == Style::Attract) attract::rowTitle(r, text, x, y, availW, size, color, active, bandH); else neon::rowTitle(r, text, x, y, availW, size, color, active, bandH); }
inline void background(SDL_Renderer* r, int w, int h) { if (style() == Style::Attract) attract::background(r, w, h); else neon::background(r, w, h); }
inline void backgroundAnimated(SDL_Renderer* r, int w, int h, float time) { if (style() == Style::Attract) attract::backgroundAnimated(r, w, h, time); else neon::backgroundAnimated(r, w, h, time); }
inline void header(SDL_Renderer* r, int w, const std::string& title, const std::string& subtitle) { if (style() == Style::Attract) attract::header(r, w, title, subtitle); else neon::header(r, w, title, subtitle); }
inline void counter(SDL_Renderer* r, int w, int index, int total) { if (style() == Style::Attract) attract::counter(r, w, index, total); else neon::counter(r, w, index, total); }
inline void rowCard(SDL_Renderer* r, const FRect& rect, bool active) { if (style() == Style::Attract) attract::rowCard(r, rect, active); else neon::rowCard(r, rect, active); }
inline void card(SDL_Renderer* r, const FRect& rect, bool active) { if (style() == Style::Attract) attract::card(r, rect, active); else neon::card(r, rect, active); }
inline void glowFrame(SDL_Renderer* r, const FRect& rect, SDL_Color c, int layers) { if (style() == Style::Attract) attract::glowFrame(r, rect, c, layers); else neon::glowFrame(r, rect, c, layers); }
inline void roundedRect(SDL_Renderer* r, const FRect& rect, float radius, SDL_Color c) { if (style() == Style::Attract) attract::roundedRect(r, rect, radius, c); else neon::roundedRect(r, rect, radius, c); }
inline void vGradient(SDL_Renderer* r, const FRect& rect, SDL_Color top, SDL_Color bottom) { if (style() == Style::Attract) attract::vGradient(r, rect, top, bottom); else neon::vGradient(r, rect, top, bottom); }
inline void fillDisc(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color c) { if (style() == Style::Attract) attract::fillDisc(r, cx, cy, radius, c); else neon::fillDisc(r, cx, cy, radius, c); }
inline void circleOutline(SDL_Renderer* r, float cx, float cy, float radius, float thick, SDL_Color c) { if (style() == Style::Attract) attract::circleOutline(r, cx, cy, radius, thick, c); else neon::circleOutline(r, cx, cy, radius, thick, c); }
inline void gradientBar(SDL_Renderer* r, float x, float baseY, float w, float height, SDL_Color bottom, SDL_Color top) { if (style() == Style::Attract) attract::gradientBar(r, x, baseY, w, height, bottom, top); else neon::gradientBar(r, x, baseY, w, height, bottom, top); }
inline SDL_Color badgeColor(const std::string& text) { return style() == Style::Attract ? attract::badgeColor(text) : neon::badgeColor(text); }
inline void monogram(SDL_Renderer* r, float cx, float cy, float radius, const std::string& label, SDL_Color disc) { if (style() == Style::Attract) attract::monogram(r, cx, cy, radius, label, disc); else neon::monogram(r, cx, cy, radius, label, disc); }
inline float chip(SDL_Renderer* r, float x, float y, const std::string& label, SDL_Color fill, SDL_Color ink, float height = 36.0f) { return style() == Style::Attract ? attract::chip(r, x, y, label, fill, ink, height) : neon::chip(r, x, y, label, fill, ink, height); }
inline float chipWidth(SDL_Renderer* r, const std::string& label, float height = 36.0f) { return style() == Style::Attract ? attract::chipWidth(r, label, height) : neon::chipWidth(r, label, height); }
inline void badge(SDL_Renderer* r, float cx, float y, const std::string& label, float textSize, SDL_Color fill, SDL_Color textColor) { if (style() == Style::Attract) attract::badge(r, cx, y, label, textSize, fill, textColor); else neon::badge(r, cx, y, label, textSize, fill, textColor); }
inline void glowTitle(SDL_Renderer* r, const std::string& text, float cx, float y, float size, SDL_Color glow, AppFont::Face face) { if (style() == Style::Attract) attract::glowTitle(r, text, cx, y, size, glow, face); else neon::glowTitle(r, text, cx, y, size, glow, face); }
inline float keyChip(SDL_Renderer* r, float x, float y, const std::string& key, bool draw) { return style() == Style::Attract ? attract::keyChip(r, x, y, key, draw) : neon::keyChip(r, x, y, key, draw); }
inline void footerHints(SDL_Renderer* r, int w, const std::string& line1, const std::string& line2) { if (style() == Style::Attract) attract::footerHints(r, w, line1, line2); else neon::footerHints(r, w, line1, line2); }
inline void confirmDialog(SDL_Renderer* r, int w, int h, const std::string& question, const std::string& cancelLabel, const std::string& confirmLabel, int selected) { if (style() == Style::Attract) attract::confirmDialog(r, w, h, question, cancelLabel, confirmLabel, selected); else neon::confirmDialog(r, w, h, question, cancelLabel, confirmLabel, selected); }
inline void arc(SDL_Renderer* r, float cx, float cy, float radius, float a0, float a1, float thick, SDL_Color c) { if (style() == Style::Attract) attract::arc(r, cx, cy, radius, a0, a1, thick, c); else neon::arc(r, cx, cy, radius, a0, a1, thick, c); }
inline void icon(SDL_Renderer* r, Icon kind, float x, float y, float s, SDL_Color c) { if (style() == Style::Attract) attract::icon(r, kind, x, y, s, c); else neon::icon(r, kind, x, y, s, c); }
inline int listTop(int sel, int count, int rows) { return style() == Style::Attract ? attract::listTop(sel, count, rows) : neon::listTop(sel, count, rows); }
inline void emptyState(SDL_Renderer* r, int w, const std::string& big, const std::string& hint, SDL_Color bigColor = Text) { if (style() == Style::Attract) attract::emptyState(r, w, big, hint, bigColor); else neon::emptyState(r, w, big, hint, bigColor); }
inline void miniEq(SDL_Renderer* r, float x, float baseY, int bars, float h, SDL_Color c, bool live) { if (style() == Style::Attract) attract::miniEq(r, x, baseY, bars, h, c, live); else neon::miniEq(r, x, baseY, bars, h, c, live); }


inline void shadowedPanel(SDL_Renderer* r, const FRect& rc, float radius, float spread, SDL_Color shadow, SDL_Color top, SDL_Color bottom, SDL_Color border, float bw) { if (style() == Style::Attract) attract::shadowedPanel(r, rc, radius, spread, shadow, top, bottom, border, bw); else neon::shadowedPanel(r, rc, radius, spread, shadow, top, bottom, border, bw); }
inline void framedPanel(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color top, SDL_Color bottom, SDL_Color border, float bw) { if (style() == Style::Attract) attract::framedPanel(r, rc, radius, top, bottom, border, bw); else neon::framedPanel(r, rc, radius, top, bottom, border, bw); }
inline void pill(SDL_Renderer* r, const FRect& rc, float radius, SDL_Color c) { if (style() == Style::Attract) attract::pill(r, rc, radius, c); else neon::pill(r, rc, radius, c); }
inline void button(SDL_Renderer* r, const FRect& rc, float radius, bool selected, SDL_Color on, SDL_Color off, float glowSpread, int glowAlpha) { if (style() == Style::Attract) attract::button(r, rc, radius, selected, on, off, glowSpread, glowAlpha); else neon::button(r, rc, radius, selected, on, off, glowSpread, glowAlpha); }
inline SDL_Color buttonText(bool selected, SDL_Color fill, SDL_Color selectedColor, SDL_Color idleColor) { return style() == Style::Attract ? attract::buttonText(selected, fill, selectedColor, idleColor) : neon::buttonText(selected, fill, selectedColor, idleColor); }
inline void topRule(SDL_Renderer* r, const FRect& rc) { if (style() == Style::Attract) attract::topRule(r, rc); else neon::topRule(r, rc); }
inline std::string displayText(const std::string& s) { return style() == Style::Attract ? attract::displayText(s) : neon::displayText(s); }
inline bool blinkOn() { return style() == Style::Attract ? attract::blinkOn() : true; }

} // namespace Theme
